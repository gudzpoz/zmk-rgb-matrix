/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * "digital_rain" effect: a Matrix-style column of falling heads with a fading
 * green (or preset-hue) tail, ported from QMK's DIGITAL_RAIN. Each LED is
 * binned into a column by its layout x; every column owns a head that falls
 * downward, and the LEDs just above the head glow with a tail that fades out.
 */

#define DT_DRV_COMPAT keypaw_rgb_matrix_digital_rain

#include <stdint.h>

#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>

#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/* Maximum number of discrete columns the LEDs are binned into by x. */
#define KP_DIGITAL_COLS 24

struct kp_eff_digital_rain_config {
  struct kp_rgb_effect_common_config common;
};

struct kp_eff_digital_rain_data {
  struct kp_rgb_effect_common_data common;
  bool seeded;
  uint16_t min_x;
  uint16_t max_x;
  uint16_t min_y;
  uint16_t max_y;
  uint8_t col_of_led[KP_LED_COUNT]; /* column index per LED */
  int16_t head_y[KP_DIGITAL_COLS];  /* current falling head y per column */
  int16_t speed[KP_DIGITAL_COLS];    /* layout-units per ms, per column */
};

/* Discover the board's x/y extents and assign each LED to a column. Must run
 * after the engine has resolved `coords`, so it is done lazily on first render.
 */
static void kp_eff_digital_rain_seed(const struct device *dev,
                                     struct kp_rgb_frame *f) {
  struct kp_eff_digital_rain_data *data = dev->data;
  uint16_t min_x = 0xFFFF, max_x = 0, min_y = 0xFFFF, max_y = 0;

  for (size_t i = 0; i < f->count; i++) {
    if (f->coords[i].x < min_x) {
      min_x = f->coords[i].x;
    }
    if (f->coords[i].x > max_x) {
      max_x = f->coords[i].x;
    }
    if (f->coords[i].y < min_y) {
      min_y = f->coords[i].y;
    }
    if (f->coords[i].y > max_y) {
      max_y = f->coords[i].y;
    }
  }
  data->min_x = min_x;
  data->max_x = max_x;
  data->min_y = min_y;
  data->max_y = max_y;

  uint32_t span_x = (uint32_t)max_x - min_x + 1u;
  uint32_t range_y = MAX((uint32_t)max_y - min_y, 1u);
  uint32_t duration = kp_rgb_effect_period(dev);

  for (size_t i = 0; i < f->count; i++) {
    data->col_of_led[i] =
        (uint8_t)((f->coords[i].x - min_x) * KP_DIGITAL_COLS / span_x);
  }

  for (uint8_t c = 0; c < KP_DIGITAL_COLS; c++) {
    /* Stagger the heads across and above the board so they don't fall in unison.
     * A negative head is still above the top edge, dropping in over time. */
    data->head_y[c] = (int16_t)((int32_t)min_y - (sys_rand32_get() % range_y));
    /* Speed in layout-units per ms; each column varies a little. */
    uint32_t frac = 50u + (sys_rand32_get() % 100u); /* 0.5x .. 1.49x */
    uint32_t sp = range_y * frac / (duration * 50u);
    data->speed[c] = (int16_t)(sp < 1u ? 1u : sp);
  }
}

static void kp_eff_digital_rain_render(const struct device *dev,
                                       struct kp_rgb_frame *f) {
  struct kp_eff_digital_rain_data *data = dev->data;
  struct kp_rgb_hsb base = data->common.color;
  uint8_t pct = kp_rgb_brightness_pct(f);
  /* Tail length in layout units: a quarter of the board height, at least a few. */
  uint16_t trail = MAX(f->board_height / 4u, 4u);

  if (!data->seeded) {
    kp_eff_digital_rain_seed(dev, f);
    data->seeded = true;
  }

  /* Advance every column's head and recycle it once it has cleared the bottom. */
  uint16_t range_y = MAX((uint16_t)(data->max_y - data->min_y), 1u);
  for (uint8_t c = 0; c < KP_DIGITAL_COLS; c++) {
    data->head_y[c] =
        (int16_t)((int32_t)data->head_y[c] +
                  (int32_t)data->speed[c] * (int32_t)f->elapsed);
    if (data->head_y[c] > (int16_t)(data->max_y + trail)) {
      data->head_y[c] =
          (int16_t)((int32_t)data->min_y - (sys_rand32_get() % range_y));
      uint32_t frac = 50u + (sys_rand32_get() % 100u);
      uint32_t sp = range_y * frac / (kp_rgb_effect_period(dev) * 50u);
      data->speed[c] = (int16_t)(sp < 1u ? 1u : sp);
    }
  }

  for (size_t i = 0; i < f->count; i++) {
    uint8_t c = data->col_of_led[i];
    int32_t d = (int32_t)data->head_y[c] - (int32_t)f->coords[i].y;
    /* Only the trail behind the falling head (the LEDs above it, smaller y) glow;
     * everything the head hasn't reached yet, and everything past the tail end, is
     * dark. */
    if (d < 0 || d > (int32_t)trail) {
      f->pixels[i] = (struct led_rgb){0, 0, 0};
      continue;
    }
    /* b: 255 right at the head, fading to 0 at the tail end. */
    uint32_t b = 255u * (uint32_t)(trail - d) / trail;
    struct kp_rgb_hsb hsb;
    hsb.h = base.h;
    /* Whiter at the head, full preset hue toward the tail. */
    hsb.s = (uint8_t)((255u - b) * KP_RGB_SAT_MAX / 255u);
    hsb.b = (uint8_t)b;
    f->pixels[i] = kp_rgb_hsb_to_rgb(kp_rgb_hsb_scale(hsb, pct));
  }
}

#define KP_EFF_DIGITAL_RAIN_DEFINE(inst)                                         \
  static const struct kp_eff_digital_rain_config                                 \
      kp_eff_digital_rain_##inst##_cfg = {                                      \
          .common = {.index = DT_PROP(DT_DRV_INST(inst), index)},                \
  };                                                                            \
  static struct kp_eff_digital_rain_data kp_eff_digital_rain_##inst##_data = {   \
      .common =                                                                 \
          {                                                                     \
              .color = KP_RGB_HSB_FROM_HEX(                                      \
                  DT_PROP_OR(DT_DRV_INST(inst), color, 0x00FF00)),               \
              .duration_ms = DT_PROP_OR(DT_DRV_INST(inst), duration, 4000),      \
          },                                                                    \
  };                                                                            \
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), kp_eff_digital_rain_render, NULL,      \
                       kp_eff_digital_rain_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_EFF_DIGITAL_RAIN_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
