/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Built-in "solid" effect: one flat colour across every LED. Port of the
 * equivalent in zmk/app/src/rgb_underglow.c.
 */

#define DT_DRV_COMPAT keypaw_rgb_matrix_solid

#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct kp_eff_solid_config {
  struct kp_rgb_effect_common_config common;
};
struct kp_eff_solid_data {
  struct kp_rgb_effect_common_data common;
};

static void kp_eff_solid_render(const struct device *dev, struct kp_rgb_frame *f) {
  const struct kp_eff_solid_data *data = dev->data;
  struct led_rgb rgb =
      kp_rgb_rgb_scale(kp_rgb_hsb_to_rgb(data->common.color), kp_rgb_brightness_pct(f));

  for (size_t i = 0; i < f->count; i++) {
    f->pixels[i] = rgb;
  }
}

#define KP_EFF_SOLID_DEFINE(inst)                                              \
  static const struct kp_eff_solid_config kp_eff_solid_##inst##_cfg = {        \
      .common = {.index = DT_PROP(DT_DRV_INST(inst), index)},                  \
  };                                                                           \
  static struct kp_eff_solid_data kp_eff_solid_##inst##_data = {               \
      .common =                                                                \
          {                                                                    \
              .color = KP_RGB_HSB_FROM_HEX(                                    \
                  DT_PROP_OR(DT_DRV_INST(inst), color, 0)),                    \
              .duration_ms = DT_PROP_OR(DT_DRV_INST(inst), duration, 0),       \
          },                                                                   \
  };                                                                           \
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), kp_eff_solid_render, NULL,           \
                       kp_eff_solid_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_EFF_SOLID_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
