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
#include <zmk/rgb_matrix_math.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/* brightness = BREATHING (whole-board fade), river = RIVERFLOW (x-offset
 * pulse), hue = HUE_BREATHING, pendulum = HUE_PENDULUM, and wave = HUE_WAVE. */
DEFINE_DT_ENUM(mode, brightness, river, hue, pendulum, wave);

struct kp_eff_breathe_config {
  struct kp_rgb_effect_common_config common;
  mode_t mode;
  /* Hue swing, in degrees, for the hue-oscillating modes. */
  uint16_t hue_amplitude;
};
struct kp_eff_breathe_data {
  struct kp_rgb_effect_common_data common;
  uint32_t phase_ms;
};

/* Sine-eased oscillation in [0, 255] for a phase in [0, period): 0 at the
 * start, 255 halfway through, back to 0. The +192 offset puts the sine's
 * minimum at phase 0, so the envelope matches the triangle ramp this replaced,
 * but the fade eases into and out of the peaks instead of reversing sharply. */
static uint32_t kp_breathe_wave(uint32_t phase, uint32_t period) {
  period = MAX(period, 1u);
  return kp_rgb_sin8((uint8_t)(phase * 256u / period + 192u));
}

static uint16_t kp_breathe_hue_offset(uint32_t phase, uint32_t period,
                                      uint16_t x, uint16_t span, mode_t mode,
                                      uint16_t hue_amplitude) {
  uint32_t position;
  if (mode == DT_ENUM_CONST(mode, hue)) {
    return (uint16_t)(kp_breathe_wave(phase, period) * hue_amplitude / 255u);
  }

  span = MAX(span, 1u);
  if (mode == DT_ENUM_CONST(mode, pendulum)) {
    uint32_t travel = kp_breathe_wave(phase, period); /* 0..255, eased */
    uint32_t target = travel * span / 255u;
    position = x > target ? x - target : target - x;
    position = MIN(position, span);
  } else {
    position = (x + (uint32_t)phase * span / period) % span;
  }
  return (uint16_t)(position * hue_amplitude / span);
}
static void kp_eff_breathe_render(const struct device *dev, struct kp_rgb_frame *f) {
  struct kp_eff_breathe_data *data = dev->data;
  const struct kp_eff_breathe_config *cfg = dev->config;
  uint32_t period = kp_rgb_effect_period(dev);
  uint32_t phase = data->phase_ms % period;
  uint8_t pct = kp_rgb_brightness_pct(f);
  struct kp_rgb_hsb base = data->common.color;

  if (cfg->mode == DT_ENUM_CONST(mode, hue) ||
      cfg->mode == DT_ENUM_CONST(mode, pendulum) ||
      cfg->mode == DT_ENUM_CONST(mode, wave)) {
    uint16_t span = MAX(f->board_length, 1u);
    for (size_t i = 0; i < f->count; i++) {
      uint16_t offset = kp_breathe_hue_offset(phase, period, f->coords[i].x,
                                              span, cfg->mode,
                                              cfg->hue_amplitude);
      struct kp_rgb_hsb hsb = base;
      hsb.h = (uint16_t)((base.h + offset) % KP_RGB_HUE_MAX);
      f->pixels[i] = kp_rgb_hsb_to_rgb(kp_rgb_hsb_scale(hsb, pct));
    }
    data->phase_ms = (phase + f->elapsed) % period;
    return;
  }

  if (cfg->mode == DT_ENUM_CONST(mode, river)) {
    /* RIVERFLOW: brightness wave offset by x position, so the pulse travels. */
    uint16_t span = MAX(f->board_length, 1u);
    for (size_t i = 0; i < f->count; i++) {
      uint32_t local = (phase + (uint32_t)f->coords[i].x * period / span) % period;
      uint8_t b = (uint8_t)((uint32_t)base.b * kp_breathe_wave(local, period) / 255u);
      struct kp_rgb_hsb hsb = base;
      hsb.b = b;
      f->pixels[i] = kp_rgb_hsb_to_rgb(kp_rgb_hsb_scale(hsb, pct));
    }
    data->phase_ms = (phase + f->elapsed) % period;
    return;
  }

  /* BREATHING: whole board fades up and down. */
  uint8_t wave = (uint8_t)kp_breathe_wave(phase, period);
  struct kp_rgb_hsb hsb = base;
  hsb.b = (uint8_t)((uint32_t)base.b * wave / 255u);
  struct led_rgb rgb = kp_rgb_hsb_to_rgb(kp_rgb_hsb_scale(hsb, pct));
  for (size_t i = 0; i < f->count; i++) {
    f->pixels[i] = rgb;
  }

  data->phase_ms = (phase + f->elapsed) % period;
}

#define KP_EFF_BREATHE_DEFINE(inst)                                            \
  BUILD_ASSERT(DT_PROP_OR(DT_DRV_INST(inst), hue_amplitude, 45) >= 0 &&        \
                   DT_PROP_OR(DT_DRV_INST(inst), hue_amplitude, 45) <=         \
                       UINT16_MAX,                                             \
               "breathe hue-amplitude must fit a nonnegative uint16_t");       \
  static const struct kp_eff_breathe_config kp_eff_breathe_##inst##_cfg = {    \
      .common = {.index = DT_PROP(DT_DRV_INST(inst), index)},                  \
      .mode = CONV_DT_ENUM(inst, mode),                                        \
      .hue_amplitude =                                                         \
          (uint16_t)CLAMP(DT_PROP_OR(DT_DRV_INST(inst), hue_amplitude, 45), 0, \
                          KP_RGB_HUE_MAX),                                     \
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
