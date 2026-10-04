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

#if IS_ENABLED(CONFIG_KEYPAW_RGB_MATRIX_AUTO_OFF_IDLE)
#include <zmk/events/activity_state_changed.h>
#endif

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
static struct led_rgb pixels[KP_LED_COUNT];
static uint32_t last_tick;
static K_MUTEX_DEFINE(kp_rgb_lock);
static atomic_t kp_rgb_any_on;
static atomic_t kp_rgb_inhibited;
static bool kp_rgb_output_ready;
static bool kp_rgb_black_pending;
static uint32_t kp_rgb_black_retry_ms = 100;

static void kp_rgb_request_black_locked(void);
static void kp_rgb_pending_purge(void);

/* A half with no strip renders nothing, but the central-only overlay and split
 * machinery still runs. */
static inline bool kp_rgb_has_leds(void) { return KP_LED_COUNT > 0; }

const struct device *const kp_rgb_no_overlays[1] = {NULL};

static bool kp_any_on_locked(void) {
  return kp_rgb_controller.state.on;
}

/* Caller holds kp_rgb_lock; timer ISR reads the atomic mirror. */
static void kp_rgb_publish_any_on_locked(void) {
  bool was_on = atomic_get(&kp_rgb_any_on);
  bool on = kp_any_on_locked();
  atomic_set(&kp_rgb_any_on, on);
  if (on && !atomic_get(&kp_rgb_inhibited)) {
    kp_rgb_black_pending = false;
  } else if (was_on && !on) {
    kp_rgb_request_black_locked();
  }
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
 * re-checks controller state under the lock. */
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
    if (kp_rgb_overlay_gate(dev)) {
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
    if (!kp_rgb_overlay_gate(dev)) {
      continue;
    }
    ovl->render(dev, frame);
  }
}

static void kp_rgb_matrix_tick(struct k_work *work) {
  ARG_UNUSED(work);
  if (!kp_rgb_output_ready || !atomic_get(&kp_rgb_any_on)) {
    return;
  }
  if (!kp_rgb_has_leds()) {
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
  uint32_t now = k_uptime_get_32();
  uint32_t elapsed = now - last_tick;
  last_tick = now;
  any_on = kp_any_on_locked() && !atomic_get(&kp_rgb_inhibited);
  memset(pixels, 0, sizeof(pixels));
  const struct device *fx = kp_rgb_controller.state.active_fx;
  if (any_on && fx != NULL) {
    struct kp_rgb_frame frame = {
        .tune = &kp_rgb_controller.tuning,
        .count = KP_LED_COUNT,
        .coords = kp_led_coords,
        .pixels = pixels,
        .elapsed = elapsed,
        .board_length = kp_rgb_board_length,
        .board_height = kp_rgb_board_height,
        .is_idle = zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE,
    };
    const struct kp_rgb_effect_api *api = fx->api;
    const struct device *const *overlays =
        api->overlays != NULL ? api->overlays : kp_rgb_overlay_list();
    size_t overlay_count =
        api->overlays != NULL ? api->overlays_len : kp_rgb_overlay_count();
    size_t first = kp_last_covering_overlay(overlays, overlay_count);
    if (first == SIZE_MAX) {
      api->render(fx, &frame);
      first = 0;
    }
    kp_render_overlays(&frame, overlays, overlay_count, first);
  }
  /* Callbacks may inhibit output while this recursive mutex is held. */
  if (any_on && !atomic_get(&kp_rgb_inhibited)) {
    int err = led_strip_update_rgb(strip, pixels, KP_LED_COUNT);
    if (err < 0) {
      LOG_WRN("Failed to update the RGB strip (%d)", err);
    }
  }
  kp_rgb_matrix_unlock();
}

static void kp_rgb_matrix_off_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(kp_off_work, kp_rgb_matrix_off_handler);

static void kp_rgb_request_black_locked(void) {
  if (!kp_rgb_output_ready || !kp_rgb_has_leds()) {
    return;
  }
  kp_rgb_black_pending = true;
  kp_rgb_black_retry_ms = 100;
  k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &kp_off_work,
                              K_NO_WAIT);
}

static void kp_rgb_matrix_off_handler(struct k_work *work) {
  ARG_UNUSED(work);
  kp_rgb_matrix_lock();
  if (kp_rgb_black_pending &&
      (!kp_any_on_locked() || atomic_get(&kp_rgb_inhibited))) {
    /* led_strip_update_rgb() may overwrite even a failed submission. */
    memset(pixels, 0, sizeof(pixels));
    int err = led_strip_update_rgb(strip, pixels, KP_LED_COUNT);
    if (err < 0) {
      LOG_WRN("Failed to clear the RGB strip (%d)", err);
      k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &kp_off_work,
                                  K_MSEC(kp_rgb_black_retry_ms));
      kp_rgb_black_retry_ms = MIN(kp_rgb_black_retry_ms * 2, 1000);
    } else {
      kp_rgb_black_pending = false;
    }
  } else {
    kp_rgb_black_pending = false;
  }
  kp_rgb_matrix_unlock();
}

int zmk_rgb_matrix_set_inhibited(bool inhibited) {
  if (k_is_in_isr()) {
    return -EWOULDBLOCK;
  }
  kp_rgb_matrix_lock();
  if (atomic_get(&kp_rgb_inhibited) != inhibited) {
    /* Release must not purge newly admitted feedback. */
    kp_rgb_pending_purge();
    atomic_set(&kp_rgb_inhibited, inhibited);
    last_tick = k_uptime_get_32();
    if (inhibited || !kp_any_on_locked()) {
      kp_rgb_request_black_locked();
    } else {
      kp_rgb_black_pending = false;
      zmk_rgb_matrix_flush();
    }
  }
  kp_rgb_matrix_unlock();
  return 0;
}

bool zmk_rgb_matrix_is_inhibited(void) {
  return atomic_get(&kp_rgb_inhibited) != 0;
}
K_WORK_DEFINE(kp_tick_work, kp_rgb_matrix_tick);

void zmk_rgb_matrix_flush(void) {
  if (!kp_rgb_output_ready) {
    return;
  }
  if (!atomic_get(&kp_rgb_any_on)) {
    return;
  }
  k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &kp_tick_work);
}

static void kp_rgb_deliver_event(const struct device *dev,
                                 const struct zmk_position_state_changed *ev) {
  const struct kp_rgb_effect_api *api = dev->api;
  if (api->on_event != NULL) {
    api->on_event(dev, ev);
  }
}

/* Caller holds kp_rgb_lock. */
static void kp_rgb_deliver_position(const struct zmk_position_state_changed *ev) {
  const struct device *fx = kp_rgb_controller.state.active_fx;
  if (!kp_rgb_controller.state.on || fx == NULL ||
      kp_rgb_led_for_position(ev->position) == SIZE_MAX) {
    return;
  }
  const struct kp_rgb_effect_api *api = fx->api;
  kp_rgb_deliver_event(fx, ev);

  const struct device *const *overlays =
      api->overlays != NULL ? api->overlays : kp_rgb_overlay_list();
  size_t count = api->overlays != NULL ? api->overlays_len : kp_rgb_overlay_count();
  for (size_t i = 0; i < count; i++) {
    if (overlays[i] == NULL) {
      continue;
    }
    const struct kp_rgb_overlay_api *ovl = overlays[i]->api;
    if (ovl->event_target == NULL || !kp_rgb_overlay_gate(overlays[i])) {
      continue;
    }
    const struct device *target = ovl->event_target(overlays[i]);
    if (target != NULL) {
      kp_rgb_deliver_event(target, ev);
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

static void kp_rgb_pending_purge(void) {
  k_spinlock_key_t key = k_spin_lock(&kp_rgb_pending_lock);
  kp_rgb_pending_tail = kp_rgb_pending_head;
  kp_rgb_pending_dropped = 0;
  k_spin_unlock(&kp_rgb_pending_lock, key);
}

static bool kp_rgb_pending_push(const struct zmk_position_state_changed *ev) {
  k_spinlock_key_t key = k_spin_lock(&kp_rgb_pending_lock);
  if (atomic_get(&kp_rgb_inhibited)) {
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
    if (atomic_get(&kp_rgb_any_on) && !atomic_get(&kp_rgb_inhibited)) {
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
  kp_rgb_controller.state.on =
      kp_rgb_effective_on(kp_rgb_controller.state.user_on, true);
  kp_rgb_publish_any_on_locked();
  bool any_on = kp_any_on_locked();
  if (any_on) {
    kp_start_timer();
  } else {
    k_timer_stop(&kp_tick_timer);
  }
  kp_rgb_matrix_unlock();
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

int zmk_rgb_matrix_on(void) {
  int ret = KP_TRY_LOCK();
  if (ret < 0)
    return ret;
  bool was_on = kp_any_on_locked();
  kp_rgb_controller.state.on = kp_rgb_effective_on(
      true, IS_ENABLED(CONFIG_KEYPAW_RGB_MATRIX_AUTO_OFF_IDLE));
  kp_rgb_publish_any_on_locked();
  bool any_on = kp_any_on_locked();
  if (!was_on && any_on) {
    kp_start_timer();
  } else if (!any_on) {
    k_timer_stop(&kp_tick_timer);
  }
  kp_rgb_matrix_unlock();
  return 0;
}

int zmk_rgb_matrix_off(void) {
  int ret = KP_TRY_LOCK();
  if (ret < 0)
    return ret;
  kp_rgb_controller.state.on = false;
  kp_rgb_publish_any_on_locked();
  bool any_on = kp_any_on_locked();
  if (!any_on)
    k_timer_stop(&kp_tick_timer);
  kp_rgb_matrix_unlock();
  return 0;
}

int zmk_rgb_matrix_toggle(void) {
  kp_rgb_matrix_lock();
  bool on = kp_rgb_controller.state.on;
  kp_rgb_matrix_unlock();
  return on ? zmk_rgb_matrix_off() : zmk_rgb_matrix_on();
}

int zmk_rgb_matrix_get_state(bool *on_off) {
  if (on_off == NULL)
    return -EINVAL;
  kp_rgb_matrix_lock();
  *on_off = kp_rgb_controller.state.on;
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
    memset(pixels, 0, sizeof(pixels));
    int err = led_strip_update_rgb(strip, pixels, KP_LED_COUNT);
    if (err < 0) {
      LOG_WRN("Failed to clear the RGB strip (%d)", err);
      kp_rgb_black_pending = true;
      kp_rgb_black_retry_ms = 200;
      k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &kp_off_work,
                                  K_MSEC(100));
    }
  }
  kp_rgb_controller.state.on = kp_rgb_effective_on(
      kp_rgb_controller.state.on,
      IS_ENABLED(CONFIG_KEYPAW_RGB_MATRIX_AUTO_OFF_IDLE));
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
