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
  uint16_t step_interval_ms;
  bool smooth;
  bool dual_hue;
  bool dual_sat;
};

struct kp_eff_starlight_data {
  struct kp_rgb_effect_common_data common;
  uint32_t step_remainder_ms;
#if KP_LED_COUNT > 0
  uint8_t cur[KP_LED_COUNT];  /* current brightness 0..255 */
  uint8_t target[KP_LED_COUNT]; /* target brightness */
  uint32_t fade_remainder[KP_LED_COUNT];
  int8_t hue_off[KP_LED_COUNT]; /* -30..30 */
  int8_t sat_off[KP_LED_COUNT]; /* -30..30 */
#endif
};

#if KP_LED_COUNT > 0
static void kp_eff_starlight_advance(struct kp_eff_starlight_data *data,
                                     size_t count, uint32_t elapsed_ms,
                                     uint32_t period) {
  if (elapsed_ms == 0) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    uint8_t current = data->cur[i];
    uint8_t target = data->target[i];
    uint32_t distance = current < target ? target - current : current - target;
    uint64_t numerator = (uint64_t)elapsed_ms * 255u + data->fade_remainder[i];
    uint64_t advance = numerator / period;
    if (advance >= distance) {
      data->cur[i] = target;
      data->fade_remainder[i] = 0;
    } else {
      data->cur[i] = current < target ? current + advance : current - advance;
      data->fade_remainder[i] = numerator % period;
    }
  }
}
#endif /* KP_LED_COUNT > 0 */

static bool kp_eff_starlight_render(const struct device *dev, const struct kp_rgb_frame *f) {
#if KP_LED_COUNT == 0
  ARG_UNUSED(dev);
  ARG_UNUSED(f);
#else
  struct kp_eff_starlight_data *data = dev->data;
  const struct kp_eff_starlight_config *cfg = dev->config;
  uint32_t period = kp_rgb_effect_period(dev);
  struct kp_rgb_hsb base = data->common.color;

  uint64_t elapsed = (uint64_t)data->step_remainder_ms + f->elapsed_ms;
  uint32_t steps = MIN(elapsed / cfg->step_interval_ms, 8u);
  uint32_t remaining_ms = f->elapsed_ms;
  uint32_t until_trial_ms = cfg->step_interval_ms - data->step_remainder_ms;
  data->step_remainder_ms = elapsed % cfg->step_interval_ms;

  for (uint32_t step = 0; step < steps; step++) {
    if (cfg->smooth) {
      kp_eff_starlight_advance(data, f->count, until_trial_ms, period);
    }
    remaining_ms -= until_trial_ms;
    until_trial_ms = cfg->step_interval_ms;
    for (size_t i = 0; i < f->count; i++) {
      if (sys_rand32_get() % 100 < 4) {
        data->fade_remainder[i] = 0;
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

      if (!cfg->smooth) {
        data->cur[i] = data->target[i];
      }
    }
  }
  /* Excess trials are discarded, but their elapsed fade time is not. */
  if (cfg->smooth) {
    kp_eff_starlight_advance(data, f->count, remaining_ms, period);
  }

  for (size_t t = 0; t < f->target_count; t++) {
    size_t i = f->targets[t];
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
      f->pixels[i] = kp_rgb_hsb_to_rgb(hsb);
    } else {
      f->pixels[i] = (struct led_rgb){0, 0, 0};
    }
  }
#endif /* KP_LED_COUNT > 0 */
  return true;
}

static void kp_eff_starlight_reset(const struct device *dev, int64_t now_ms) {
  ARG_UNUSED(now_ms);
  struct kp_eff_starlight_data *data = dev->data;
  struct kp_rgb_effect_common_data common = data->common;
  *data = (struct kp_eff_starlight_data){.common = common};
}

static const struct kp_rgb_effect_callbacks kp_eff_starlight_callbacks = {
    .render = kp_eff_starlight_render,
    .reset = kp_eff_starlight_reset,
};

#define KP_EFF_STARLIGHT_DEFINE(inst)                                          \
  BUILD_ASSERT(DT_PROP(DT_DRV_INST(inst), step_interval_ms) > 0 &&             \
                   DT_PROP(DT_DRV_INST(inst), step_interval_ms) <= UINT16_MAX, \
               "starlight step-interval-ms must be in 1..65535");              \
  static const struct kp_eff_starlight_config kp_eff_starlight_##inst##_cfg =  \
      {                                                                        \
          KP_RGB_EFFECT_COMMON_CONFIG(inst),                                   \
          .step_interval_ms = DT_PROP(DT_DRV_INST(inst), step_interval_ms),    \
          .smooth = DT_PROP_OR(DT_DRV_INST(inst), smooth, 0),                  \
          .dual_hue = DT_PROP_OR(DT_DRV_INST(inst), dual_hue, 0),              \
          .dual_sat = DT_PROP_OR(DT_DRV_INST(inst), dual_sat, 0),              \
  };                                                                           \
  static struct kp_eff_starlight_data kp_eff_starlight_##inst##_data = {       \
      KP_RGB_EFFECT_COMMON_DATA(inst, 0xFFFFFF, 0)};                           \
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), &kp_eff_starlight_callbacks,         \
                       kp_eff_starlight_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_EFF_STARLIGHT_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
