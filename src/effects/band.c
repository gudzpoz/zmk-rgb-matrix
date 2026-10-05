/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * "band" effect: a single-hue board with a moving band that fades either
 * saturation or brightness. Covers QMK's BAND_SAT / BAND_VAL and their
 * pinwheel / spiral variants, selected by the `channel` and `shape` attributes.
 */

#define DT_DRV_COMPAT keypaw_rgb_matrix_band

#include <stdint.h>

#include <zephyr/sys/util.h>

#include <zmk/rgb_matrix.h>
#include <zmk/rgb_matrix_math.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/* channel: sat = BAND_SAT family, val = BAND_VAL family. */
/* shape: linear = scroll, pinwheel = rotate, spiral = wind outward. */
DEFINE_DT_ENUM(kp_band_channel_t, channel, sat, val);
DEFINE_DT_ENUM(kp_band_shape_t, shape, linear, pinwheel, spiral);

struct kp_eff_band_config {
  struct kp_rgb_effect_common_config common;
  kp_band_channel_t channel;
  kp_band_shape_t shape;
};

struct kp_eff_band_data {
  struct kp_rgb_effect_common_data common;
  uint32_t phase_ms;
};



/* Spatial coordinate of an LED in [0, 65536) along the chosen shape. */
static uint32_t kp_band_spatial(kp_band_shape_t shape, uint16_t x, uint16_t y, uint16_t bl,
                                uint16_t bh, uint32_t max_r) {
  uint16_t cx = bl / 2;
  uint16_t cy = bh / 2;
  if (shape == DT_ENUM_CONST(shape, linear)) {
    return (uint32_t)x * 65536u / MAX(bl, 1u);
  }
  int32_t dx = (int32_t)x - cx;
  int32_t dy = (int32_t)y - cy;
  if (shape == DT_ENUM_CONST(shape, pinwheel)) { /* pinwheel: angle around the centre */
    return (uint32_t)kp_rgb_atan2_8(dy, dx) * 256u;
  }
  /* spiral: angle plus a radial term */
  uint32_t ang = (uint32_t)kp_rgb_atan2_8(dy, dx) * 256u;
  uint32_t dist = kp_rgb_isqrt((uint32_t)(dx * dx + dy * dy));
  return ang + dist * 65536u / MAX(max_r, 1u);
}

static bool kp_eff_band_render(const struct device *dev, const struct kp_rgb_frame *f) {
  struct kp_eff_band_data *data = dev->data;
  const struct kp_eff_band_config *cfg = dev->config;
  uint32_t period = kp_rgb_effect_period(dev);
  uint32_t phase = (uint32_t)(((uint64_t)data->phase_ms + f->elapsed_ms) % period);
  data->phase_ms = phase;
  uint32_t phase01 = phase * 65536u / period;
  /* The spiral wraps twice (angle + radius), so halve its phase to keep each
   * front at a single-wrap rate. */
  uint32_t phase_eff = kp_rgb_arm_phase(
      phase01, cfg->shape == DT_ENUM_CONST(shape, spiral) ? 2u : 1u);
  uint16_t bl = MAX(f->board_length, 1u);
  uint16_t bh = MAX(f->board_height, 1u);
  uint16_t cx = bl / 2;
  uint16_t cy = bh / 2;
  uint32_t max_r = MAX(kp_rgb_isqrt((uint32_t)cx * cx + (uint32_t)cy * cy), 1u);
  struct kp_rgb_hsb base = data->common.color;

  for (size_t t = 0; t < f->target_count; t++) {
    size_t i = f->targets[t];
    uint32_t s01 = kp_band_spatial(cfg->shape, f->coords[i].x, f->coords[i].y, bl, bh,
                                   max_r);
    /* Distance of this LED from the moving band front, wrapped to [-0.5, 0.5]. */
    int32_t d = (int32_t)s01 - (int32_t)phase_eff;
    if (d > 32768) {
      d -= 65536;
    } else if (d < -32768) {
      d += 65536;
    }
    uint32_t ad = d < 0 ? (uint32_t)(-d) : (uint32_t)d;
    uint8_t amp = ad >= 32768u ? 0 : (uint8_t)(255u - 2u * ad / 256u);

    struct kp_rgb_hsb hsb = base;
    if (cfg->channel == DT_ENUM_CONST(channel, sat)) {
      hsb.s = (uint8_t)((uint32_t)base.s * amp / 255u);
      hsb.b = base.b;
    } else {
      hsb.b = (uint8_t)((uint32_t)base.b * amp / 255u);
      hsb.s = base.s;
    }
    f->pixels[i] = kp_rgb_hsb_to_rgb(hsb);
  }

  return true;
}

static void kp_eff_band_reset(const struct device *dev, int64_t now_ms) {
  ARG_UNUSED(now_ms);
  struct kp_eff_band_data *data = dev->data;
  data->phase_ms = 0;
}

static const struct kp_rgb_effect_callbacks kp_eff_band_callbacks = {
    .render = kp_eff_band_render,
    .reset = kp_eff_band_reset,
};

#define KP_EFF_BAND_DEFINE(inst)                                               \
  static const struct kp_eff_band_config kp_eff_band_##inst##_cfg = {          \
      KP_RGB_EFFECT_COMMON_CONFIG(DT_DRV_INST(inst),                           \
                                  KP_RGB_EFFECT_INDEX(inst)),                  \
      .channel = CONV_DT_ENUM(inst, channel),                                  \
      .shape = CONV_DT_ENUM(inst, shape),                                      \
  };                                                                           \
  static struct kp_eff_band_data kp_eff_band_##inst##_data = {                 \
      KP_RGB_EFFECT_COMMON_DATA(DT_DRV_INST(inst), 0xFFFFFF),                  \
  };                                                                           \
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), &kp_eff_band_callbacks,              \
                       kp_eff_band_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_EFF_BAND_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
