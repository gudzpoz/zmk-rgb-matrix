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
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/util.h>

#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/rgb_matrix.h>

#include <zmk/events/activity_state_changed.h>

#include "rgb_matrix_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define KP_LAYOUT DT_PHANDLE(KP_RGB_NODE, physical_layout)
#define KP_NKEYS DT_PROP_LEN_OR(KP_LAYOUT, keys, 0)

BUILD_ASSERT(DT_NODE_HAS_COMPAT(KP_LAYOUT, zmk_physical_layout),
             "RGB physical-layout must reference zmk,physical-layout");
BUILD_ASSERT(DT_NODE_HAS_PROP(KP_LAYOUT, keys),
             "RGB physical-layout must provide keys");

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
             "RGB matrix requires exactly one enabled matrix node");
BUILD_ASSERT(
    DT_PROP_LEN(KP_RGB_NODE, mapping) == KP_LED_COUNT,
    "keypaw,rgb-matrix: 'mapping' must hold exactly one entry per LED");
BUILD_ASSERT(CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS <=
                 CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS,
             "KEYPAW_RGB_MATRIX_DURATION_MIN_MS must not exceed "
             "KEYPAW_RGB_MATRIX_DURATION_MAX_MS");
BUILD_ASSERT(DT_PROP(KP_RGB_NODE, max_brightness) >= 0 &&
                 DT_PROP(KP_RGB_NODE, max_brightness) <= KP_RGB_BRT_MAX,
             "max-brightness must be in 0..100");
BUILD_ASSERT(DT_PROP(KP_RGB_NODE, idle_brightness) >= 0 &&
                 DT_PROP(KP_RGB_NODE, idle_brightness) <= KP_RGB_BRT_MAX,
             "idle-brightness must be in 0..100");
BUILD_ASSERT(DT_PROP(KP_RGB_NODE, initial_brightness) >= 0 &&
                 DT_PROP(KP_RGB_NODE, initial_brightness) <= KP_RGB_BRT_MAX,
             "initial-brightness must be in 0..100");

#define KP_MAP_ENTRY_ASSERT(node_id, prop, idx)                                \
  BUILD_ASSERT((DT_PROP_BY_IDX(node_id, prop, idx) & KP_RGB_NO_KEY_XY_FLAG) || \
                   DT_PROP_BY_IDX(node_id, prop, idx) < KP_NKEYS,              \
               "keypaw,rgb-matrix: invalid mapping entry");
DT_FOREACH_PROP_ELEM(KP_RGB_NODE, mapping, KP_MAP_ENTRY_ASSERT);

#define KP_RGB_INITIAL_ON DT_PROP_OR(KP_RGB_NODE, initial_on, 1)
#define KP_RGB_INITIAL_BRIGHTNESS DT_PROP(KP_RGB_NODE, initial_brightness)

const struct kp_rgb_config kp_rgb_config = {
    .initial_on = KP_RGB_INITIAL_ON,
    .initial_brightness = KP_RGB_INITIAL_BRIGHTNESS,
    .max_brightness = DT_PROP(KP_RGB_NODE, max_brightness),
    .idle_brightness = DT_PROP(KP_RGB_NODE, idle_brightness),
};

struct kp_rgb_global kp_rgb_global = {
    .user_on = KP_RGB_INITIAL_ON,
    .brightness = KP_RGB_INITIAL_BRIGHTNESS,
};

/* Match ZMK's signed 16-bit physical attributes before unsigned storage. */
#define KP_KEY_CENTER_AXIS(x, width)                                           \
  (uint16_t)((int16_t)(int32_t)DT_PHA_BY_IDX(node_id, prop, idx, x) +          \
             (int16_t)(int32_t)DT_PHA_BY_IDX(node_id, prop, idx, width) / 2)
#define KP_KEY_CENTER(node_id, prop, idx)                                      \
  {                                                                            \
      .x = KP_KEY_CENTER_AXIS(x, width),                                       \
      .y = KP_KEY_CENTER_AXIS(y, height),                                      \
  },

static const struct kp_rgb_coord kp_key_centers[KP_NKEYS] = {
#if DT_NODE_HAS_COMPAT(KP_LAYOUT, zmk_physical_layout) && DT_NODE_HAS_PROP(KP_LAYOUT, keys)
    DT_FOREACH_PROP_ELEM(KP_LAYOUT, keys, KP_KEY_CENTER)
#endif
};

static const uint32_t kp_map[KP_LED_COUNT] = DT_PROP(KP_RGB_NODE, mapping);
static struct kp_rgb_coord kp_led_coords[KP_LED_COUNT];
static uint16_t kp_rgb_board_length;
static uint16_t kp_rgb_board_height;
static size_t kp_key_to_led[KP_NKEYS];

static void kp_resolve_layout(void) {
  uint16_t min_x = UINT16_MAX, min_y = UINT16_MAX, max_x = 0, max_y = 0;
  memset(kp_key_to_led, 0xFF, sizeof(kp_key_to_led));
  for (size_t i = 0; i < KP_LED_COUNT; i++) {
    uint32_t k = kp_map[i];
    uint32_t x, y;
    if (k & KP_RGB_NO_KEY_XY_FLAG) {
      x = (k >> 16) & KP_RGB_NO_KEY_XY_X_MASK;
      y = k & KP_RGB_NO_KEY_XY_Y_MASK;
    } else {
      x = kp_key_centers[k].x;
      y = kp_key_centers[k].y;
      kp_key_to_led[k] = i;
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

/* Composition cache through the last leading settled participant
   (pre-brightness). */
static struct led_rgb kp_rgb_prefix[KP_LED_COUNT];
static size_t kp_rgb_prefix_len;

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
static size_t kp_rgb_refresh_holds;
static enum zmk_activity_state kp_rgb_activity_state = ZMK_ACTIVITY_ACTIVE;

/* shared - local, in ms; 0 on the central/standalone. Written by the split
 * command handler, read once per render pass. */
static atomic_t kp_rgb_clock_offset_ms;

/* A half with no strip renders nothing, but the central-only overlay and split
 * machinery still runs. */
static inline bool kp_rgb_has_leds(void) { return KP_LED_COUNT > 0; }

struct kp_rgb_scene_runtime {
  struct kp_rgb_callback_state state;
  bool wanted;
  bool reset_pending;
};

static struct kp_rgb_scene_runtime kp_rgb_overlay_scenes[MAX(1, KP_RGB_OVERLAY_COUNT)];

static bool kp_rgb_logical_on_locked(void) {
  return kp_rgb_global.user_on &&
         (kp_rgb_activity_state == ZMK_ACTIVITY_ACTIVE ||
          (kp_rgb_activity_state == ZMK_ACTIVITY_IDLE && kp_rgb_config.idle_brightness != 0));
}

static uint8_t kp_rgb_brightness_cap_locked(void) {
  return kp_rgb_activity_state == ZMK_ACTIVITY_ACTIVE
             ? kp_rgb_config.max_brightness
             : MIN(kp_rgb_config.idle_brightness, kp_rgb_config.max_brightness);
}

#define KP_TRY_LOCK()                                                          \
  k_mutex_lock(&kp_rgb_lock, K_MSEC(CONFIG_KEYPAW_RGB_MATRIX_TICK_MS))

void kp_rgb_matrix_lock(void) { k_mutex_lock(&kp_rgb_lock, K_FOREVER); }
void kp_rgb_matrix_unlock(void) { k_mutex_unlock(&kp_rgb_lock); }

uint32_t kp_rgb_clock_ms(void) {
  return (uint32_t)k_uptime_get() +
         (uint32_t)(int32_t)atomic_get(&kp_rgb_clock_offset_ms);
}

void kp_rgb_set_clock_offset(int32_t offset_ms) {
  atomic_set(&kp_rgb_clock_offset_ms, (atomic_val_t)offset_ms);
}

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
  int err = k_work_reschedule_for_queue(kp_rgb_work_q(), &kp_output_work,
                                      delay_ms ? K_MSEC(delay_ms) : K_NO_WAIT);
  if (err < 0) {
    kp_rgb_pass_requested = true;
    k_work_reschedule_for_queue(kp_rgb_work_q(), &kp_output_work,
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

void kp_rgb_control_refresh_locked(void) {
  kp_rgb_refresh_pending = true;
  kp_rgb_refresh_token++;
  if (!kp_rgb_refresh_holds)
    kp_rgb_conditions_request_refresh(kp_rgb_refresh_token);
}

void kp_rgb_control_refresh_hold_locked(void) {
  kp_rgb_refresh_holds++;
  kp_rgb_control_refresh_locked();
}

void kp_rgb_control_refresh_release_locked(void) {
  __ASSERT(kp_rgb_refresh_holds, "unbalanced RGB control refresh release");
  if (--kp_rgb_refresh_holds == 0)
    kp_rgb_conditions_request_refresh(kp_rgb_refresh_token);
}

void kp_rgb_conditions_refreshed(uint64_t token) {
  kp_rgb_matrix_lock();
  if (kp_rgb_refresh_pending && !kp_rgb_refresh_holds &&
      token == kp_rgb_refresh_token) {
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

void kp_rgb_request_runtime_reset_locked(void) {
  const struct device *const *overlays = kp_rgb_overlay_list();
  for (size_t i = 0; i < kp_rgb_overlay_count(); i++) {
    if (overlays[i] == NULL) continue;
    kp_rgb_overlay_scenes[i].reset_pending = true;
  }
  zmk_rgb_matrix_flush();
}

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

static bool kp_rgb_scene_pass_locked(int64_t now_ms, bool allowed, bool paint) {
  const struct device *const *overlays = kp_rgb_overlay_list();
  size_t overlay_count = kp_rgb_overlay_count();
  size_t first = allowed ? kp_last_covering_overlay(overlays, overlay_count) : SIZE_MAX;
  for (size_t i = 0; i < overlay_count; i++) {
    const struct device *dev = overlays[i];
    if (dev == NULL) continue;
    kp_rgb_overlay_scenes[i].wanted =
        allowed && (first == SIZE_MAX || i >= first) && kp_rgb_overlay_gate(dev);
  }

  for (size_t i = 0; i < overlay_count; i++) {
    const struct device *dev = overlays[i];
    if (dev == NULL) continue;
    const struct kp_rgb_overlay_api *api = dev->api;
    struct kp_rgb_scene_runtime *runtime = &kp_rgb_overlay_scenes[i];
    if (runtime->state.active && !runtime->wanted) {
      kp_rgb_callbacks_set_active(dev, api->callbacks, &runtime->state, false, now_ms);
    }
  }

  for (size_t i = 0; i < overlay_count; i++) {
    const struct device *dev = overlays[i];
    if (dev == NULL) continue;
    const struct kp_rgb_overlay_api *api = dev->api;
    struct kp_rgb_scene_runtime *runtime = &kp_rgb_overlay_scenes[i];
    if (runtime->reset_pending || (!runtime->state.initialized && runtime->wanted)) {
      kp_rgb_callbacks_reset(dev, api->callbacks, &runtime->state, now_ms);
      runtime->reset_pending = false;
    }
  }

  for (size_t i = 0; i < overlay_count; i++) {
    const struct device *dev = overlays[i];
    if (dev == NULL) continue;
    const struct kp_rgb_overlay_api *api = dev->api;
    struct kp_rgb_scene_runtime *runtime = &kp_rgb_overlay_scenes[i];
    if (runtime->wanted) {
      kp_rgb_callbacks_set_active(dev, api->callbacks, &runtime->state, true, now_ms);
    }
  }

  struct kp_rgb_key_event event;
  bool input_changed = false;
  for (size_t i = 0; i < KP_RGB_EVENT_QUEUE_LEN && kp_rgb_pending_pop(&event); i++) {
    input_changed |= kp_rgb_deliver_position(&event);
  }

  if (allowed && (paint || kp_rgb_scene_dirty || input_changed)) {
    bool reuse = kp_rgb_prefix_len != 0 && !kp_rgb_scene_dirty && !input_changed;
    size_t skip = reuse ? kp_rgb_prefix_len : 0;
    size_t prefix_count = 0;
    if (reuse) {
      memcpy(scene, kp_rgb_prefix, sizeof(scene));
    } else {
      memset(scene, 0, sizeof(scene));
    }

    struct kp_rgb_frame frame = {
        .targets = kp_rgb_all_targets,
        .target_count = KP_LED_COUNT,
        .count = KP_LED_COUNT,
        .coords = kp_led_coords,
        .pixels = scene,
        .local_ms = now_ms,
        .clock_ms = kp_rgb_clock_ms(),
        .board_length = kp_rgb_board_length,
        .board_height = kp_rgb_board_height,
    };
    kp_rgb_scene_animating = false;
    frame.scratch = scratch;
    for (size_t i = first == SIZE_MAX ? 0 : first; i < overlay_count; i++) {
      const struct device *dev = overlays[i];
      if (dev == NULL || !kp_rgb_overlay_gate(dev)) continue;
      if (skip > 0) {
        skip--;
        continue;
      }
      frame.targets = kp_rgb_overlay_targets(dev);
      frame.target_count = kp_rgb_overlay_target_count(dev);

      const struct kp_rgb_overlay_api *ovl = dev->api;
      kp_rgb_scene_animating |= kp_rgb_callbacks_render(
          dev, ovl->callbacks, &kp_rgb_overlay_scenes[i].state, &frame);
      if (!reuse && !kp_rgb_scene_animating) {
        memcpy(kp_rgb_prefix, frame.pixels, sizeof(kp_rgb_prefix));
        prefix_count++;
      }
    }
    if (!reuse) {
      kp_rgb_prefix_len = prefix_count;
    }

    uint8_t cap = kp_rgb_brightness_cap_locked();
    uint8_t brightness = (uint8_t)(((uint16_t)cap * kp_rgb_global.brightness) /
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
  bool transfer_ready = !allowed || ready || kp_rgb_output_pending == KP_RGB_OUTPUT_BLACK;
  if (transfer_ready)
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
                        transfer_ready ? kp_rgb_output_deadline_locked() : INT64_MAX);
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
  if (kp_rgb_led_for_position(ev->position) == SIZE_MAX) return false;
  bool changed = false;
  const struct device *const *overlays = kp_rgb_overlay_list();
  size_t count = kp_rgb_overlay_count();
  for (size_t i = 0; i < count; i++) {
    const struct device *dev = overlays[i];
    if (dev == NULL || !kp_rgb_overlay_gate(dev)) continue;
    const struct kp_rgb_overlay_api *ovl = dev->api;
    changed |= kp_rgb_callbacks_on_event(dev, ovl->callbacks,
                                         &kp_rgb_overlay_scenes[i].state, ev);
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
    const struct device *const *overlays = kp_rgb_overlay_list();
    for (size_t i = 0; i < kp_rgb_overlay_count(); i++) {
      if (overlays[i] == NULL) continue;
      kp_rgb_overlay_scenes[i].reset_pending = true;
    }
    kp_rgb_next_frame_ms = INT64_MAX;
    kp_rgb_scene_dirty = true;
    kp_rgb_control_refresh_locked();
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
    enum zmk_activity_state state = zmk_activity_get_state();
    if (state != kp_rgb_activity_state) {
      uint8_t old_cap = kp_rgb_brightness_cap_locked();
      kp_rgb_activity_state = state;
      kp_rgb_reconcile_power_locked();
      if (state == ZMK_ACTIVITY_SLEEP && kp_rgb_output_ready && kp_rgb_has_leds()) {
        /* ZMK suspends devices immediately after this event returns. */
        kp_rgb_request_black_locked();
        kp_rgb_output_urgent_black = true;
        kp_rgb_attempt_output_locked();
        if (kp_rgb_output_pending != KP_RGB_OUTPUT_NONE) zmk_rgb_matrix_flush();
      } else if (atomic_get(&kp_rgb_output_allowed) &&
                 old_cap != kp_rgb_brightness_cap_locked()) {
        zmk_rgb_matrix_flush();
      }
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
  kp_rgb_global.user_on = on;
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
  kp_rgb_global.user_on = !kp_rgb_global.user_on;
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
  *user_on = kp_rgb_global.user_on;
  kp_rgb_matrix_unlock();
  return 0;
}

void kp_rgb_apply_defaults(void) {
  kp_rgb_matrix_lock();
  kp_rgb_global.user_on = kp_rgb_config.initial_on;
  kp_rgb_global.brightness = kp_rgb_config.initial_brightness;
  kp_rgb_reconcile_power_locked();
  zmk_rgb_matrix_flush();
  kp_rgb_matrix_unlock();
}

static bool kp_rgb_parameter_target_ready(const struct device *fx) {
  return fx != NULL && device_is_ready(fx);
}

int kp_rgb_set_color_locked(const struct device *fx,
                           const struct kp_rgb_hsb *color) {
  if (!kp_rgb_parameter_target_ready(fx)) {
    return -ENODEV;
  }
  if (color == NULL || color->h > KP_RGB_HUE_MAX || color->s > KP_RGB_SAT_MAX ||
      color->b > KP_RGB_BRT_MAX) {
    return -EINVAL;
  }
  struct kp_rgb_hsb next = *color;
  next.h %= KP_RGB_HUE_MAX;
  struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(fx);
  if (data->color.h != next.h || data->color.s != next.s ||
      data->color.b != next.b) {
    data->color = next;
    zmk_rgb_matrix_flush();
  }
  return 0;
}

int kp_rgb_set_period_locked(const struct device *fx, uint32_t period_ms) {
  if (!kp_rgb_parameter_target_ready(fx)) {
    return -ENODEV;
  }
  if (period_ms < CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS ||
      period_ms > CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS) {
    return -EINVAL;
  }
  struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(fx);
  if (data->duration_ms != period_ms) {
    data->duration_ms = (uint16_t)period_ms;
    zmk_rgb_matrix_flush();
  }
  return 0;
}

int zmk_rgb_matrix_set_color(const struct device *fx,
                            const struct kp_rgb_hsb *color) {
  if (k_is_in_isr()) {
    return -EWOULDBLOCK;
  }
  kp_rgb_matrix_lock();
  int ret = kp_rgb_set_color_locked(fx, color);
  kp_rgb_matrix_unlock();
  return ret;
}

int zmk_rgb_matrix_set_period(const struct device *fx, uint32_t period_ms) {
  if (k_is_in_isr()) {
    return -EWOULDBLOCK;
  }
  kp_rgb_matrix_lock();
  int ret = kp_rgb_set_period_locked(fx, period_ms);
  kp_rgb_matrix_unlock();
  return ret;
}

static int kp_rgb_matrix_layout_init(void) {
  kp_resolve_layout();
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
  kp_rgb_activity_state = zmk_activity_get_state();
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
