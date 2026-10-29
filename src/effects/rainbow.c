/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * "rainbow" effect: a generalized gradient animator covering QMK cycle_*,
 * pinwheel, beacon, chevron and flower families. A single compatible selects a
 * spatial basis (x, y, radial, pinwheel, spiral, chevron), a direction (out,
 * in, dual, bloom) and a palette (rainbow or solid); instantiate several nodes
 * with different attributes to get distinct cycle entries.
 */

#define DT_DRV_COMPAT keypaw_rgb_matrix_rainbow

#include <stdint.h>

#include <zephyr/sys/util.h>

#include <zmk/rgb_matrix.h>
#include "effect_math.h"

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/* basis: x, y, radial, pinwheel, spiral, chevron, flag. */
/* direction: out, in, dual, bloom. */
/* palette: rainbow, solid. */
DEFINE_DT_ENUM(kp_rainbow_basis_t, basis, x, y, radial, pinwheel, spiral, chevron, flag);
DEFINE_DT_ENUM(kp_rainbow_direction_t, direction, out, in, dual, bloom);
DEFINE_DT_ENUM(kp_rainbow_palette_t, palette, rainbow, solid);

struct kp_eff_rainbow_config {
  struct kp_rgb_effect_common_config common;
  kp_rainbow_basis_t basis;
  kp_rainbow_direction_t direction;
  kp_rainbow_palette_t palette;
};

struct kp_eff_rainbow_data {
  struct kp_rgb_effect_common_data common;
};



static bool kp_eff_rainbow_render(const struct device *dev, const struct kp_rgb_frame *f) {
  struct kp_eff_rainbow_data *data = dev->data;
  const struct kp_eff_rainbow_config *cfg = dev->config;
  uint32_t period = kp_rgb_effect_period(dev);
  uint32_t phase = f->clock_ms % period;
  uint32_t phase01 = phase * 65536u / period;
  uint16_t bl = f->board_length;
  uint16_t bh = f->board_height;
  uint16_t cx = bl / 2;
  uint16_t cy = bh / 2;
  bool dual = cfg->direction == DT_ENUM_CONST(direction, dual);
  bool rainbow = cfg->palette == DT_ENUM_CONST(palette, rainbow);
  struct kp_rgb_hsb base = data->common.color;

  /* Farthest distance from the (single or nearer dual) center to a corner. */
  uint32_t max_r = kp_rgb_isqrt((uint32_t)cx * cx + (uint32_t)cy * cy);
  if (dual) {
    max_r = kp_rgb_isqrt((uint32_t)(3 * bl / 4) * (3 * bl / 4) + (uint32_t)cy * cy);
  }
  max_r = MAX(max_r, 1u);

  /* The spiral and the mirrored dual/bloom centres wrap twice, so their phase is
   * divided by the arm count to keep pace with a single-wrap effect. */
  uint32_t arms = 1;
  if (cfg->basis == DT_ENUM_CONST(basis, spiral) ||
      (dual && (cfg->basis == DT_ENUM_CONST(basis, radial) ||
                cfg->basis == DT_ENUM_CONST(basis, pinwheel))) ||
      (cfg->direction == DT_ENUM_CONST(direction, bloom) &&
       cfg->basis == DT_ENUM_CONST(basis, x))) {
    arms = 2;
  }
  uint32_t phase01_eff = kp_rgb_arm_phase(phase01, arms);

  for (size_t t = 0; t < f->target_count; t++) {
    size_t i = f->targets[t];
    uint16_t x = f->coords[i].x;
    uint16_t y = f->coords[i].y;
    uint32_t spatial;

    if (cfg->basis == DT_ENUM_CONST(basis, flag)) {
      /* FLAG: a horizontal sweep (so it travels with x) plus a per-row skew of
       * up to an eighth of the hue wheel, so the bands undulate instead of
       * staying parallel. */
      spatial = kp_rgb_spatial_linear(x, bl) + kp_rgb_spatial_linear(y, 8u * bh);
    } else if (cfg->basis == DT_ENUM_CONST(basis, x)) {
      spatial = kp_rgb_spatial_linear(x, bl);
    } else if (cfg->basis == DT_ENUM_CONST(basis, y)) {
      spatial = kp_rgb_spatial_linear(y, bh);
    } else {
      int32_t dx, dy;
      if (dual && (cfg->basis == DT_ENUM_CONST(basis, radial) ||
                   cfg->basis == DT_ENUM_CONST(basis, pinwheel))) {
        int32_t d1x = (int32_t)x - (int32_t)(bl / 4);
        int32_t d1y = (int32_t)y - (int32_t)cy;
        int32_t d2x = (int32_t)x - (int32_t)(3 * bl / 4);
        int32_t d2y = (int32_t)y - (int32_t)cy;
        uint32_t r1 = kp_rgb_coord_distance(kp_rgb_coord_distance_squared(d1x, d1y));
        uint32_t r2 = kp_rgb_coord_distance(kp_rgb_coord_distance_squared(d2x, d2y));
        if (r1 <= r2) {
          dx = d1x;
          dy = d1y;
        } else {
          dx = d2x;
          dy = d2y;
        }
      } else {
        dx = (int32_t)x - cx;
        dy = (int32_t)y - cy;
      }

      if (cfg->basis == DT_ENUM_CONST(basis, radial)) { /* radial */
        spatial = kp_rgb_spatial_radius(dx, dy, max_r);
      } else if (cfg->basis == DT_ENUM_CONST(basis, pinwheel)) { /* pinwheel */
        spatial = kp_rgb_spatial_angle(dy, dx);
      } else if (cfg->basis == DT_ENUM_CONST(basis, spiral)) { /* spiral */
        spatial = kp_rgb_spatial_angle(dy, dx) + kp_rgb_spatial_radius(dx, dy, max_r);
      } else { /* chevron */
        uint32_t ax = (x > cx) ? (uint32_t)(x - cx) : (uint32_t)(cx - x);
        spatial = kp_rgb_spatial_linear(ax + (uint32_t)y, MAX((uint32_t)cx + bh, 1u));
      }
    }

    if (cfg->direction == DT_ENUM_CONST(direction, in)) { /* in: reverse the sweep */
      spatial = 65536u - spatial;
    }
    if (cfg->direction == DT_ENUM_CONST(direction, bloom) &&
        cfg->basis == DT_ENUM_CONST(basis, x)) { /* bloom: mirror each half */
      uint32_t local = (x < cx) ? (uint32_t)x : (uint32_t)(bl - x);
      spatial = kp_rgb_spatial_linear(local, MAX(cx, 1u));
    }

    uint32_t pos = (spatial + phase01_eff) % 65536u;

    struct kp_rgb_hsb hsb;
    if (rainbow) {
      hsb.h = (uint16_t)((base.h + pos * KP_RGB_HUE_MAX / 65536u) % KP_RGB_HUE_MAX);
      hsb.s = KP_RGB_SAT_MAX;
      hsb.b = base.b;
    } else {
      hsb.h = base.h;
      hsb.s = base.s;
      /* Solid palette: a travelling brightness band keeps the sweep visible. */
      hsb.b = (uint8_t)((uint32_t)base.b * (32768u + pos / 2) / 65536u);
    }
    f->pixels[i] = kp_rgb_hsb_to_rgb(hsb);
  }

  return true;
}

static const struct kp_rgb_effect_callbacks kp_eff_rainbow_callbacks = {
    .render = kp_eff_rainbow_render,
};

#define KP_EFF_RAINBOW_DEFAULTS(inst) KP_RGB_EFFECT_DEFAULTS(inst, 0xFFFFFF, 0)
#define KP_EFF_RAINBOW_DEFINE(inst)                                            \
  static const struct kp_eff_rainbow_config kp_eff_rainbow_##inst##_cfg = {    \
      KP_RGB_EFFECT_COMMON_CONFIG(inst, KP_EFF_RAINBOW_DEFAULTS(inst)),        \
      .basis = CONV_DT_ENUM(inst, basis),                                      \
      .direction = CONV_DT_ENUM(inst, direction),                              \
      .palette = CONV_DT_ENUM(inst, palette),                                  \
  };                                                                           \
  static struct kp_eff_rainbow_data kp_eff_rainbow_##inst##_data = {           \
      KP_RGB_EFFECT_COMMON_DATA(KP_EFF_RAINBOW_DEFAULTS(inst))};               \
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), &kp_eff_rainbow_callbacks,           \
                       kp_eff_rainbow_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_EFF_RAINBOW_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
