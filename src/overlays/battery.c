/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Battery-gauge overlay kind: a positional level bar that lights the first
 * `soc` percent of its targets, with a defined empty track.
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

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct kp_ovl_battery_config {
  struct kp_rgb_overlay_common_config common;
  const struct device *condition; /* NULL = always active */
  uint32_t color;                 /* 0xRRGGBB */
  uint32_t background_color;      /* 0xRRGGBB */
  bool reverse;
};
struct kp_ovl_battery_data {
  struct kp_rgb_overlay_common_data common;
};

static bool kp_ovl_battery_active(const struct device *dev) {
  const struct kp_ovl_battery_config *cfg = dev->config;

  if (cfg->condition == NULL) {
    return true;
  }
  const struct kp_rgb_condition_api *cond =
      (const struct kp_rgb_condition_api *)cfg->condition->api;
  return cond->active(cfg->condition);
}

#if KP_LED_COUNT > 0
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
#endif /* KP_LED_COUNT > 0 */

static void kp_ovl_battery_render(const struct device *dev,
                                  struct kp_rgb_frame *frame) {
#if KP_LED_COUNT == 0
  ARG_UNUSED(dev);
  ARG_UNUSED(frame);
#else
  const struct kp_ovl_battery_config *cfg = dev->config;
  const struct kp_ovl_battery_data *data = dev->data;
  const bool all = cfg->common.all_leds;

  const size_t count = all ? frame->count : data->common.led_count;
  if (count == 0) {
    return;
  }
  size_t *order = kp_ovl_battery_order_buf;
  for (size_t i = 0; i < count; i++) {
    order[i] = all ? i : data->common.leds[i];
  }
  kp_ovl_battery_order(frame, order, count, cfg->reverse);

  uint8_t soc = 0;
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
  soc = zmk_battery_state_of_charge();
#endif
  soc = MIN(soc, 100);
  const size_t lit = (count * soc + 50u) / 100u; /* rounded */

  const struct led_rgb on =
      kp_rgb_rgb_scale(kp_hex_to_rgb(cfg->color), kp_rgb_brightness_pct(frame));
  const struct led_rgb off = kp_rgb_rgb_scale(
      kp_hex_to_rgb(cfg->background_color), kp_rgb_brightness_pct(frame));

  /* At opacity 100 this is a replace, which the all-leds cover skip relies on. */
  for (size_t i = 0; i < count; i++) {
    frame->pixels[order[i]] = kp_rgb_rgb_mix(
        frame->pixels[order[i]], i < lit ? on : off, cfg->common.opacity);
  }
#endif /* KP_LED_COUNT > 0 */
}

#define KP_OVL_BATTERY_DEFINE(inst)                                            \
  BUILD_ASSERT(DT_NODE_HAS_PROP(DT_DRV_INST(inst), all_leds) ||                \
                   DT_PROP_LEN_OR(DT_DRV_INST(inst), keys, 0) > 0 ||           \
                   DT_PROP_LEN_OR(DT_DRV_INST(inst), leds, 0) > 0,             \
               "keypaw,rgb-overlay-battery targets nothing: declare "          \
               "`all-leds;`, `keys` or `leds`");                               \
  KP_RGB_OVERLAY_TARGET_ARRAYS(inst, kp_ovl_battery_##inst);                   \
  static const struct kp_ovl_battery_config kp_ovl_battery_##inst##_cfg = {    \
      .common =                                                                \
          KP_RGB_OVERLAY_COMMON(DT_DRV_INST(inst), kp_ovl_battery_##inst),     \
      .condition = KP_RGB_CONDITION_PTR(DT_DRV_INST(inst)),                    \
      .color = DT_PROP_OR(DT_DRV_INST(inst), color, 0x00FF00),                 \
      .background_color =                                                      \
          DT_PROP_OR(DT_DRV_INST(inst), background_color, 0x000000),           \
      .reverse = DT_PROP(DT_DRV_INST(inst), reverse),                          \
  };                                                                           \
  static struct kp_ovl_battery_data kp_ovl_battery_##inst##_data;              \
  KP_RGB_OVERLAY_DEFINE(inst, kp_ovl_battery_active, kp_ovl_battery_render,    \
                        NULL, kp_ovl_battery_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_OVL_BATTERY_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
