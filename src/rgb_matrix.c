/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT keypaw_rgb_matrix

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/physical_layouts.h>
#include <zmk/rgb_matrix.h>
#include <zmk/workqueue.h>

#include <zmk/events/activity_state_changed.h>

#include "rgb_matrix_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define KP_LAYOUT DT_PHANDLE(KP_RGB_NODE, physical_layout)
#define KP_NKEYS DT_PROP_LEN(KP_LAYOUT, keys)

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(keypaw_behavior_rgb_matrix) == 1,
             "RGB matrix requires exactly one enabled controller");
BUILD_ASSERT(IS_ENABLED(CONFIG_KEYPAW_BEHAVIOR_RGB_MATRIX),
             "RGB matrix requires its control behavior");
BUILD_ASSERT(
    DT_PROP_LEN(KP_RGB_NODE, mapping) == KP_LED_COUNT,
    "keypaw,rgb-matrix: 'mapping' must hold exactly one entry per LED");
BUILD_ASSERT(CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS <=
                 CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS,
             "KEYPAW_RGB_MATRIX_DURATION_MIN_MS must not exceed "
             "KEYPAW_RGB_MATRIX_DURATION_MAX_MS");

#define KP_MAP_ENTRY_ASSERT(node_id, prop, idx)                                \
  BUILD_ASSERT((DT_PROP_BY_IDX(node_id, prop, idx) & KP_RGB_NO_KEY_XY_FLAG) || \
                   DT_PROP_BY_IDX(node_id, prop, idx) < KP_NKEYS,              \
               "keypaw,rgb-matrix: invalid mapping entry");
DT_FOREACH_PROP_ELEM(KP_RGB_NODE, mapping, KP_MAP_ENTRY_ASSERT);

static const uint32_t kp_map[KP_LED_COUNT] = DT_PROP(KP_RGB_NODE, mapping);
static struct kp_rgb_coord kp_led_coords[KP_LED_COUNT];
static uint16_t kp_rgb_board_length;
static uint16_t kp_rgb_board_height;
static size_t kp_key_to_led[KP_NKEYS];

static void kp_resolve_layout(const struct zmk_physical_layout *layout) {
  uint16_t min_x = UINT16_MAX, min_y = UINT16_MAX, max_x = 0, max_y = 0;
  memset(kp_key_to_led, 0xFF, sizeof(kp_key_to_led));
  for (size_t i = 0; i < KP_LED_COUNT; i++) {
    uint32_t k = kp_map[i];
    uint32_t x, y;
    if (k & KP_RGB_NO_KEY_XY_FLAG) {
      x = (k >> 16) & KP_RGB_NO_KEY_XY_X_MASK;
      y = k & KP_RGB_NO_KEY_XY_Y_MASK;
    } else if (k < MIN((size_t)KP_NKEYS, layout->keys_len)) {
      const struct zmk_key_physical_attrs *key = &layout->keys[k];
      x = key->x + key->width / 2;
      y = key->y + key->height / 2;
      kp_key_to_led[k] = i;
    } else {
      LOG_ERR("mapping key %u out of range (layout has %u keys)", k,
              (uint32_t)layout->keys_len);
      x = 0;
      y = 0;
    }
    kp_led_coords[i] =
        (struct kp_rgb_coord){.x = (uint16_t)x, .y = (uint16_t)y};
    min_x = MIN(min_x, (uint16_t)x);
    max_x = MAX(max_x, (uint16_t)x);
    min_y = MIN(min_y, (uint16_t)y);
    max_y = MAX(max_y, (uint16_t)y);
  }
  for (size_t i = 0; i < KP_LED_COUNT; i++) {
    kp_led_coords[i].x -= min_x;
    kp_led_coords[i].y -= min_y;
  }
  kp_rgb_board_length = MAX((uint16_t)MAX(max_x - min_x, max_y - min_y), 100);
  kp_rgb_board_height = MAX((uint16_t)(max_y - min_y), 100);
}

size_t kp_rgb_led_for_position(uint32_t position) {
  return position < KP_NKEYS ? kp_key_to_led[position] : SIZE_MAX;
}

const struct kp_rgb_coord *kp_rgb_led_coord(size_t led) {
  return led < KP_LED_COUNT ? &kp_led_coords[led] : NULL;
}

static const struct device *const strip =
    COND_CODE_1(KP_RGB_HAS_STRIP, (DEVICE_DT_GET(KP_RGB_STRIP)), (NULL));
static struct led_rgb scene[KP_LED_COUNT];
static struct led_rgb scratch[KP_LED_COUNT];
static K_MUTEX_DEFINE(kp_rgb_lock);
static atomic_t kp_rgb_output_allowed;
static atomic_t kp_rgb_inhibited;
static bool kp_rgb_output_ready;

enum kp_rgb_output_kind {
  KP_RGB_OUTPUT_NONE,
  KP_RGB_OUTPUT_SCENE,
  KP_RGB_OUTPUT_BLACK,
};

static enum kp_rgb_output_kind kp_rgb_output_pending;
static uint32_t kp_rgb_output_retry_ms = 100;
static int64_t kp_rgb_output_retry_deadline_ms;
static bool kp_rgb_output_urgent_black;
static int64_t kp_rgb_next_frame_ms = INT64_MAX;
static struct k_spinlock kp_rgb_schedule_lock;
static bool kp_rgb_scene_requested;
static bool kp_rgb_pass_active;
static bool kp_rgb_pass_requested;
static bool kp_rgb_scene_dirty = true;
static bool kp_rgb_scene_animating;
static uint64_t kp_rgb_refresh_token;
static bool kp_rgb_refresh_pending;
static int kp_rgb_activity_state = ZMK_ACTIVITY_ACTIVE;

/* A half with no strip renders nothing, but the central-only overlay and split
 * machinery still runs. */
static inline bool kp_rgb_has_leds(void) { return KP_LED_COUNT > 0; }

const struct device *const kp_rgb_no_overlays[1] = {NULL};

#define KP_RGB_SCENE_EFFECT_COUNT DT_CHILD_NUM(DT_INST(0, keypaw_behavior_rgb_matrix))

static struct kp_rgb_scene_runtime kp_rgb_effect_scenes[MAX(1, KP_RGB_SCENE_EFFECT_COUNT)];
static struct kp_rgb_scene_runtime kp_rgb_overlay_scenes[MAX(1, KP_RGB_OVERLAY_COUNT)];

static struct kp_rgb_scene_runtime *kp_rgb_effect_scene(const struct device *dev) {
  return &kp_rgb_effect_scenes[kp_rgb_effect_cfg(dev)->index];
}

static struct kp_rgb_scene_runtime *kp_rgb_overlay_scene(const struct device *dev) {
  const struct kp_rgb_overlay_common_data *data = dev->data;
  return &kp_rgb_overlay_scenes[data->index];
}

static bool kp_rgb_logical_on_locked(void) {
  return kp_rgb_controller.state.user_on &&
         (!IS_ENABLED(CONFIG_KEYPAW_RGB_MATRIX_AUTO_OFF_IDLE) ||
          zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE);
}

#define KP_TRY_LOCK()                                                          \
  k_mutex_lock(&kp_rgb_lock, K_MSEC(CONFIG_KEYPAW_RGB_MATRIX_TICK_MS))

void kp_rgb_matrix_lock(void) { k_mutex_lock(&kp_rgb_lock, K_FOREVER); }
void kp_rgb_matrix_unlock(void) { k_mutex_unlock(&kp_rgb_lock); }

/* The lock is never held across flash I/O. Bound the wait so a stuck holder
 * warns instead of hanging the queue. */
#define KP_RGB_LOCK_RETRY_MS 1
#define KP_RGB_LOCK_RETRIES 8

static bool kp_rgb_matrix_lock_patiently(void) {
  for (int attempt = 0; attempt < KP_RGB_LOCK_RETRIES; attempt++) {
    if (k_mutex_lock(&kp_rgb_lock, K_MSEC(KP_RGB_LOCK_RETRY_MS)) == 0) {
      return true;
    }
  }
  return false;
}

static void kp_rgb_output_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(kp_output_work, kp_rgb_output_handler);

/* Scheduling lock held; no matrix lock is needed to request a pass. */
static int kp_rgb_schedule_locked(int64_t deadline_ms) {
  if (deadline_ms == INT64_MAX) return 0;
  int64_t delay_ms = MAX(INT64_C(0), deadline_ms - k_uptime_get());
  int err = k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &kp_output_work,
                                      delay_ms ? K_MSEC(delay_ms) : K_NO_WAIT);
  if (err < 0) {
    kp_rgb_pass_requested = true;
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &kp_output_work,
                                K_MSEC(1));
  }
  return err;
}

static void kp_rgb_request_output_pass(bool scene_changed) {
  k_spinlock_key_t key = k_spin_lock(&kp_rgb_schedule_lock);
  kp_rgb_pass_requested = true;
  kp_rgb_scene_requested |= scene_changed;
  int err = kp_rgb_pass_active ? 0 : kp_rgb_schedule_locked(k_uptime_get());
  k_spin_unlock(&kp_rgb_schedule_lock, key);
  if (err < 0) LOG_WRN("Failed to queue RGB output (%d)", err);
}

void zmk_rgb_matrix_flush(void) { kp_rgb_request_output_pass(true); }

void kp_rgb_effect_invalidate(const struct device *effect) {
  ARG_UNUSED(effect);
  zmk_rgb_matrix_flush();
}

void kp_rgb_conditions_refreshed(uint64_t token) {
  kp_rgb_matrix_lock();
  if (kp_rgb_refresh_pending && token == kp_rgb_refresh_token) {
    kp_rgb_refresh_pending = false;
    zmk_rgb_matrix_flush();
  }
  kp_rgb_matrix_unlock();
}

static void kp_rgb_begin_output_pass(void) {
  k_spinlock_key_t key = k_spin_lock(&kp_rgb_schedule_lock);
  kp_rgb_pass_active = true;
  k_spin_unlock(&kp_rgb_schedule_lock, key);
}

static bool kp_rgb_take_scene_request(void) {
  k_spinlock_key_t key = k_spin_lock(&kp_rgb_schedule_lock);
  bool requested = kp_rgb_scene_requested;
  kp_rgb_scene_requested = false;
  kp_rgb_pass_requested = false;
  k_spin_unlock(&kp_rgb_schedule_lock, key);
  return requested;
}

static void kp_rgb_finish_output_pass(int64_t deadline_ms, bool lock_retry) {
  k_spinlock_key_t key = k_spin_lock(&kp_rgb_schedule_lock);
  if (lock_retry) {
    kp_rgb_pass_requested = true;
  } else if (kp_rgb_pass_requested) {
    deadline_ms = k_uptime_get();
  }
  int err = kp_rgb_schedule_locked(deadline_ms);
  kp_rgb_pass_active = false;
  k_spin_unlock(&kp_rgb_schedule_lock, key);
  if (err < 0) LOG_WRN("Failed to queue RGB output (%d)", err);
}

/* Prioritize key handling over RGB feedback: every event is copied here and
 * delivered by the low-priority queue. Drop events if the ring fills up. */
#define KP_RGB_EVENT_QUEUE_LEN 16

static struct kp_rgb_key_event
    kp_rgb_pending[KP_RGB_EVENT_QUEUE_LEN];
static uint8_t kp_rgb_pending_head;
static uint8_t kp_rgb_pending_tail;
static uint32_t kp_rgb_pending_dropped;
static struct k_spinlock kp_rgb_pending_lock;

static bool kp_rgb_pending_pop(struct kp_rgb_key_event *out);
static bool kp_rgb_pending_available(void);
static uint32_t kp_rgb_pending_take_dropped(void);
static bool kp_rgb_deliver_position(const struct kp_rgb_key_event *ev);

typedef void (*scene_visitor)(const struct device *dev,
                              const struct kp_rgb_effect_callbacks *callbacks,
                              struct kp_rgb_scene_runtime *runtime,
                              int64_t now_ms);

static void kp_rgb_each_scene(scene_visitor visit, int64_t now_ms) {
  for (size_t i = 0; i < kp_rgb_effect_count(); i++) {
    const struct device *dev = kp_rgb_effect_at(i);
    if (dev == NULL) continue;
    const struct kp_rgb_effect_api *api = dev->api;
    visit(dev, api->callbacks, &kp_rgb_effect_scenes[i], now_ms);
  }
  const struct device *const *overlays = kp_rgb_overlay_list();
  for (size_t i = 0; i < kp_rgb_overlay_count(); i++) {
    const struct device *dev = overlays[i];
    if (dev == NULL) continue;
    const struct kp_rgb_overlay_api *api = dev->api;
    visit(dev, api->callbacks, &kp_rgb_overlay_scenes[i], now_ms);
  }
}

static void kp_rgb_restart_clock(const struct device *dev,
                                 const struct kp_rgb_effect_callbacks *callbacks,
                                 struct kp_rgb_scene_runtime *runtime, int64_t now_ms) {
  ARG_UNUSED(dev);
  ARG_UNUSED(callbacks);
  ARG_UNUSED(now_ms);
  runtime->state.animating = false;
}

static void kp_rgb_clear_wanted(const struct device *dev,
                                const struct kp_rgb_effect_callbacks *callbacks,
                                struct kp_rgb_scene_runtime *runtime, int64_t now_ms) {
  ARG_UNUSED(dev);
  ARG_UNUSED(callbacks);
  ARG_UNUSED(now_ms);
  runtime->wanted = false;
}

static void kp_rgb_mark_reset(const struct device *dev,
                              const struct kp_rgb_effect_callbacks *callbacks,
                              struct kp_rgb_scene_runtime *runtime, int64_t now_ms) {
  ARG_UNUSED(dev);
  ARG_UNUSED(callbacks);
  ARG_UNUSED(now_ms);
  runtime->reset_pending = true;
}

void kp_rgb_request_runtime_reset_locked(void) {
  kp_rgb_each_scene(kp_rgb_mark_reset, 0);
  zmk_rgb_matrix_flush();
}

static void kp_rgb_deactivate_scene(const struct device *dev,
                                    const struct kp_rgb_effect_callbacks *callbacks,
                                    struct kp_rgb_scene_runtime *runtime, int64_t now_ms) {
  if (runtime->state.active && !runtime->wanted) {
    kp_rgb_callbacks_set_active(dev, callbacks, &runtime->state, false, now_ms);
  }
}

static void kp_rgb_reset_scene(const struct device *dev,
                               const struct kp_rgb_effect_callbacks *callbacks,
                               struct kp_rgb_scene_runtime *runtime, int64_t now_ms) {
  if (runtime->reset_pending || (!runtime->state.initialized && runtime->wanted)) {
    kp_rgb_callbacks_reset(dev, callbacks, &runtime->state, now_ms);
    runtime->reset_pending = false;
  }
}

static void kp_rgb_activate_scene(const struct device *dev,
                                  const struct kp_rgb_effect_callbacks *callbacks,
                                  struct kp_rgb_scene_runtime *runtime, int64_t now_ms) {
  if (runtime->wanted) {
    kp_rgb_callbacks_set_active(dev, callbacks, &runtime->state, true, now_ms);
  }
}

/* Returns the index of the last overlay covering every LED, or SIZE_MAX when
 * none does; the caller then renders the effect and starts painting at 0. See
 * docs/development.md#an-opaque-full-cover-overlay-skips-what-it-hides. */
static size_t kp_last_covering_overlay(const struct device *const *overlays,
                                       size_t count) {
  size_t last = SIZE_MAX;
  for (size_t i = 0; i < count; i++) {
    const struct device *dev = overlays[i];
    if (dev == NULL || !kp_rgb_overlay_covers_all(dev)) {
      continue;
    }
    if (kp_rgb_overlay_gate(dev)) {
      last = i;
    }
  }
  return last;
}

static bool kp_render_overlays(const struct kp_rgb_frame *frame,
                               const struct device *const *overlays,
                               size_t count, size_t first) {
  bool animating = false;
  for (size_t i = first; i < count; i++) {
    const struct device *dev = overlays[i];
    if (dev == NULL) {
      continue;
    }
    const struct kp_rgb_overlay_api *ovl = dev->api;
    if (!kp_rgb_overlay_gate(dev)) {
      continue;
    }
    struct kp_rgb_frame local = *frame;
    local.targets = kp_rgb_overlay_targets(dev);
    local.target_count = kp_rgb_overlay_target_count(dev);
    local.scratch = scratch;
    animating |= kp_rgb_callbacks_render(
        dev, ovl->callbacks, &kp_rgb_overlay_scene(dev)->state, &local);
  }
  return animating;
}

static bool kp_rgb_scene_pass_locked(int64_t now_ms, bool allowed, bool paint) {
  const struct device *fx = kp_rgb_controller.state.active_fx;
  const struct device *const *overlays = NULL;
  size_t overlay_count = 0, first = SIZE_MAX;
  kp_rgb_each_scene(kp_rgb_clear_wanted, now_ms);

  if (fx != NULL && allowed) {
    const struct kp_rgb_effect_api *api = fx->api;
    overlays = api->overlays != NULL ? api->overlays : kp_rgb_overlay_list();
    overlay_count = api->overlays != NULL ? api->overlays_len : kp_rgb_overlay_count();
    first = kp_last_covering_overlay(overlays, overlay_count);
    kp_rgb_effect_scene(fx)->wanted = first == SIZE_MAX;

    for (size_t i = first == SIZE_MAX ? 0 : first; i < overlay_count; i++) {
      if (overlays[i] == NULL || !kp_rgb_overlay_gate(overlays[i])) continue;
      kp_rgb_overlay_scene(overlays[i])->wanted = true;
    }
  }

  kp_rgb_each_scene(kp_rgb_deactivate_scene, now_ms);
  kp_rgb_each_scene(kp_rgb_reset_scene, now_ms);
  kp_rgb_each_scene(kp_rgb_activate_scene, now_ms);

  struct kp_rgb_key_event event;
  bool input_changed = false;
  for (size_t i = 0; i < KP_RGB_EVENT_QUEUE_LEN && kp_rgb_pending_pop(&event); i++) {
    input_changed |= kp_rgb_deliver_position(&event);
  }

  if (allowed && (paint || kp_rgb_scene_dirty || input_changed)) {
    memset(scene, 0, sizeof(scene));
    struct kp_rgb_frame frame = {
        .targets = kp_rgb_all_targets,
        .target_count = KP_LED_COUNT,
        .count = KP_LED_COUNT,
        .coords = kp_led_coords,
        .pixels = scene,
        .now_ms = now_ms,
        .board_length = kp_rgb_board_length,
        .board_height = kp_rgb_board_height,
        .is_idle = zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE,
    };
    kp_rgb_scene_animating = false;
    if (fx != NULL) {
      const struct kp_rgb_effect_api *api = fx->api;
      kp_rgb_scene_animating |= kp_rgb_callbacks_render(
          fx, api->callbacks, &kp_rgb_effect_scene(fx)->state, &frame);
      kp_rgb_scene_animating |= kp_render_overlays(
          &frame, overlays, overlay_count, first == SIZE_MAX ? 0 : first);
    }
    uint8_t cap = frame.is_idle ? kp_rgb_controller.tuning.idle_brightness
                                : kp_rgb_controller.tuning.max_brightness;
    uint8_t brightness = (uint8_t)(((uint16_t)cap * kp_rgb_controller.brightness) /
                                   KP_RGB_BRT_MAX);
    for (size_t i = 0; i < KP_LED_COUNT; i++) {
      scene[i] = kp_rgb_rgb_scale(scene[i], brightness);
    }
    kp_rgb_output_pending = KP_RGB_OUTPUT_SCENE;
    kp_rgb_scene_dirty = false;
    return true;
  }
  return false;
}

static void kp_rgb_request_black_locked(void) {
  if (!kp_rgb_output_ready || !kp_rgb_has_leds() ||
      kp_rgb_output_pending == KP_RGB_OUTPUT_BLACK) return;
  kp_rgb_output_pending = KP_RGB_OUTPUT_BLACK;
  kp_rgb_output_urgent_black = true;
}

static int64_t kp_rgb_output_deadline_locked(void) {
  if (kp_rgb_output_pending == KP_RGB_OUTPUT_NONE) return INT64_MAX;
  return kp_rgb_output_urgent_black ? 0 : kp_rgb_output_retry_deadline_ms;
}

static void kp_rgb_attempt_output_locked(void) {
  if (kp_rgb_output_pending == KP_RGB_OUTPUT_NONE ||
      k_uptime_get() < kp_rgb_output_deadline_locked()) return;

  bool black = kp_rgb_output_pending == KP_RGB_OUTPUT_BLACK;
  kp_rgb_output_urgent_black = false;
  /* led_strip_update_rgb() may overwrite even a failed submission. */
  if (black) {
    memset(scratch, 0, sizeof(scratch));
  } else {
    memcpy(scratch, scene, sizeof(scene));
  }
  int err = led_strip_update_rgb(strip, scratch, KP_LED_COUNT);
  if (err < 0) {
    LOG_WRN("Failed to %s the RGB strip (%d)", black ? "clear" : "update", err);
    kp_rgb_output_retry_deadline_ms = k_uptime_get() + kp_rgb_output_retry_ms;
    kp_rgb_output_retry_ms = MIN(kp_rgb_output_retry_ms * 2, 1000);
  } else {
    kp_rgb_output_pending = KP_RGB_OUTPUT_NONE;
    kp_rgb_output_retry_deadline_ms = 0;
    kp_rgb_output_retry_ms = 100;
  }
}

static void kp_rgb_output_handler(struct k_work *work) {
  ARG_UNUSED(work);
  kp_rgb_begin_output_pass();
  if (!kp_rgb_matrix_lock_patiently()) {
    LOG_WRN("Failed to obtain RGB matrix lock");
    kp_rgb_finish_output_pass(k_uptime_get() + KP_RGB_LOCK_RETRY_MS, true);
    return;
  }

  kp_rgb_scene_dirty |= kp_rgb_take_scene_request();
  bool allowed = atomic_get(&kp_rgb_output_allowed);
  int64_t now_ms = k_uptime_get();
  bool ready = allowed && !kp_rgb_refresh_pending;
  bool painted = false;
  if (!allowed || ready) {
    painted = kp_rgb_scene_pass_locked(now_ms, ready, now_ms >= kp_rgb_next_frame_ms);
  }
  kp_rgb_attempt_output_locked();

  if (painted) {
    if (kp_rgb_scene_animating) {
      if (kp_rgb_next_frame_ms == INT64_MAX) kp_rgb_next_frame_ms = now_ms;
      int64_t finished_ms = k_uptime_get();
      if (kp_rgb_next_frame_ms <= finished_ms) {
        int64_t period_ms = CONFIG_KEYPAW_RGB_MATRIX_TICK_MS;
        kp_rgb_next_frame_ms +=
            ((finished_ms - kp_rgb_next_frame_ms) / period_ms + 1) * period_ms;
      }
    } else {
      kp_rgb_next_frame_ms = INT64_MAX;
    }
  }

  int64_t next_ms = MIN(ready ? kp_rgb_next_frame_ms : INT64_MAX,
                        kp_rgb_output_deadline_locked());
  if (ready && kp_rgb_pending_available()) next_ms = k_uptime_get();
  kp_rgb_finish_output_pass(next_ms, false);
  kp_rgb_matrix_unlock();

  uint32_t dropped = kp_rgb_pending_take_dropped();
  if (dropped > 0) LOG_WRN("dropped %u RGB event(s): key-event queue full", dropped);
}

int zmk_rgb_matrix_set_inhibited(bool inhibited) {
  if (k_is_in_isr()) {
    return -EWOULDBLOCK;
  }
  kp_rgb_matrix_lock();
  if (atomic_get(&kp_rgb_inhibited) != inhibited) {
    atomic_set(&kp_rgb_inhibited, inhibited);
    kp_rgb_reconcile_power_locked();
  }
  kp_rgb_matrix_unlock();
  return 0;
}

bool zmk_rgb_matrix_is_inhibited(void) {
  return atomic_get(&kp_rgb_inhibited) != 0;
}

/* Caller holds kp_rgb_lock. */
static bool kp_rgb_deliver_position(const struct kp_rgb_key_event *ev) {
  const struct device *fx = kp_rgb_controller.state.active_fx;
  if (fx == NULL || kp_rgb_led_for_position(ev->position) == SIZE_MAX) {
    return false;
  }
  const struct kp_rgb_effect_api *api = fx->api;
  bool changed = kp_rgb_callbacks_on_event(fx, api->callbacks,
                                           &kp_rgb_effect_scene(fx)->state, ev);

  const struct device *const *overlays =
      api->overlays != NULL ? api->overlays : kp_rgb_overlay_list();
  size_t count = api->overlays != NULL ? api->overlays_len : kp_rgb_overlay_count();
  for (size_t i = 0; i < count; i++) {
    const struct device *dev = overlays[i];
    if (dev == NULL || !kp_rgb_overlay_gate(dev)) continue;
    const struct kp_rgb_overlay_api *ovl = dev->api;
    changed |= kp_rgb_callbacks_on_event(dev, ovl->callbacks,
                                         &kp_rgb_overlay_scene(dev)->state, ev);
  }
  return changed;
}

void kp_rgb_reconcile_power_locked(void) {
  bool allowed = kp_rgb_output_ready && kp_rgb_has_leds() &&
                 kp_rgb_logical_on_locked() && !atomic_get(&kp_rgb_inhibited);
  if (allowed == (bool)atomic_get(&kp_rgb_output_allowed)) {
    return;
  }

  k_spinlock_key_t key = k_spin_lock(&kp_rgb_pending_lock);
  kp_rgb_pending_tail = kp_rgb_pending_head;
  kp_rgb_pending_dropped = 0;
  /* Admission and purge share the ring lock so newly admitted input survives. */
  atomic_set(&kp_rgb_output_allowed, allowed);
  k_spin_unlock(&kp_rgb_pending_lock, key);

  if (allowed) {
    kp_rgb_output_pending = KP_RGB_OUTPUT_NONE;
    kp_rgb_output_urgent_black = false;
    kp_rgb_each_scene(kp_rgb_restart_clock, 0);
    kp_rgb_next_frame_ms = INT64_MAX;
    kp_rgb_scene_dirty = true;
    kp_rgb_refresh_pending = true;
    kp_rgb_refresh_token++;
    kp_rgb_conditions_request_refresh(kp_rgb_refresh_token);
  } else {
    kp_rgb_next_frame_ms = INT64_MAX;
    kp_rgb_scene_animating = false;
    kp_rgb_refresh_pending = false;
    kp_rgb_request_black_locked();
  }
  zmk_rgb_matrix_flush();
}

static bool kp_rgb_pending_push(const struct kp_rgb_key_event *ev) {
  k_spinlock_key_t key = k_spin_lock(&kp_rgb_pending_lock);
  if (!atomic_get(&kp_rgb_output_allowed)) {
    k_spin_unlock(&kp_rgb_pending_lock, key);
    return false;
  }
  uint8_t next = (uint8_t)((kp_rgb_pending_head + 1) % KP_RGB_EVENT_QUEUE_LEN);
  bool room = next != kp_rgb_pending_tail;
  if (room) {
    kp_rgb_pending[kp_rgb_pending_head] = *ev;
    kp_rgb_pending_head = next;
  } else {
    kp_rgb_pending_dropped++;
  }
  k_spin_unlock(&kp_rgb_pending_lock, key);
  return room;
}

static bool kp_rgb_pending_pop(struct kp_rgb_key_event *out) {
  k_spinlock_key_t key = k_spin_lock(&kp_rgb_pending_lock);
  bool pending = kp_rgb_pending_tail != kp_rgb_pending_head;
  if (pending) {
    *out = kp_rgb_pending[kp_rgb_pending_tail];
    kp_rgb_pending_tail =
        (uint8_t)((kp_rgb_pending_tail + 1) % KP_RGB_EVENT_QUEUE_LEN);
  }
  k_spin_unlock(&kp_rgb_pending_lock, key);
  return pending;
}

static uint32_t kp_rgb_pending_take_dropped(void) {
  k_spinlock_key_t key = k_spin_lock(&kp_rgb_pending_lock);
  uint32_t dropped = kp_rgb_pending_dropped;
  kp_rgb_pending_dropped = 0;
  k_spin_unlock(&kp_rgb_pending_lock, key);
  return dropped;
}

static bool kp_rgb_pending_available(void) {
  k_spinlock_key_t key = k_spin_lock(&kp_rgb_pending_lock);
  bool pending = kp_rgb_pending_tail != kp_rgb_pending_head;
  k_spin_unlock(&kp_rgb_pending_lock, key);
  return pending;
}

static int kp_rgb_matrix_event_listener(const zmk_event_t *eh) {
  const struct zmk_position_state_changed *pos_ev =
      as_zmk_position_state_changed(eh);
  if (pos_ev != NULL) {
    struct kp_rgb_key_event event = {
        .position = pos_ev->position,
        .pressed = pos_ev->state,
        .timestamp_ms = pos_ev->timestamp,
    };
    if (kp_rgb_pending_push(&event)) kp_rgb_request_output_pass(false);
    return ZMK_EV_EVENT_BUBBLE;
  }

  if (as_zmk_activity_state_changed(eh) != NULL) {
    kp_rgb_matrix_lock();
    int state = zmk_activity_get_state();
    if (state != kp_rgb_activity_state) {
      kp_rgb_activity_state = state;
      kp_rgb_reconcile_power_locked();
      zmk_rgb_matrix_flush();
    }
    kp_rgb_matrix_unlock();
    return ZMK_EV_EVENT_BUBBLE;
  }
  return -ENOTSUP;
}
ZMK_LISTENER(kp_rgb_matrix, kp_rgb_matrix_event_listener);
ZMK_SUBSCRIPTION(kp_rgb_matrix, zmk_position_state_changed);
ZMK_SUBSCRIPTION(kp_rgb_matrix, zmk_activity_state_changed);

static int kp_rgb_set_user_on(bool on) {
  if (k_is_in_isr()) return -EWOULDBLOCK;
  int ret = KP_TRY_LOCK();
  if (ret < 0) {
    return ret;
  }
  kp_rgb_controller.state.user_on = on;
  kp_rgb_reconcile_power_locked();
  kp_rgb_matrix_unlock();
  return 0;
}

int zmk_rgb_matrix_on(void) { return kp_rgb_set_user_on(true); }

int zmk_rgb_matrix_off(void) { return kp_rgb_set_user_on(false); }

int zmk_rgb_matrix_toggle(void) {
  if (k_is_in_isr()) return -EWOULDBLOCK;
  int ret = KP_TRY_LOCK();
  if (ret < 0) {
    return ret;
  }
  kp_rgb_controller.state.user_on = !kp_rgb_controller.state.user_on;
  kp_rgb_reconcile_power_locked();
  kp_rgb_matrix_unlock();
  return 0;
}

int zmk_rgb_matrix_get_state(bool *on_off) {
  if (k_is_in_isr()) return -EWOULDBLOCK;
  if (on_off == NULL)
    return -EINVAL;
  kp_rgb_matrix_lock();
  *on_off = kp_rgb_logical_on_locked();
  kp_rgb_matrix_unlock();
  return 0;
}

int zmk_rgb_matrix_get_user_state(bool *user_on) {
  if (k_is_in_isr()) return -EWOULDBLOCK;
  if (user_on == NULL) return -EINVAL;
  kp_rgb_matrix_lock();
  *user_on = kp_rgb_controller.state.user_on;
  kp_rgb_matrix_unlock();
  return 0;
}

static int kp_rgb_matrix_layout_init(void) {
  const struct zmk_physical_layout *const *layouts;
  size_t layout_count = zmk_physical_layouts_get_list(&layouts);
  if (layout_count == 0) {
    LOG_ERR("No physical layout available for the RGB matrix");
    return -ENODEV;
  }
  kp_resolve_layout(layouts[0]);
  return 0;
}
SYS_INIT(kp_rgb_matrix_layout_init, POST_KERNEL,
         CONFIG_KERNEL_INIT_PRIORITY_OBJECTS);

static int kp_rgb_matrix_init(void) {
  if (kp_rgb_has_leds() && !device_is_ready(strip)) {
    LOG_ERR("LED strip \"%s\" is not ready", strip->name);
    return -ENODEV;
  }
  kp_rgb_matrix_lock();
  kp_rgb_output_ready = true;
  if (kp_rgb_has_leds()) {
    kp_rgb_request_black_locked();
    kp_rgb_attempt_output_locked();
    if (kp_rgb_output_pending != KP_RGB_OUTPUT_NONE) zmk_rgb_matrix_flush();
  }
  kp_rgb_reconcile_power_locked();
  kp_rgb_matrix_unlock();
  return 0;
}
SYS_INIT(kp_rgb_matrix_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
