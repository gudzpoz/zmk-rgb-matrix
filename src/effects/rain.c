/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * "rain" effect: randomly lit keys with random colours, covering QMK's
 * PIXEL_RAIN, PIXEL_FLOW, RAINDROPS, JELLYBEAN_RAINDROPS and PIXEL_FRACTAL via
 * the `mode` attribute.
 */

#define DT_DRV_COMPAT keypaw_rgb_matrix_rain

#include <stdint.h>

#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>

#include <zmk/rgb_matrix.h>
#include <zmk/rgb_matrix_math.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/* pixel = random keys, flow = cursor walking the chain, jellybean = random hue
 * and saturation, fractal = a single-hue pulse from the centre, drops = QMK's
 * RAINDROPS (see kp_rain_drops_hue below). */
DEFINE_DT_ENUM(kp_rain_mode_t, mode, pixel, flow, drops, jellybean, fractal);

/* Hue nudges RAINDROPS makes per animation period. Tying the rate to the period
 * keeps `duration` meaningful here as it is for every other effect. */
#define KP_RAIN_DROPS_PER_PERIOD 64u

/* QMK's RAINDROPS walks a key's hue toward the antipode in four steps; the
 * negative step makes it walk down the wheel, as QMK does. */
#define KP_RAIN_DROPS_HUE_STEP (-(KP_RGB_HUE_MAX / 8))
#define KP_RAIN_DROPS_STEPS 3u /* QMK's random8_max(3): 0, 1 or 2 steps */

struct kp_eff_rain_config {
  struct kp_rgb_effect_common_config common;
  kp_rain_mode_t mode;
  uint16_t step_interval_ms;
};

struct kp_eff_rain_data {
  struct kp_rgb_effect_common_data common;
#if KP_LED_COUNT > 0
  uint8_t val[KP_LED_COUNT];  /* current brightness 0..255 */
  uint16_t hue[KP_LED_COUNT]; /* per-LED hue, 0..KP_RGB_HUE_MAX */
  uint8_t sat[KP_LED_COUNT];  /* per-LED saturation while lit */
  uint32_t fade_remainder[KP_LED_COUNT];
#endif
  uint32_t step_remainder_ms;
  uint32_t flow_idx;
  uint32_t phase_ms;
  uint32_t drop_remainder; /* elapsed_ms * 64, modulo duration */
  bool drops_seeded;
};

#if KP_LED_COUNT > 0
/* One QMK RAINDROPS nudge: a fresh hue for a single key, derived from the
 * preset. Saturation is deliberately left to the preset, so an unsaturated
 * colour renders every nudge as the same washed-out white. */
static uint16_t kp_rain_drops_hue(uint16_t base_hue) {
  int32_t hue = (int32_t)base_hue +
                (int32_t)KP_RAIN_DROPS_HUE_STEP *
                    (int32_t)(sys_rand32_get() % KP_RAIN_DROPS_STEPS);
  if (hue < 0) {
    hue += KP_RGB_HUE_MAX;
  }
  return (uint16_t)hue;
}

static void kp_eff_rain_fade(struct kp_eff_rain_data *data, size_t count,
                             uint32_t elapsed_ms, uint32_t period) {
  if (elapsed_ms == 0) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    uint64_t progress = (uint64_t)elapsed_ms * 255u + data->fade_remainder[i];
    uint64_t decay = progress / period;
    if (decay >= data->val[i]) {
      data->val[i] = 0;
      data->fade_remainder[i] = 0;
    } else {
      data->val[i] -= decay;
      data->fade_remainder[i] = progress % period;
    }
  }
}
#endif /* KP_LED_COUNT > 0 */

static bool kp_eff_rain_render(const struct device *dev, const struct kp_rgb_frame *f) {
#if KP_LED_COUNT == 0
  ARG_UNUSED(dev);
  ARG_UNUSED(f);
#else
  struct kp_eff_rain_data *data = dev->data;
  const struct kp_eff_rain_config *cfg = dev->config;
  uint32_t period = kp_rgb_effect_period(dev);
  struct kp_rgb_hsb base = data->common.color;
  uint16_t bl = MAX(f->board_length, 1u);

  if (f->count == 0) {
    return true;
  }
  if (cfg->mode == DT_ENUM_CONST(mode, drops) && !data->drops_seeded) {
    for (size_t i = 0; i < f->count; i++) {
      data->val[i] = 255;
      data->sat[i] = base.s;
      data->hue[i] = kp_rain_drops_hue(base.h);
    }
    data->drops_seeded = true;
  }
  if (cfg->mode == DT_ENUM_CONST(mode, fractal)) {
    data->phase_ms = (uint32_t)(((uint64_t)data->phase_ms + f->elapsed_ms) % period);
  } else if (cfg->mode == DT_ENUM_CONST(mode, drops)) {
    uint64_t progress = data->drop_remainder +
                        (uint64_t)f->elapsed_ms * KP_RAIN_DROPS_PER_PERIOD;
    uint32_t nudges = MIN(progress / period, 8u);
    data->drop_remainder = progress % period;
    for (uint32_t step = 0; step < nudges; step++) {
      size_t idx = sys_rand32_get() % f->count;
      data->hue[idx] = kp_rain_drops_hue(base.h);
    }
  } else {
    uint32_t remaining = f->elapsed_ms;
    uint32_t interval = cfg->step_interval_ms;
    uint32_t until_step = interval - data->step_remainder_ms;
    for (uint32_t step = 0; step < 8 && remaining >= until_step; step++) {
      kp_eff_rain_fade(data, f->count, until_step, period);
      remaining -= until_step;
      data->step_remainder_ms = 0;
      until_step = interval;
      if (cfg->mode == DT_ENUM_CONST(mode, flow)) {
        data->flow_idx = (data->flow_idx + 1) % f->count;
        size_t idx = data->flow_idx;
        data->val[idx] = 255;
        data->fade_remainder[idx] = 0;
        data->hue[idx] = (uint16_t)(sys_rand32_get() % KP_RGB_HUE_MAX);
        data->sat[idx] = base.s;
      } else if (sys_rand32_get() % 100 < 35) {
        size_t idx = sys_rand32_get() % f->count;
        data->val[idx] = 255;
        data->fade_remainder[idx] = 0;
        data->hue[idx] = (uint16_t)(sys_rand32_get() % KP_RGB_HUE_MAX);
        data->sat[idx] = (cfg->mode == DT_ENUM_CONST(mode, jellybean))
                            ? (uint8_t)(sys_rand32_get() % KP_RGB_SAT_MAX)
                            : base.s;
      }
    }
    kp_eff_rain_fade(data, f->count, remaining, period);
    data->step_remainder_ms =
        ((uint64_t)data->step_remainder_ms + remaining) % interval;
  }

  for (size_t t = 0; t < f->target_count; t++) {
    size_t i = f->targets[t];
    uint8_t v = data->val[i];

    if (cfg->mode == DT_ENUM_CONST(mode, fractal)) {
      /* The pulse has two fronts (it mirrors from the centre), so halve the
       * phase to keep each front at a single-front rate. */
      uint32_t phase01 = kp_rgb_arm_phase(data->phase_ms * 65536u / period, 2u);
      uint32_t rad = phase01 * (bl / 2u + 100u) / 65536u;
      int32_t d = (int32_t)f->coords[i].x - bl / 2;
      if (d < 0) {
        d = -d;
      }
      int32_t diff = (int32_t)d - (int32_t)rad;
      if (diff < 0) {
        diff = -diff;
      }
      v = diff < 100 ? (uint8_t)(255u - (uint32_t)diff * 255u / 100u) : 0;
    }

    if (v > 0) {
      struct kp_rgb_hsb hsb;
      if (cfg->mode == DT_ENUM_CONST(mode, fractal)) {
        hsb.h = base.h;
        hsb.s = base.s;
      } else {
        hsb.h = data->hue[i];
        hsb.s = data->sat[i];
      }
      hsb.b = (uint8_t)((uint32_t)base.b * v / 255u);
      f->pixels[i] = kp_rgb_hsb_to_rgb(hsb);
    } else {
      f->pixels[i] = (struct led_rgb){0, 0, 0};
    }
  }
#endif /* KP_LED_COUNT > 0 */
  return true;
}

static void kp_eff_rain_reset(const struct device *dev, int64_t now_ms) {
  ARG_UNUSED(now_ms);
  struct kp_eff_rain_data *data = dev->data;
  struct kp_rgb_effect_common_data common = data->common;
  *data = (struct kp_eff_rain_data){.common = common};
}

static const struct kp_rgb_effect_callbacks kp_eff_rain_callbacks = {
    .render = kp_eff_rain_render,
    .reset = kp_eff_rain_reset,
};

#define KP_EFF_RAIN_DEFINE(inst)                                               \
  BUILD_ASSERT(DT_PROP(DT_DRV_INST(inst), step_interval_ms) > 0 &&             \
                   DT_PROP(DT_DRV_INST(inst), step_interval_ms) <= UINT16_MAX, \
               "rain step-interval-ms must be in 1..65535");                   \
  static const struct kp_eff_rain_config kp_eff_rain_##inst##_cfg = {          \
      KP_RGB_EFFECT_COMMON_CONFIG(DT_DRV_INST(inst),                           \
                                  KP_RGB_EFFECT_INDEX(inst)),                  \
      .mode = CONV_DT_ENUM(inst, mode),                                        \
      .step_interval_ms = DT_PROP(DT_DRV_INST(inst), step_interval_ms),        \
  };                                                                           \
  static struct kp_eff_rain_data kp_eff_rain_##inst##_data = {                 \
      KP_RGB_EFFECT_COMMON_DATA(DT_DRV_INST(inst), 0xFFFFFF),                  \
  };                                                                           \
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), &kp_eff_rain_callbacks,              \
                       kp_eff_rain_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_EFF_RAIN_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
