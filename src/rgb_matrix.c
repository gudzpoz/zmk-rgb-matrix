/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * One physical RGB matrix engine merges the independently-owned behavior zones.
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

#if IS_ENABLED(CONFIG_KEYPAW_RGB_MATRIX_AUTO_OFF_IDLE)
#include <zmk/events/activity_state_changed.h>
#endif

#include "rgb_matrix_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define KP_LAYOUT DT_PHANDLE(KP_RGB_NODE, physical_layout)
#define KP_NKEYS DT_PROP_LEN(KP_LAYOUT, keys)

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
static struct led_rgb pixels[KP_LED_COUNT];
static struct led_rgb scratch[KP_LED_COUNT];
static uint32_t last_tick;
static K_MUTEX_DEFINE(kp_rgb_lock);
static bool kp_rgb_matrix_valid;
static atomic_t kp_rgb_any_on;

/* A half with no strip renders nothing, but the central-only overlay and split
 * machinery still runs. */
static inline bool kp_rgb_has_leds(void) { return KP_LED_COUNT > 0; }

const struct device *const kp_rgb_no_overlays[1] = {NULL};

bool kp_rgb_behavior_owns_led(const struct kp_rgb_behavior_context *ctx,
                              size_t led) {
  if (ctx == NULL || !ctx->zone_valid || led >= KP_LED_COUNT) {
    return false;
  }
  if (ctx->all_leds) {
    return true;
  }
  for (size_t i = 0; i < ctx->leds_len; i++) {
    if (ctx->leds[i] == led) {
      return true;
    }
  }
  return false;
}

static bool kp_any_on_locked(void) {
  for (size_t i = 0; i < kp_rgb_behavior_count(); i++) {
    struct kp_rgb_behavior_context *ctx = kp_rgb_behavior_at(i);
    if (ctx != NULL && ctx->state.on) {
      return true;
    }
  }
  return false;
}

/* Caller should hold kp_rgb_lock and has just changed a ctx->state.on.
 * Republish the lock-free mirror the timer ISR and zmk_rgb_matrix_flush()
 * read. */
static void kp_rgb_publish_any_on_locked(void) {
  atomic_set(&kp_rgb_any_on, kp_any_on_locked() ? 1 : 0);
}
#define KP_TRY_LOCK()                                                          \
  k_mutex_lock(&kp_rgb_lock, K_MSEC(CONFIG_KEYPAW_RGB_MATRIX_TICK_MS))

void kp_rgb_matrix_lock(void) { k_mutex_lock(&kp_rgb_lock, K_FOREVER); }
void kp_rgb_matrix_unlock(void) { k_mutex_unlock(&kp_rgb_lock); }

/* The lock is never held across flash I/O and every holder outranks this
 * low-priority queue, so waiting is a hiccup, not a deadlock. Dropping the frame
 * would also skip the overlay dispatch on this tick. Bounded, so a stuck holder
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

extern struct k_work kp_tick_work;
static void kp_rgb_matrix_tick(struct k_work *work);
/* Runs in the system timer ISR, so it must not take kp_rgb_lock (a mutex is
 * illegal in ISR context). It reads the atomic mirror; the work handler
 * re-checks the contexts authoritatively under the lock. */
static void kp_rgb_matrix_tick_handler(struct k_timer *timer) {
  ARG_UNUSED(timer);
  if (atomic_get(&kp_rgb_any_on)) {
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &kp_tick_work);
  }
}

K_TIMER_DEFINE(kp_tick_timer, kp_rgb_matrix_tick_handler, NULL);

static void kp_start_timer(void) {
  if (!atomic_get(&kp_rgb_any_on)) {
    return;
  }
  last_tick = k_uptime_get_32();
  k_timer_start(&kp_tick_timer, K_NO_WAIT,
                K_MSEC(CONFIG_KEYPAW_RGB_MATRIX_TICK_MS));
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
    if (kp_rgb_overlay_gate(dev, dev->api)) {
      last = i;
    }
  }
  return last;
}

static void kp_render_overlays(struct kp_rgb_frame *frame,
                               const struct device *const *overlays,
                               size_t count, size_t first) {
  for (size_t i = first; i < count; i++) {
    const struct device *dev = overlays[i];
    if (dev == NULL) {
      continue;
    }
    const struct kp_rgb_overlay_api *ovl = dev->api;
    if (!kp_rgb_overlay_gate(dev, ovl)) {
      continue;
    }
    ovl->render(dev, frame);
  }
}

static void kp_rgb_matrix_tick(struct k_work *work) {
  ARG_UNUSED(work);
  if (!kp_rgb_matrix_valid || !atomic_get(&kp_rgb_any_on)) {
    return;
  }
  uint32_t now = k_uptime_get_32();
  uint32_t elapsed = now - last_tick;
  last_tick = now;
  kp_rgb_triggers_poll();
  bool ovl_changed = kp_rgb_overlay_refresh();
  if (!kp_rgb_has_leds()) {
    if (ovl_changed) {
      kp_rgb_overlay_dispatch();
    }
    return;
  }
  bool any_on;
  if (!kp_rgb_matrix_lock_patiently()) {
    LOG_WRN("Failed to obtain RGB matrix lock");
    /* Retry on the queue rather than waiting for the next timer tick; the
     * bounded wait above is what keeps this from becoming a tight loop. */
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &kp_tick_work);
    return;
  }
  any_on = kp_any_on_locked();
  memset(pixels, 0, sizeof(pixels));
  if (any_on) {
    for (size_t n = 0; n < kp_rgb_behavior_count(); n++) {
      struct kp_rgb_behavior_context *ctx = kp_rgb_behavior_at(n);
      if (ctx == NULL || !ctx->zone_valid || !ctx->state.on ||
          ctx->state.active_fx == NULL) {
        continue;
      }
      /* `pixels` direct write when ctx->all_leds. */
      struct led_rgb *buf = ctx->all_leds ? pixels : scratch;
      if (!ctx->all_leds) {
        memset(scratch, 0, sizeof(scratch));
      }
      struct kp_rgb_frame frame = {
          .tune = &ctx->tuning,
          .count = KP_LED_COUNT,
          .coords = kp_led_coords,
          .pixels = buf,
          .elapsed = elapsed,
          .board_length = kp_rgb_board_length,
          .board_height = kp_rgb_board_height,
          .is_idle = zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE,
      };
      const struct kp_rgb_effect_api *api =
          (const struct kp_rgb_effect_api *)ctx->state.active_fx->api;
      const struct device *const *overlays =
          api->overlays != NULL ? api->overlays : kp_rgb_overlay_list();
      size_t overlay_count =
          api->overlays != NULL ? api->overlays_len : kp_rgb_overlay_count();
      size_t first = kp_last_covering_overlay(overlays, overlay_count);
      if (first == SIZE_MAX) {
        api->render(ctx->state.active_fx, &frame);
        first = 0;
      }
      kp_render_overlays(&frame, overlays, overlay_count, first);
      if (!ctx->all_leds) {
        for (size_t led = 0; led < ctx->leds_len; led++) {
          if (ctx->leds[led] < KP_LED_COUNT) {
            pixels[ctx->leds[led]] = scratch[ctx->leds[led]];
          }
        }
      }
    }
  }
  kp_rgb_matrix_unlock();
  if (any_on) {
    int err = led_strip_update_rgb(strip, pixels, KP_LED_COUNT);
    if (err < 0) {
      LOG_WRN("Failed to update the RGB strip (%d)", err);
    }
  }
  if (ovl_changed) {
    kp_rgb_overlay_dispatch();
  }
}

static void kp_rgb_matrix_off_handler(struct k_work *work) {
  ARG_UNUSED(work);
  if (!kp_rgb_has_leds()) {
    return;
  }
  /* Hold the lock across the write so a stale black frame cannot clobber ON. */
  kp_rgb_matrix_lock();
  if (!kp_any_on_locked()) {
    memset(pixels, 0, sizeof(pixels));
    int err = led_strip_update_rgb(strip, pixels, KP_LED_COUNT);
    if (err < 0) {
      LOG_WRN("Failed to clear the RGB strip (%d)", err);
    }
  }
  kp_rgb_matrix_unlock();
}
K_WORK_DEFINE(kp_off_work, kp_rgb_matrix_off_handler);
K_WORK_DEFINE(kp_tick_work, kp_rgb_matrix_tick);

void zmk_rgb_matrix_flush(void) {
  if (!kp_rgb_matrix_valid) {
    return;
  }
  if (!atomic_get(&kp_rgb_any_on)) {
    return;
  }
  k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &kp_tick_work);
}

/* A key event reaches every effect a behavior's active view reads: the active
 * effect itself, plus the effect each active overlay composites. A composited
 * reactive/ripple effect reads state its on_event fills, so without this it
 * would never see a keystroke.
 *
 * Presses and releases are both delivered; the callback's `ev->state` separates
 * them, so an effect that only cares about presses returns early itself.
 *
 * `seen` dedups by device so an effect that is both the active effect and an
 * overlay target, or shared by two overlays, is delivered once. The cap is
 * generous (one entry per overlay plus a few active effects); past it a
 * duplicate is possible but an event is never dropped. */
#define KP_RGB_EVENT_TARGETS_MAX (KP_RGB_OVERLAY_COUNT + 4)

static void kp_rgb_deliver_event(const struct device *dev,
                                 const struct zmk_position_state_changed *ev,
                                 const struct device **seen, size_t *seen_len) {
  for (size_t i = 0; i < *seen_len; i++) {
    if (seen[i] == dev) {
      return;
    }
  }
  const struct kp_rgb_effect_api *api =
      (const struct kp_rgb_effect_api *)dev->api;
  if (api->on_event != NULL) {
    api->on_event(dev, ev);
  }
  if (*seen_len < KP_RGB_EVENT_TARGETS_MAX) {
    seen[(*seen_len)++] = dev;
  }
}

/* Delivers one position event to every effect a behavior's active view reads:
 * the active effect, plus the effect each active overlay composites. The caller
 * holds kp_rgb_lock. */
static void
kp_rgb_deliver_position(const struct zmk_position_state_changed *ev) {
  size_t led = kp_rgb_led_for_position(ev->position);
  const struct device *seen[KP_RGB_EVENT_TARGETS_MAX];
  size_t seen_len = 0;
  for (size_t n = 0; n < kp_rgb_behavior_count(); n++) {
    struct kp_rgb_behavior_context *ctx = kp_rgb_behavior_at(n);
    if (ctx == NULL || !ctx->state.on || ctx->state.active_fx == NULL ||
        !kp_rgb_behavior_owns_led(ctx, led)) {
      continue;
    }
    const struct kp_rgb_effect_api *api =
        (const struct kp_rgb_effect_api *)ctx->state.active_fx->api;
    kp_rgb_deliver_event(ctx->state.active_fx, ev, seen, &seen_len);

    /* Deliver only while the overlay actually renders, matching the render
     * gate, so an inactive overlay does not accumulate stale state. */
    const struct device *const *overlays =
        api->overlays != NULL ? api->overlays : kp_rgb_overlay_list();
    size_t count =
        api->overlays != NULL ? api->overlays_len : kp_rgb_overlay_count();
    for (size_t i = 0; i < count; i++) {
      if (overlays[i] == NULL) {
        continue;
      }
      const struct kp_rgb_overlay_api *ovl = overlays[i]->api;
      if (ovl->event_target == NULL || !kp_rgb_overlay_gate(overlays[i], ovl)) {
        continue;
      }
      const struct device *target = ovl->event_target(overlays[i]);
      if (target != NULL) {
        kp_rgb_deliver_event(target, ev, seen, &seen_len);
      }
    }
  }
}

/* Prioritize key handling over RGB feedback: every event is copied here and
 * delivered by the low-priority queue. Drop events if the ring fills up. */
#define KP_RGB_EVENT_QUEUE_LEN 16

static struct zmk_position_state_changed
    kp_rgb_pending[KP_RGB_EVENT_QUEUE_LEN];
static uint8_t kp_rgb_pending_head;
static uint8_t kp_rgb_pending_tail;
static uint32_t kp_rgb_pending_dropped;
static struct k_spinlock kp_rgb_pending_lock;

static bool kp_rgb_pending_push(const struct zmk_position_state_changed *ev) {
  k_spinlock_key_t key = k_spin_lock(&kp_rgb_pending_lock);
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

static bool kp_rgb_pending_pop(struct zmk_position_state_changed *out) {
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

static void kp_rgb_matrix_pending_handler(struct k_work *work);
K_WORK_DEFINE(kp_pending_work, kp_rgb_matrix_pending_handler);

static void kp_rgb_matrix_pending_handler(struct k_work *work) {
  ARG_UNUSED(work);
  if (!kp_rgb_matrix_lock_patiently()) {
    /* Bounded wait failed; keep the events queued and try again. */
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &kp_pending_work);
    return;
  }
  struct zmk_position_state_changed ev;
  while (kp_rgb_pending_pop(&ev)) {
    if (atomic_get(&kp_rgb_any_on)) {
      kp_rgb_deliver_position(&ev);
    }
  }
  kp_rgb_matrix_unlock();

  uint32_t dropped = kp_rgb_pending_take_dropped();
  if (dropped > 0) {
    LOG_WRN("dropped %u RGB event(s): key-event queue full", dropped);
  }
}

#if IS_ENABLED(CONFIG_KEYPAW_RGB_MATRIX_AUTO_OFF_IDLE)
static void kp_rgb_permission_handler(struct k_work *work) {
  ARG_UNUSED(work);
  kp_rgb_matrix_lock();
  bool was_on = kp_any_on_locked();
  for (size_t n = 0; n < kp_rgb_behavior_count(); n++) {
    struct kp_rgb_behavior_context *ctx = kp_rgb_behavior_at(n);
    if (ctx != NULL) {
      ctx->state.on = kp_rgb_effective_on(ctx->state.user_on, true);
    }
  }
  kp_rgb_publish_any_on_locked();
  bool any_on = kp_any_on_locked();
  if (any_on) {
    kp_start_timer();
  } else {
    k_timer_stop(&kp_tick_timer);
  }
  kp_rgb_matrix_unlock();
  if (was_on && !any_on) {
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &kp_off_work);
  }
}
K_WORK_DEFINE(kp_permission_work, kp_rgb_permission_handler);
#endif

static int kp_rgb_matrix_event_listener(const zmk_event_t *eh) {
  const struct zmk_position_state_changed *pos_ev =
      as_zmk_position_state_changed(eh);
  if (pos_ev != NULL) {
    /* RGB may drop under sustained pressure; key handling must not. */
    if (atomic_get(&kp_rgb_any_on) && kp_rgb_pending_push(pos_ev)) {
      k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &kp_pending_work);
    }
    return ZMK_EV_EVENT_BUBBLE;
  }

#if IS_ENABLED(CONFIG_KEYPAW_RGB_MATRIX_AUTO_OFF_IDLE)
  if (as_zmk_activity_state_changed(eh) != NULL) {
    static bool is_awake = true;
    bool awake = zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE;
    if (awake != is_awake) {
      is_awake = awake;
      kp_rgb_permission_handler(NULL);
    }
    return ZMK_EV_EVENT_BUBBLE;
  }
#endif
  return -ENOTSUP;
}
ZMK_LISTENER(kp_rgb_matrix, kp_rgb_matrix_event_listener);
ZMK_SUBSCRIPTION(kp_rgb_matrix, zmk_position_state_changed);
#if IS_ENABLED(CONFIG_KEYPAW_RGB_MATRIX_AUTO_OFF_IDLE)
ZMK_SUBSCRIPTION(kp_rgb_matrix, zmk_activity_state_changed);
#endif

int zmk_rgb_matrix_on(const struct device *behavior) {
  struct kp_rgb_behavior_context *ctx =
      kp_rgb_behavior_context_from_device(behavior);
  if (ctx == NULL)
    return -ENODEV;
  int ret = KP_TRY_LOCK();
  if (ret < 0)
    return ret;
  bool was_on = kp_any_on_locked();
  ctx->state.on = kp_rgb_effective_on(true, IS_ENABLED(CONFIG_KEYPAW_POWER_FIRST));
  kp_rgb_publish_any_on_locked();
  bool any_on = kp_any_on_locked();
  if (!was_on && any_on) {
    kp_start_timer();
  } else if (!any_on) {
    k_timer_stop(&kp_tick_timer);
  }
  kp_rgb_matrix_unlock();
  if (was_on && !any_on) {
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &kp_off_work);
  }
  return 0;
}

int zmk_rgb_matrix_off(const struct device *behavior) {
  struct kp_rgb_behavior_context *ctx =
      kp_rgb_behavior_context_from_device(behavior);
  if (ctx == NULL)
    return -ENODEV;
  int ret = KP_TRY_LOCK();
  if (ret < 0)
    return ret;
  bool was_on = kp_any_on_locked();
  ctx->state.on = false;
  kp_rgb_publish_any_on_locked();
  bool any_on = kp_any_on_locked();
  if (!any_on)
    k_timer_stop(&kp_tick_timer);
  kp_rgb_matrix_unlock();
  if (was_on && !any_on)
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &kp_off_work);
  return 0;
}

int zmk_rgb_matrix_toggle(const struct device *behavior) {
  struct kp_rgb_behavior_context *ctx =
      kp_rgb_behavior_context_from_device(behavior);
  if (ctx == NULL)
    return -ENODEV;
  kp_rgb_matrix_lock();
  bool on = ctx->state.on;
  kp_rgb_matrix_unlock();
  return on ? zmk_rgb_matrix_off(behavior) : zmk_rgb_matrix_on(behavior);
}

int zmk_rgb_matrix_get_state(const struct device *behavior, bool *on_off) {
  struct kp_rgb_behavior_context *ctx =
      kp_rgb_behavior_context_from_device(behavior);
  if (ctx == NULL || on_off == NULL)
    return -EINVAL;
  kp_rgb_matrix_lock();
  *on_off = ctx->state.on;
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

static int kp_rgb_validate_zones(void) {
  kp_rgb_matrix_valid = true;
  /* With no LEDs there are no zones to partition; accepting every behavior
   * keeps the central-only machinery enabled on a strip-less half. */
  if (!kp_rgb_has_leds()) {
    return 0;
  }
  for (size_t i = 0; i < kp_rgb_behavior_count(); i++) {
    struct kp_rgb_behavior_context *ctx = kp_rgb_behavior_at(i);
    if (ctx == NULL)
      continue;
    ctx->zone_valid = true;
    if (ctx->all_leds && i != 0) {
      LOG_ERR(
          "RGB behavior %s has an all-LED zone overlapping another behavior",
          ctx->dev->name);
      ctx->zone_valid = false;
      kp_rgb_matrix_valid = false;
    }
    for (size_t j = 0; j < ctx->leds_len; j++) {
      if (ctx->leds[j] >= KP_LED_COUNT) {
        LOG_ERR("RGB behavior %s claims out-of-range LED %u", ctx->dev->name,
                (uint32_t)ctx->leds[j]);
        ctx->zone_valid = false;
        kp_rgb_matrix_valid = false;
      }
      for (size_t k = 0; k < j; k++) {
        if (ctx->leds[j] == ctx->leds[k]) {
          LOG_ERR("RGB behavior %s claims LED %u more than once",
                  ctx->dev->name, (uint32_t)ctx->leds[j]);
          ctx->zone_valid = false;
          kp_rgb_matrix_valid = false;
        }
      }
    }
    for (size_t prior = 0; prior < i; prior++) {
      struct kp_rgb_behavior_context *other = kp_rgb_behavior_at(prior);
      if (other == NULL)
        continue;
      bool overlap =
          (ctx->all_leds && (other->all_leds || other->leds_len > 0)) ||
          (other->all_leds && ctx->leds_len > 0);
      if (!overlap && !ctx->all_leds && !other->all_leds) {
        for (size_t a = 0; a < ctx->leds_len && !overlap; a++) {
          for (size_t b = 0; b < other->leds_len; b++) {
            if (ctx->leds[a] == other->leds[b])
              overlap = true;
          }
        }
      }
      if (overlap) {
        LOG_ERR("RGB behavior zones %s and %s overlap", ctx->dev->name,
                other->dev->name);
        ctx->zone_valid = false;
        other->zone_valid = false;
        kp_rgb_matrix_valid = false;
      }
    }
  }
  return kp_rgb_matrix_valid ? 0 : -EINVAL;
}

static int kp_rgb_matrix_init(void) {
  if (kp_rgb_has_leds() && !device_is_ready(strip)) {
    LOG_ERR("LED strip \"%s\" is not ready", strip->name);
    return -ENODEV;
  }
  if (kp_rgb_has_leds()) {
    memset(pixels, 0, sizeof(pixels));
    int err = led_strip_update_rgb(strip, pixels, KP_LED_COUNT);
    if (err < 0) {
      LOG_WRN("Failed to clear the RGB strip (%d)", err);
    }
  }
  if (kp_rgb_validate_zones() < 0) {
    LOG_ERR("RGB matrix disabled because LED zones are invalid");
    return -EINVAL;
  }
  kp_rgb_matrix_lock();
  for (size_t n = 0; n < kp_rgb_behavior_count(); n++) {
    struct kp_rgb_behavior_context *ctx = kp_rgb_behavior_at(n);
    if (ctx != NULL) {
      ctx->state.on = kp_rgb_effective_on(
          ctx->state.on, IS_ENABLED(CONFIG_KEYPAW_POWER_FIRST));
    }
  }
  kp_rgb_publish_any_on_locked();
  bool any_on = kp_any_on_locked();
  kp_rgb_matrix_unlock();
  if (any_on) {
    kp_start_timer();
  }
  return 0;
}
SYS_INIT(kp_rgb_matrix_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
