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
DEFINE_DT_ENUM(mode, pixel, flow, drops, jellybean, fractal);

/* How many hue nudges RAINDROPS makes per animation period. QMK derives its rate
 * from the global speed setting (and triggers on roughly every frame at the
 * default speed), which this module has no equivalent of; tying it to the effect
 * period keeps `duration` meaningful like every other effect here, and a nudge
 * per ~60 ms reads as a continuous drift rather than a static mosaic. */
#define KP_RAIN_DROPS_PER_PERIOD 64u

/* QMK's RAINDROPS moves a key's hue along "the shortest path between hues": the
 * distance to the antipode (half the wheel) split into four steps, applied zero,
 * one or two times. In QMK's 0..255 hue space that is (int8_t)128 / 4 = -32, and
 * the negative sign makes the hue walk down the wheel -- keep the direction so
 * the drift looks the same. In degrees: -(360 / 2) / 4 = -45. */
#define KP_RAIN_DROPS_HUE_STEP (-(KP_RGB_HUE_MAX / 8))
#define KP_RAIN_DROPS_STEPS 3u /* QMK's random8_max(3): 0, 1 or 2 steps */

struct kp_eff_rain_config {
  struct kp_rgb_effect_common_config common;
  mode_t mode;
};

struct kp_eff_rain_data {
  struct kp_rgb_effect_common_data common;
  uint8_t val[KP_LED_COUNT];  /* current brightness 0..255 */
  uint16_t hue[KP_LED_COUNT]; /* per-LED hue, 0..KP_RGB_HUE_MAX */
  uint8_t sat[KP_LED_COUNT];  /* per-LED saturation while lit */
  uint32_t flow_idx;
  uint32_t phase_ms; /* fractal / drops phase */
  bool drops_seeded;
};

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

static void kp_eff_rain_render(const struct device *dev, struct kp_rgb_frame *f) {
  struct kp_eff_rain_data *data = dev->data;
  const struct kp_eff_rain_config *cfg = dev->config;
  uint32_t period = kp_rgb_effect_period(dev);
  uint32_t decay = MAX(255u * f->elapsed / period, 1u);
  uint8_t pct = kp_rgb_brightness_pct(f);
  struct kp_rgb_hsb base = data->common.color;
  uint16_t bl = MAX(f->board_length, 1u);

  /* Decay every lit LED. RAINDROPS is the exception: it never turns anything
   * off, it only re-hues one key at a time, so every LED stays lit. */
  if (cfg->mode != DT_ENUM_CONST(mode, drops)) {
    for (size_t i = 0; i < f->count; i++) {
      data->val[i] = data->val[i] > decay ? (uint8_t)(data->val[i] - decay) : 0;
    }
  }

  switch (cfg->mode) {
  case DT_ENUM_CONST(mode, flow): { /* a cursor walks the LED chain, sparking each in turn */
    data->flow_idx = (data->flow_idx + 1) % f->count;
    size_t idx = data->flow_idx;
    data->val[idx] = 255;
    data->hue[idx] = (uint16_t)(sys_rand32_get() % KP_RGB_HUE_MAX);
    data->sat[idx] = base.s;
    break;
  }
  case DT_ENUM_CONST(mode, drops): { /* RAINDROPS: nudge one key's hue, board stays lit */
    if (!data->drops_seeded) {
      /* QMK colours every LED on the effect's init frame. */
      for (size_t i = 0; i < f->count; i++) {
        data->val[i] = 255;
        data->sat[i] = base.s;
        data->hue[i] = kp_rain_drops_hue(base.h);
      }
      data->drops_seeded = true;
      break;
    }
    data->phase_ms += f->elapsed;
    uint32_t step_ms = MAX(period / KP_RAIN_DROPS_PER_PERIOD, 1u);
    if (data->phase_ms >= step_ms) {
      data->phase_ms -= step_ms;
      size_t idx = sys_rand32_get() % f->count;
      data->hue[idx] = kp_rain_drops_hue(base.h);
    }
    break;
  }
  case DT_ENUM_CONST(mode, fractal): { /* a single-hue pulse expanding horizontally from centre */
    data->phase_ms = (data->phase_ms + f->elapsed) % period;
    break;
  }
  default: { /* pixel / jellybean: occasionally spark a random key */
    if (sys_rand32_get() % 100 < 35) {
      size_t idx = sys_rand32_get() % f->count;
      data->val[idx] = 255;
      data->hue[idx] = (uint16_t)(sys_rand32_get() % KP_RGB_HUE_MAX);
      /* jellybean also randomises saturation; the others keep the preset. */
      data->sat[idx] =
          (cfg->mode == DT_ENUM_CONST(mode, jellybean))
              ? (uint8_t)(sys_rand32_get() % KP_RGB_SAT_MAX)
              : base.s;
    }
    break;
  }
  }

  for (size_t i = 0; i < f->count; i++) {
    uint8_t v = data->val[i];

    if (cfg->mode == DT_ENUM_CONST(mode, fractal)) {
      /* Recompute brightness from the expanding pulse (ignores val[]). */
      /* The pulse mirrors from the centre, so it has two fronts; halve the
       * temporal phase to keep each front at a single-front rate. */
      uint32_t phase01 =
          kp_rgb_arm_phase(data->phase_ms * 65536u / period, 2u);
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
      f->pixels[i] = kp_rgb_hsb_to_rgb(kp_rgb_hsb_scale(hsb, pct));
    } else {
      f->pixels[i] = (struct led_rgb){0, 0, 0};
    }
  }
}

#define KP_EFF_RAIN_DEFINE(inst)                                               \
  static const struct kp_eff_rain_config kp_eff_rain_##inst##_cfg = {          \
      .common = {.index = KP_RGB_EFFECT_INDEX(inst)},                          \
      .mode = CONV_DT_ENUM(inst, mode),                                        \
  };                                                                           \
  static struct kp_eff_rain_data kp_eff_rain_##inst##_data = {                 \
      .common =                                                                \
          {                                                                    \
              .color = KP_RGB_HSB_FROM_HEX(                                    \
                  DT_PROP_OR(DT_DRV_INST(inst), color, 0xFFFFFF)),             \
              .duration_ms = DT_PROP_OR(DT_DRV_INST(inst), duration, 0),       \
          },                                                                   \
  };                                                                           \
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), kp_eff_rain_render, NULL,            \
                       kp_eff_rain_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_EFF_RAIN_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
