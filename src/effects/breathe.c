/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Built-in "breathe" effect: the whole board fades up and down once per
 * animation period. Port of the equivalent in zmk/app/src/rgb_underglow.c, but
 * driven by the effect's own period (`duration`) instead of a global speed.
 */

#define DT_DRV_COMPAT keypaw_rgb_matrix_breathe

#include <zephyr/sys/util.h>

#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct kp_eff_breathe_config {
  struct kp_rgb_effect_common_config common;
};
struct kp_eff_breathe_data {
  struct kp_rgb_effect_common_data common;
  uint32_t phase_ms;
};

static void kp_eff_breathe_render(const struct device *dev, struct kp_rgb_frame *f) {
  struct kp_eff_breathe_data *data = dev->data;
  uint32_t period = kp_rgb_effect_period(dev);
  uint32_t half = MAX(period / 2u, 1u);
  uint32_t phase = data->phase_ms % period;

  struct kp_rgb_hsb hsb = data->common.color;
  /* Triangle: dimmest at the period edges, full at the half-way point. */
  uint32_t ramp = phase < half ? phase : period - phase;
  hsb.b = (uint8_t)((uint32_t)hsb.b * ramp / half);
  struct led_rgb rgb = kp_rgb_hsb_to_rgb(kp_rgb_hsb_scale(hsb, kp_rgb_brightness_pct(f)));

  for (size_t i = 0; i < f->count; i++) {
    f->pixels[i] = rgb;
  }

  data->phase_ms = (phase + f->elapsed) % period;
}

#define KP_EFF_BREATHE_DEFINE(inst)                                            \
  static const struct kp_eff_breathe_config kp_eff_breathe_##inst##_cfg = {    \
      .common = {.index = DT_PROP(DT_DRV_INST(inst), index)},                  \
  };                                                                           \
  static struct kp_eff_breathe_data kp_eff_breathe_##inst##_data = {           \
      .common =                                                                \
          {                                                                    \
              .color = KP_RGB_HSB_FROM_HEX(                                    \
                  DT_PROP_OR(DT_DRV_INST(inst), color, 0xFFFFFF)),             \
              .duration_ms = DT_PROP_OR(DT_DRV_INST(inst), duration, 0),       \
          },                                                                   \
  };                                                                           \
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), kp_eff_breathe_render, NULL,         \
                       kp_eff_breathe_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_EFF_BREATHE_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
