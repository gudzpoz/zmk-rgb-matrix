/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Position-aware RGB matrix engine. Modelled on zmk/app/src/rgb_underglow.c:
 * a singleton driven by a k_timer submitting to ZMK's low priority work queue.
 *
 * The engine mostly just delegates work (and event handling) to the current
 * active effect and does not known about anything else. Effect switching is
 * handled by behavior_rgb_matrix.c.
 */

#define DT_DRV_COMPAT keypaw_rgb_matrix

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/led_strip.h>

#include <zmk/activity.h>
#include <zmk/rgb_matrix.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/workqueue.h>
#include <zmk/physical_layouts.h>

#if IS_ENABLED(CONFIG_KEYPAW_RGB_MATRIX_AUTO_OFF_IDLE)
#include <zmk/events/activity_state_changed.h>
#endif

#include "rgb_matrix_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define KP_LAYOUT DT_PHANDLE(KP_RGB_NODE, physical_layout)
#define KP_NKEYS DT_PROP_LEN(KP_LAYOUT, keys)

BUILD_ASSERT(DT_PROP_LEN(KP_RGB_NODE, mapping) == KP_LED_COUNT,
             "keypaw,rgb-matrix: 'mapping' must hold exactly one entry per LED");

#define KP_MAP_ENTRY_ASSERT(node_id, prop, idx)                         \
  BUILD_ASSERT((DT_PROP_BY_IDX(node_id, prop, idx) & KP_RGB_NO_KEY_XY_FLAG) || \
               DT_PROP_BY_IDX(node_id, prop, idx) < KP_NKEYS,           \
               "keypaw,rgb-matrix: 'mapping' entry is neither KP_RGB_NO_KEY(x, y) " \
               "nor a valid key position");
DT_FOREACH_PROP_ELEM(KP_RGB_NODE, mapping, KP_MAP_ENTRY_ASSERT);

/* Raw mapping as written in the devicetree: key position, or packed (x, y). */
static const uint32_t kp_map[KP_LED_COUNT] = DT_PROP(KP_RGB_NODE, mapping);
/* Resolved LED centres, re-based so both halves start at (0, 0). Published to the
 * effects through the frame and kp_rgb_led_coord(). */
static struct kp_rgb_coord kp_led_coords[KP_LED_COUNT];
static uint16_t kp_rgb_board_length;
static uint16_t kp_rgb_board_height;
/* Key index to LED index mapping */
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
      /* Rotation is ignored to save some computation. */
      x = key->x + key->width / 2;
      y = key->y + key->height / 2;
      kp_key_to_led[k] = i;
    } else {
      LOG_ERR("mapping key %u out of range (layout has %u keys)", k,
              (uint32_t)layout->keys_len);
      x = 0;
      y = 0;
    }

    kp_led_coords[i] = (struct kp_rgb_coord){.x = (uint16_t)x, .y = (uint16_t)y};
    min_x = MIN(min_x, (uint16_t)x);
    max_x = MAX(max_x, (uint16_t)x);
    min_y = MIN(min_y, (uint16_t)y);
    max_y = MAX(max_y, (uint16_t)y);
  }

  /* Re-base the half so effects see it as an origin-anchored board: the two
   * halves occupy disjoint layout x offsets, and only relative geometry is
   * interesting to them. */
  for (size_t i = 0; i < KP_LED_COUNT; i++) {
    kp_led_coords[i].x -= min_x;
    kp_led_coords[i].y -= min_y;
  }

  /* Never 0 to save some effects from zero division. */
  kp_rgb_board_length = MAX((uint16_t)MAX(max_x - min_x, max_y - min_y), 100);
  kp_rgb_board_height = MAX((uint16_t)(max_y - min_y), 100);
  LOG_DBG("RGB matrix half extent %u x %u layout units", kp_rgb_board_length,
          kp_rgb_board_height);
}

size_t kp_rgb_led_for_position(uint32_t position) {
  if (position >= KP_NKEYS) {
    return SIZE_MAX;
  }
  return kp_key_to_led[position];
}

const struct kp_rgb_coord *kp_rgb_led_coord(size_t led) {
  return led < KP_LED_COUNT ? &kp_led_coords[led] : NULL;
}

static const struct device *const strip = DEVICE_DT_GET(KP_RGB_STRIP);
struct kp_rgb_state kp_rgb_state;
static struct led_rgb pixels[KP_LED_COUNT];
static uint32_t last_tick;
static struct k_mutex kp_rgb_lock;

/* A devicetree array cannot be empty, so "this effect wants no indicators" is
 * expressed by pointing at this zero-length list instead. */
const struct device *const kp_rgb_no_indicators[1] = {NULL};

/* Indicators composited over every effect, in paint order, unless the active
 * effect overrides the list. */
COND_CODE_1(DT_NODE_HAS_PROP(KP_RGB_NODE, indicators),
            (static const struct device *const kp_rgb_indicators[] = {
                 LISTIFY(DT_PROP_LEN(KP_RGB_NODE, indicators),
                         KP_RGB_INDICATORS_AT_IDX, (,), KP_RGB_NODE)};),
            ())
static const struct device *const *const kp_rgb_default_indicators =
    COND_CODE_1(DT_NODE_HAS_PROP(KP_RGB_NODE, indicators), (kp_rgb_indicators),
                (kp_rgb_no_indicators));
static const size_t kp_rgb_default_indicator_count =
    DT_PROP_LEN_OR(KP_RGB_NODE, indicators, 0);

#define KP_TRY_LOCK() k_mutex_lock(&kp_rgb_lock, K_MSEC(CONFIG_KEYPAW_RGB_MATRIX_TICK_MS))

void kp_rgb_matrix_lock(void) { k_mutex_lock(&kp_rgb_lock, K_FOREVER); }

void kp_rgb_matrix_unlock(void) { k_mutex_unlock(&kp_rgb_lock); }

static void kp_rgb_matrix_tick(struct k_work *work);
K_WORK_DEFINE(kp_tick_work, kp_rgb_matrix_tick);
static void kp_rgb_matrix_tick_handler(struct k_timer *timer) {
  if (!kp_rgb_state.on) {
    return;
  }
  k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &kp_tick_work);
}

K_TIMER_DEFINE(kp_tick_timer, kp_rgb_matrix_tick_handler, NULL);
static void kp_start_timer(void) {
  last_tick = k_uptime_get_32();
  k_timer_start(&kp_tick_timer, K_NO_WAIT, K_MSEC(CONFIG_KEYPAW_RGB_MATRIX_TICK_MS));
}

static void kp_rgb_matrix_tick(struct k_work *work) {
  ARG_UNUSED(work);

  if (!kp_rgb_state.on) {
    return;
  }

  uint32_t now = k_uptime_get_32();
  uint32_t elapsed = now - last_tick;
  last_tick = now;

  struct kp_rgb_frame frame = {
    .tune = &kp_rgb_tuning,
    .count = KP_LED_COUNT,
    .coords = kp_led_coords,
    .pixels = pixels,
    .elapsed = elapsed,
    .board_length = kp_rgb_board_length,
    .board_height = kp_rgb_board_height,
    .is_idle = zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE,
  };

  int ret = KP_TRY_LOCK();
  if (ret < 0) {
    LOG_WRN("Failed to obtain RGB matrix lock");
    return;
  }
  if (kp_rgb_state.active_fx != NULL) {
    const struct kp_rgb_effect_api *api =
      (const struct kp_rgb_effect_api *)kp_rgb_state.active_fx->api;
    api->render(kp_rgb_state.active_fx, &frame);

    /* An effect may replace the matrix-wide list, or ask for none at all. */
    const struct device *const *indicators =
      api->indicators != NULL ? api->indicators : kp_rgb_default_indicators;
    size_t count =
      api->indicators != NULL ? api->indicators_len : kp_rgb_default_indicator_count;
    for (size_t i = 0; i < count; i++) {
      const struct kp_rgb_indicator_api *ind =
        (const struct kp_rgb_indicator_api *)indicators[i]->api;
      ind->render(indicators[i], &frame);
    }
  }
  k_mutex_unlock(&kp_rgb_lock);

  int err = led_strip_update_rgb(strip, pixels, KP_LED_COUNT);
  if (err < 0) {
    LOG_WRN("Failed to update the RGB strip (%d)", err);
  }
}

static int kp_rgb_matrix_event_listener(const zmk_event_t *eh) {
  const struct zmk_position_state_changed *pos_ev = as_zmk_position_state_changed(eh);
  if (pos_ev != NULL) {
    if (pos_ev->state && kp_rgb_state.on && kp_rgb_state.active_fx != NULL) {
      const struct kp_rgb_effect_api *api =
        (const struct kp_rgb_effect_api *)kp_rgb_state.active_fx->api;
      if (api->on_event != NULL) {
        if (KP_TRY_LOCK() == 0) {
          api->on_event(kp_rgb_state.active_fx, eh);
          k_mutex_unlock(&kp_rgb_lock);
        } else {
          LOG_WRN("Failed to acquire RGB matrix lock");
        }
      }
    }
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
    if (is_awake) {
      if (kp_rgb_state.user_on) {
        zmk_rgb_matrix_on();
      }
    } else if (kp_rgb_state.on) {
      zmk_rgb_matrix_off();
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

static void kp_rgb_matrix_off_handler(struct k_work *work) {
  ARG_UNUSED(work);

  for (size_t i = 0; i < KP_LED_COUNT; i++) {
    pixels[i] = (struct led_rgb){.r = 0, .g = 0, .b = 0};
  }

  led_strip_update_rgb(strip, pixels, KP_LED_COUNT);
}

K_WORK_DEFINE(kp_off_work, kp_rgb_matrix_off_handler);

int zmk_rgb_matrix_on(void) {
  int ret = KP_TRY_LOCK();
  if (ret < 0) {
    return ret;
  }
  kp_start_timer();
  kp_rgb_state.on = true;
  k_mutex_unlock(&kp_rgb_lock);
  return 0;
}

int zmk_rgb_matrix_off(void) {
  k_timer_stop(&kp_tick_timer);
  k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &kp_off_work);

  int ret = KP_TRY_LOCK();
  if (ret < 0) {
    return ret;
  }
  kp_rgb_state.on = false;
  k_mutex_unlock(&kp_rgb_lock);
  return 0;
}

int zmk_rgb_matrix_toggle(void) {
  return kp_rgb_state.on ? zmk_rgb_matrix_off() : zmk_rgb_matrix_on();
}

int zmk_rgb_matrix_get_state(bool *on_off) {
  *on_off = kp_rgb_state.on;
  return 0;
}

/* Resolve the key -> LED table before any indicator device initialises: an
 * indicator's `keys` spec is resolved through kp_rgb_led_for_position() from its
 * POST_KERNEL init, and this runs at the OBJECTS priority, ahead of the default
 * device priority. Only static data is needed (the devicetree mapping and the
 * physical layout list), so this does not have to wait for the strip or the
 * keymap. */
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

SYS_INIT(kp_rgb_matrix_layout_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_OBJECTS);

static int kp_rgb_matrix_init(void) {
  if (!device_is_ready(strip)) {
    LOG_ERR("LED strip \"%s\" is not ready", strip->name);
    return -ENODEV;
  }

  /* kp_rgb_state.on and the active effect are set up by the behavior module at
   * POST_KERNEL, which runs before APPLICATION. */
  if (kp_rgb_state.on) {
    kp_start_timer();
  }

  return 0;
}

SYS_INIT(kp_rgb_matrix_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
