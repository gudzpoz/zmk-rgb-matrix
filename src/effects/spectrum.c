/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Built-in "spectrum" effect: the whole board cycles through the hue wheel once
 * per animation period. Port of the equivalent in zmk/app/src/rgb_underglow.c.
 *
 * Saturation is forced to full so the sweep is visible whatever preset colour
 * was configured; the preset hue only sets the phase offset.
 */

#define DT_DRV_COMPAT keypaw_rgb_matrix_spectrum

#include <zephyr/sys/util.h>

#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct kp_eff_spectrum_config {
  struct kp_rgb_effect_common_config common;
};
struct kp_eff_spectrum_data {
  struct kp_rgb_effect_common_data common;
  uint32_t phase_ms;
};

/* CYCLE_ALL: every LED shares the same hue cycle. */

static void kp_eff_spectrum_render(const struct device *dev, struct kp_rgb_frame *f) {
  struct kp_eff_spectrum_data *data = dev->data;
  uint32_t period = kp_rgb_effect_period(dev);
  uint32_t phase = data->phase_ms % period;
  struct kp_rgb_hsb hsb = data->common.color;
  uint8_t pct = kp_rgb_brightness_pct(f);

  hsb.h = (uint16_t)((hsb.h + phase * KP_RGB_HUE_MAX / period) % KP_RGB_HUE_MAX);
  hsb.s = KP_RGB_SAT_MAX;
  struct led_rgb rgb = kp_rgb_hsb_to_rgb(kp_rgb_hsb_scale(hsb, pct));
  for (size_t i = 0; i < f->count; i++) {
    f->pixels[i] = rgb;
  }

  data->phase_ms = (phase + f->elapsed) % period;
}

#define KP_EFF_SPECTRUM_DEFINE(inst)                                           \
  static const struct kp_eff_spectrum_config kp_eff_spectrum_##inst##_cfg = {  \
      .common = {.index = KP_RGB_EFFECT_INDEX(inst)},                          \
  };                                                                           \
  static struct kp_eff_spectrum_data kp_eff_spectrum_##inst##_data = {         \
      .common =                                                                \
          {                                                                    \
              .color = KP_RGB_HSB_FROM_HEX(                                    \
                  DT_PROP_OR(DT_DRV_INST(inst), color, 0xFFFFFF)),             \
              .duration_ms = DT_PROP_OR(DT_DRV_INST(inst), duration, 0),       \
          },                                                                   \
  };                                                                           \
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), kp_eff_spectrum_render, NULL,        \
                       kp_eff_spectrum_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_EFF_SPECTRUM_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
