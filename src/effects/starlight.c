/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * "starlight" effect: LEDs turn on and off at random at varying brightness,
 * keeping the user colour (with optional hue/sat jitter). Covers QMK's
 * STARLIGHT, STARLIGHT_SMOOTH, STARLIGHT_DUAL_HUE and STARLIGHT_DUAL_SAT via the
 * smooth / dual-hue / dual-sat attributes.
 */

#define DT_DRV_COMPAT keypaw_rgb_matrix_starlight

#include <stdint.h>

#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>

#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct kp_eff_starlight_config {
  struct kp_rgb_effect_common_config common;
  bool smooth;
  bool dual_hue;
  bool dual_sat;
};

struct kp_eff_starlight_data {
  struct kp_rgb_effect_common_data common;
  uint8_t cur[KP_LED_COUNT];  /* current brightness 0..255 */
  uint8_t target[KP_LED_COUNT]; /* target brightness */
  int8_t hue_off[KP_LED_COUNT]; /* -30..30 */
  int8_t sat_off[KP_LED_COUNT]; /* -30..30 */
};

static void kp_eff_starlight_render(const struct device *dev, struct kp_rgb_frame *f) {
  struct kp_eff_starlight_data *data = dev->data;
  const struct kp_eff_starlight_config *cfg = dev->config;
  uint32_t period = kp_rgb_effect_period(dev);
  uint8_t pct = kp_rgb_brightness_pct(f);
  struct kp_rgb_hsb base = data->common.color;

  /* Per-tick ramp rate toward the target (smooth mode), in brightness units. */
  uint32_t rate = MAX(255u * f->elapsed / period, 1u);

  for (size_t i = 0; i < f->count; i++) {
    /* Occasionally toggle this LED on (varying brightness) or off. */
    if (sys_rand32_get() % 100 < 4) {
      if (data->target[i] == 0) {
        data->target[i] = (uint8_t)(40u + (sys_rand32_get() % 60u));
        if (cfg->dual_hue) {
          data->hue_off[i] = (int8_t)((sys_rand32_get() % 61u) - 30);
        }
        if (cfg->dual_sat) {
          data->sat_off[i] = (int8_t)((sys_rand32_get() % 61u) - 30);
        }
      } else {
        data->target[i] = 0;
      }
    }

    if (cfg->smooth) {
      int16_t c = data->cur[i];
      int16_t t = data->target[i];
      if (c < t) {
        c = (int16_t)MIN((int32_t)t, c + (int32_t)rate);
      } else if (c > t) {
        c = (int16_t)MAX((int32_t)t, c - (int32_t)rate);
      }
      data->cur[i] = (uint8_t)c;
    } else {
      data->cur[i] = data->target[i];
    }

    if (data->cur[i] > 0) {
      int16_t h = (int16_t)base.h + (cfg->dual_hue ? data->hue_off[i] : 0);
      h %= (int16_t)KP_RGB_HUE_MAX;
      if (h < 0) {
        h += KP_RGB_HUE_MAX;
      }
      int16_t s = (int16_t)base.s + (cfg->dual_sat ? data->sat_off[i] : 0);
      s = MAX(0, MIN((int16_t)KP_RGB_SAT_MAX, s));
      struct kp_rgb_hsb hsb = {
          .h = (uint16_t)h,
          .s = (uint8_t)s,
          .b = (uint8_t)((uint32_t)base.b * data->cur[i] / 255u),
      };
      f->pixels[i] = kp_rgb_hsb_to_rgb(kp_rgb_hsb_scale(hsb, pct));
    } else {
      f->pixels[i] = (struct led_rgb){0, 0, 0};
    }
  }
}

#define KP_EFF_STARLIGHT_DEFINE(inst)                                           \
  static const struct kp_eff_starlight_config kp_eff_starlight_##inst##_cfg = { \
      .common = {.index = DT_PROP(DT_DRV_INST(inst), index)},                   \
      .smooth = DT_PROP_OR(DT_DRV_INST(inst), smooth, 0),                       \
      .dual_hue = DT_PROP_OR(DT_DRV_INST(inst), dual_hue, 0),                   \
      .dual_sat = DT_PROP_OR(DT_DRV_INST(inst), dual_sat, 0),                  \
  };                                                                           \
  static struct kp_eff_starlight_data kp_eff_starlight_##inst##_data = {        \
      .common =                                                                \
          {                                                                    \
              .color = KP_RGB_HSB_FROM_HEX(                                    \
                  DT_PROP_OR(DT_DRV_INST(inst), color, 0xFFFFFF)),             \
              .duration_ms = DT_PROP_OR(DT_DRV_INST(inst), duration, 0),       \
          },                                                                   \
  };                                                                           \
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), kp_eff_starlight_render, NULL,        \
                       kp_eff_starlight_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_EFF_STARLIGHT_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
