/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * The charge is read per half, so each half shows its own battery.
 */

#define DT_DRV_COMPAT keypaw_rgb_overlay_battery

#include <stdbool.h>
#include <stddef.h>

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#include <zmk/battery.h>
#include <zmk/rgb_matrix.h>

#include "../rgb_matrix_internal.h"

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define KP_BATTERY_BAR_COUNT(inst) + DT_INST_ENUM_HAS_VALUE(inst, mode, bar)
#define KP_BATTERY_HAS_BAR (0 DT_INST_FOREACH_STATUS_OKAY(KP_BATTERY_BAR_COUNT))

struct kp_ovl_battery_config {
  struct kp_rgb_overlay_common_config common;
  uint32_t color;                 /* 0xRRGGBB */
  uint32_t background_color;      /* 0xRRGGBB */
  bool reverse;
  bool hue;
  uint16_t hue_empty;
  uint16_t hue_full;
};
struct kp_ovl_battery_data {
  struct kp_rgb_overlay_common_data common;
  size_t *canonical_order;
  bool canonical_order_ready;
};

#if KP_LED_COUNT > 0 && KP_BATTERY_HAS_BAR
/* Reading order: the longer board axis first, then the other; `reverse` flips it
 * so the bar can fill from either end. */
static bool kp_ovl_battery_before(const struct kp_rgb_frame *frame, size_t a,
                                  size_t b, bool x_primary, bool reverse) {
  const struct kp_rgb_coord *ca = &frame->coords[a];
  const struct kp_rgb_coord *cb = &frame->coords[b];

  if (x_primary) {
    if (ca->x != cb->x) {
      return reverse ? ca->x > cb->x : ca->x < cb->x;
    }
    return reverse ? ca->y > cb->y : ca->y < cb->y;
  }
  if (ca->y != cb->y) {
    return reverse ? ca->y > cb->y : ca->y < cb->y;
  }
  return reverse ? ca->x > cb->x : ca->x < cb->x;
}

static void kp_ovl_battery_order(const struct kp_rgb_frame *frame, size_t *idx,
                                 size_t count, bool reverse) {
  const struct kp_rgb_coord *coords = frame->coords;

  uint16_t min_x = coords[idx[0]].x, max_x = coords[idx[0]].x;
  uint16_t min_y = coords[idx[0]].y, max_y = coords[idx[0]].y;
  for (size_t i = 1; i < count; i++) {
    const struct kp_rgb_coord *c = &coords[idx[i]];
    min_x = MIN(min_x, c->x);
    max_x = MAX(max_x, c->x);
    min_y = MIN(min_y, c->y);
    max_y = MAX(max_y, c->y);
  }
  const bool x_primary = (uint16_t)(max_x - min_x) >= (uint16_t)(max_y - min_y);

  /* Insertion sort: n is a keyboard's LED count. */
  for (size_t i = 1; i < count; i++) {
    const size_t value = idx[i];
    size_t j = i;
    while (j > 0 && kp_ovl_battery_before(frame, value, idx[j - 1], x_primary,
                                          reverse)) {
      idx[j] = idx[j - 1];
      j--;
    }
    idx[j] = value;
  }
}

/* Overlay renderers run one at a time under the matrix lock, so one shared
 * scratch order (like the compositor's shared layer buffer) is enough. */
static size_t kp_ovl_battery_order_buf[KP_LED_COUNT];
#endif

static bool kp_ovl_battery_render(const struct device *dev,
                                  const struct kp_rgb_frame *frame) {
#if KP_LED_COUNT == 0
  ARG_UNUSED(dev);
  ARG_UNUSED(frame);
#else
  const struct kp_ovl_battery_config *cfg = dev->config;
  const size_t count = frame->target_count;
  if (count == 0) {
    return false;
  }
  uint8_t soc = 0;
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
  soc = zmk_battery_state_of_charge();
#endif
  soc = MIN(soc, 100);
  if (cfg->hue) {
    const int32_t hue = (int32_t)cfg->hue_empty +
        ((int32_t)cfg->hue_full - cfg->hue_empty) * soc / 100;
    const struct led_rgb color = kp_rgb_hsb_to_rgb(
        (struct kp_rgb_hsb){.h = hue, .s = 100, .b = 100});
    for (size_t t = 0; t < count; t++) {
      const size_t i = frame->targets[t];
      frame->pixels[i] = kp_rgb_rgb_mix(frame->pixels[i], color, cfg->common.opacity);
    }
    return false;
  }
#if KP_BATTERY_HAS_BAR
  struct kp_ovl_battery_data *data = dev->data;
  const bool canonical =
      frame->coords == kp_rgb_led_coord(0) &&
      frame->targets == kp_rgb_overlay_targets(dev) &&
      frame->count == KP_LED_COUNT && count == kp_rgb_overlay_target_count(dev);
  size_t *order;
  if (canonical) {
    order = data->canonical_order;
    if (!data->canonical_order_ready) {
      for (size_t i = 0; i < count; i++) {
        order[i] = frame->targets[i];
      }
      kp_ovl_battery_order(frame, order, count, cfg->reverse);
      data->canonical_order_ready = true;
    }
  } else {
    order = kp_ovl_battery_order_buf;
    for (size_t i = 0; i < count; i++) {
      order[i] = frame->targets[i];
    }
    kp_ovl_battery_order(frame, order, count, cfg->reverse);
  }

  const size_t lit = (count / 100u) * soc +
                     ((count % 100u) * soc + 50u) / 100u;

  const struct led_rgb on = kp_hex_to_rgb(cfg->color);
  const struct led_rgb off = kp_hex_to_rgb(cfg->background_color);

  for (size_t i = 0; i < count; i++) {
    frame->pixels[order[i]] = kp_rgb_rgb_mix(
        frame->pixels[order[i]], i < lit ? on : off, cfg->common.opacity);
  }
#endif
#endif /* KP_LED_COUNT > 0 */
  return false;
}

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
static int kp_rgb_battery_overlay_listener(const zmk_event_t *eh) {
  ARG_UNUSED(eh);
  zmk_rgb_matrix_flush();
  return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(kp_rgb_battery_overlay, kp_rgb_battery_overlay_listener);
ZMK_SUBSCRIPTION(kp_rgb_battery_overlay, zmk_battery_state_changed);
#endif

static const struct kp_rgb_effect_callbacks kp_ovl_battery_callbacks = {
    .render = kp_ovl_battery_render,
};

#define KP_OVL_BATTERY_DEFINE(inst)                                            \
  BUILD_ASSERT(DT_NODE_HAS_PROP(DT_DRV_INST(inst), all_leds) ||                \
                   DT_PROP_LEN_OR(DT_DRV_INST(inst), keys, 0) > 0 ||           \
                   DT_PROP_LEN_OR(DT_DRV_INST(inst), leds, 0) > 0,             \
               "keypaw,rgb-overlay-battery targets nothing: declare "          \
               "`all-leds;`, `keys` or `leds`");                               \
  BUILD_ASSERT(DT_INST_PROP(inst, hue_empty) >= 0 &&                           \
                   DT_INST_PROP(inst, hue_empty) <= 359 &&                     \
                   DT_INST_PROP(inst, hue_full) >= 0 &&                        \
                   DT_INST_PROP(inst, hue_full) <= 359,                        \
               "battery hue endpoints must be in 0..359");                     \
  KP_RGB_OVERLAY_TARGET_ARRAYS(inst, kp_ovl_battery_##inst);                   \
  static const struct kp_ovl_battery_config kp_ovl_battery_##inst##_cfg = {    \
      .common = KP_RGB_OVERLAY_COMMON(DT_DRV_INST(inst),                       \
                                      &kp_ovl_battery_callbacks, true),        \
      .color = DT_PROP_OR(DT_DRV_INST(inst), color, 0x00FF00),                 \
      .background_color =                                                      \
          DT_PROP_OR(DT_DRV_INST(inst), background_color, 0x000000),           \
      .reverse = DT_PROP(DT_DRV_INST(inst), reverse),                          \
      .hue = DT_INST_ENUM_HAS_VALUE(inst, mode, hue),                          \
      .hue_empty = DT_INST_PROP(inst, hue_empty),                              \
      .hue_full = DT_INST_PROP(inst, hue_full),                                \
  };                                                                           \
  COND_CODE_1(                                                                 \
      DT_INST_ENUM_HAS_VALUE(inst, mode, bar),                                 \
      (static size_t kp_ovl_battery_##inst##_canonical_order[MAX(              \
          1, DT_PROP(DT_DRV_INST(inst), all_leds)                              \
                 ? KP_LED_COUNT                                                \
                 : MIN(KP_LED_COUNT,                                           \
                       DT_PROP_LEN_OR(DT_DRV_INST(inst), keys, 0) +            \
                           DT_PROP_LEN_OR(DT_DRV_INST(inst), leds, 0)))];),    \
      ())                                                                      \
  static struct kp_ovl_battery_data kp_ovl_battery_##inst##_data = {           \
      KP_RGB_OVERLAY_COMMON_DATA(inst, kp_ovl_battery_##inst),                 \
      .canonical_order =                                                       \
          COND_CODE_1(DT_INST_ENUM_HAS_VALUE(inst, mode, bar),                 \
                      (kp_ovl_battery_##inst##_canonical_order), (NULL)),      \
  };                                                                           \
  KP_RGB_OVERLAY_DEFINE(inst, kp_ovl_battery_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_OVL_BATTERY_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
