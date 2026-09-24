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

static const struct device *const strip = DEVICE_DT_GET(KP_RGB_STRIP);
static struct led_rgb pixels[KP_LED_COUNT];
static struct led_rgb scratch[KP_LED_COUNT];
static uint32_t last_tick;
static struct k_mutex kp_rgb_lock;
static bool kp_rgb_matrix_valid;

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

bool kp_rgb_behavior_any_on(void) { return kp_any_on_locked(); }
#define KP_TRY_LOCK()                                                          \
  k_mutex_lock(&kp_rgb_lock, K_MSEC(CONFIG_KEYPAW_RGB_MATRIX_TICK_MS))

void kp_rgb_matrix_lock(void) { k_mutex_lock(&kp_rgb_lock, K_FOREVER); }
void kp_rgb_matrix_unlock(void) { k_mutex_unlock(&kp_rgb_lock); }

/* The lock is never held across flash I/O (rgb_settings.c encodes under it and
 * saves outside it) and every holder runs at a higher priority than this
 * low-priority queue, so a busy mutex is a hiccup rather than a deadlock. Wait
 * it out instead of dropping the frame: a dropped frame also skips the
 * overlay dispatch that shares the tick, costing a full period on both
 * halves. Bounded, so a genuinely stuck holder still surfaces a warning
 * instead of hanging this queue forever. */
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
static void kp_rgb_matrix_tick_handler(struct k_timer *timer) {
  ARG_UNUSED(timer);
  kp_rgb_matrix_lock();
  bool on = kp_any_on_locked();
  kp_rgb_matrix_unlock();
  if (on) {
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &kp_tick_work);
  }
}

K_TIMER_DEFINE(kp_tick_timer, kp_rgb_matrix_tick_handler, NULL);

static void kp_start_timer(void) {
  last_tick = k_uptime_get_32();
  k_timer_start(&kp_tick_timer, K_NO_WAIT,
                K_MSEC(CONFIG_KEYPAW_RGB_MATRIX_TICK_MS));
}

static void kp_render_overlays(struct kp_rgb_behavior_context *ctx,
                               struct kp_rgb_frame *frame,
                               const struct kp_rgb_effect_api *api) {
  const struct device *const *overlays =
      api->overlays != NULL ? api->overlays : ctx->overlays;
  size_t count =
      api->overlays != NULL ? api->overlays_len : ctx->overlays_len;
  for (size_t i = 0; i < count; i++) {
    if (overlays[i] == NULL) {
      continue;
    }
    const struct kp_rgb_overlay_api *ovl = overlays[i]->api;
    if (!kp_rgb_overlay_gate(overlays[i], ovl)) {
      continue;
    }
    ovl->render(overlays[i], frame);
  }
}

static void kp_rgb_matrix_tick(struct k_work *work) {
  ARG_UNUSED(work);
  if (!kp_rgb_matrix_valid) {
    return;
  }
  uint32_t now = k_uptime_get_32();
  uint32_t elapsed = now - last_tick;
  last_tick = now;
  /* Resolve every central-authoritative overlay before rendering */
  bool ovl_changed = kp_rgb_overlay_refresh();
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
      memset(scratch, 0, sizeof(scratch));
      struct kp_rgb_frame frame = {
          .tune = &ctx->tuning,
          .count = KP_LED_COUNT,
          .coords = kp_led_coords,
          .pixels = scratch,
          .elapsed = elapsed,
          .board_length = kp_rgb_board_length,
          .board_height = kp_rgb_board_height,
          .is_idle = zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE,
      };
      const struct kp_rgb_effect_api *api =
          (const struct kp_rgb_effect_api *)ctx->state.active_fx->api;
      api->render(ctx->state.active_fx, &frame);
      kp_render_overlays(ctx, &frame, api);
      if (ctx->all_leds) {
        memcpy(pixels, scratch, sizeof(pixels));
      } else {
        for (size_t led = 0; led < ctx->leds_len; led++) {
          if (ctx->leds[led] < KP_LED_COUNT) {
            pixels[ctx->leds[led]] = scratch[ctx->leds[led]];
          }
        }
      }
    }
  }
  kp_rgb_matrix_unlock();
  /* Paint locally before pushing; the split send can block on a full run queue,
   * and the local LEDs must not wait behind it. */
  if (any_on) {
    int err = led_strip_update_rgb(strip, pixels, KP_LED_COUNT);
    if (err < 0) {
      LOG_WRN("Failed to update the RGB strip (%d)", err);
    }
  }
  /* Push the new overlay state after the local paint. */
  if (ovl_changed) {
    kp_rgb_overlay_dispatch();
  }
}

static void kp_rgb_matrix_off_handler(struct k_work *work) {
  ARG_UNUSED(work);
  memset(pixels, 0, sizeof(pixels));
  led_strip_update_rgb(strip, pixels, KP_LED_COUNT);
}
K_WORK_DEFINE(kp_off_work, kp_rgb_matrix_off_handler);
K_WORK_DEFINE(kp_tick_work, kp_rgb_matrix_tick);

void zmk_rgb_matrix_flush(void) {
  if (!kp_rgb_matrix_valid) {
    return;
  }
  /* No lock on purpose: `on` only decides whether a frame is worth scheduling,
   * a racy read costs at most one redundant frame, and this runs from the split
   * RX and event contexts where blocking would be worse. Concurrent callers
   * need no dedup of their own -- k_work_submit_to_queue() does nothing when
   * the work is already queued (returns 0) and re-queues it, one extra frame,
   * only while it is running (returns 2). The return value is ignored, as in
   * the timer handler below. */
  if (!kp_any_on_locked()) {
    return;
  }
  k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &kp_tick_work);
}

static int kp_rgb_matrix_event_listener(const zmk_event_t *eh) {
  const struct zmk_position_state_changed *pos_ev =
      as_zmk_position_state_changed(eh);
  if (pos_ev != NULL) {
    if (!pos_ev->state || KP_TRY_LOCK() < 0) {
      return ZMK_EV_EVENT_BUBBLE;
    }
    size_t led = kp_rgb_led_for_position(pos_ev->position);
    for (size_t n = 0; n < kp_rgb_behavior_count(); n++) {
      struct kp_rgb_behavior_context *ctx = kp_rgb_behavior_at(n);
      if (ctx == NULL || !ctx->state.on || ctx->state.active_fx == NULL ||
          !kp_rgb_behavior_owns_led(ctx, led)) {
        continue;
      }
      const struct kp_rgb_effect_api *api =
          (const struct kp_rgb_effect_api *)ctx->state.active_fx->api;
      if (api->on_event != NULL) {
        api->on_event(ctx->state.active_fx, eh);
      }
    }
    kp_rgb_matrix_unlock();
    return ZMK_EV_EVENT_BUBBLE;
  }

#if IS_ENABLED(CONFIG_KEYPAW_RGB_MATRIX_AUTO_OFF_IDLE)
  if (as_zmk_activity_state_changed(eh) != NULL) {
    static bool is_awake = true;
    bool wakening = zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE;
    if (is_awake == wakening) {
      return ZMK_EV_EVENT_BUBBLE;
    }
    is_awake = wakening;
    kp_rgb_matrix_lock();
    for (size_t n = 0; n < kp_rgb_behavior_count(); n++) {
      struct kp_rgb_behavior_context *ctx = kp_rgb_behavior_at(n);
      if (ctx != NULL) {
        ctx->state.on = wakening ? ctx->state.user_on : false;
      }
    }
    bool any_on = kp_any_on_locked();
    if (any_on) {
      kp_start_timer();
    } else {
      k_timer_stop(&kp_tick_timer);
    }
    kp_rgb_matrix_unlock();
    if (!any_on) {
      k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &kp_off_work);
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
  ctx->state.on = true;
  if (!was_on)
    kp_start_timer();
  kp_rgb_matrix_unlock();
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
  ctx->state.on = false;
  bool any_on = kp_any_on_locked();
  if (!any_on)
    k_timer_stop(&kp_tick_timer);
  kp_rgb_matrix_unlock();
  if (!any_on)
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
  if (!device_is_ready(strip)) {
    LOG_ERR("LED strip \"%s\" is not ready", strip->name);
    return -ENODEV;
  }
  if (kp_rgb_validate_zones() < 0) {
    LOG_ERR("RGB matrix disabled because LED zones are invalid");
    return -EINVAL;
  }
  kp_rgb_matrix_lock();
  bool any_on = kp_any_on_locked();
  kp_rgb_matrix_unlock();
  if (any_on)
    kp_start_timer();
  return 0;
}
SYS_INIT(kp_rgb_matrix_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
