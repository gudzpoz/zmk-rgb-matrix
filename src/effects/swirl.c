/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Built-in "swirl" effect: a hue sweep travelling along the board. Port of the
 * equivalent in zmk/app/src/rgb_underglow.c, but re-based on the LED's real x
 * position instead of its index in the chain, so the sweep follows the physical
 * board and completes one pass per animation period.
 */

#define DT_DRV_COMPAT keypaw_rgb_matrix_swirl

#include <zephyr/sys/util.h>

#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct kp_eff_swirl_config {
  struct kp_rgb_effect_common_config common;
};
struct kp_eff_swirl_data {
  struct kp_rgb_effect_common_data common;
  uint32_t phase_ms;
};

static void kp_eff_swirl_render(const struct device *dev, struct kp_rgb_frame *f) {
  struct kp_eff_swirl_data *data = dev->data;
  uint32_t period = kp_rgb_effect_period(dev);
  uint32_t phase = data->phase_ms % period;
  uint8_t pct = kp_rgb_brightness_pct(f);
  struct kp_rgb_hsb base = data->common.color;
  /* f->coords is normalised to this half, and board_length is its longest edge,
   * so the sweep spans the actual board rather than a hardcoded width. */
  uint32_t sweep = phase * KP_RGB_HUE_MAX / period;

  for (size_t i = 0; i < f->count; i++) {
    struct kp_rgb_hsb hsb = base;
    hsb.h = ((uint32_t)f->coords[i].x * KP_RGB_HUE_MAX / f->board_length + sweep) %
            KP_RGB_HUE_MAX;
    f->pixels[i] = kp_rgb_hsb_to_rgb(kp_rgb_hsb_scale(hsb, pct));
  }

  data->phase_ms = (phase + f->elapsed) % period;
}

#define KP_EFF_SWIRL_DEFINE(inst)                                              \
  static const struct kp_eff_swirl_config kp_eff_swirl_##inst##_cfg = {        \
      .common = {.index = DT_PROP(DT_DRV_INST(inst), index)},                  \
  };                                                                           \
  static struct kp_eff_swirl_data kp_eff_swirl_##inst##_data = {               \
      .common =                                                                \
          {                                                                    \
              .color = KP_RGB_HSB_FROM_HEX(                                    \
                  DT_PROP_OR(DT_DRV_INST(inst), color, 0xFFFFFF)),             \
              .duration_ms = DT_PROP_OR(DT_DRV_INST(inst), duration, 0),       \
          },                                                                   \
  };                                                                           \
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), kp_eff_swirl_render, NULL,           \
                       kp_eff_swirl_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_EFF_SWIRL_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
