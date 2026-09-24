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

/* One key row, in layout units: physical-layout units are 100 per key. */
#define KP_RGB_KEY_UNIT 100u

/* Tail length, in key rows. QMK's DIGITAL_RAIN has no tail parameter at all: a
 * cell decays over about 255 frames while the pattern shifts down one row every
 * 29, so the tail settles at roughly nine rows at full brightness whatever the
 * board size and column count. Counting in rows (rather than absolute layout
 * units, which scale with the key pitch) is what keeps that relationship. */
#define KP_DIGITAL_RAIN_TRAIL_ROWS 9u

/* Heads and speeds are held in 1/256 of a layout unit. A column falls at
 * 0.075-0.22 units/ms for the durations this module clamps to, which truncates
 * to zero in whole units -- and a zero speed guard would then pin every column
 * to the same minimum. Multiplying by a constant rather than shifting keeps the
 * negative (above-the-board) start positions well defined. */
#define KP_RAIN_Q8 (1 << 8)
#define KP_RAIN_TO_Q8(v) ((int32_t)(v) * KP_RAIN_Q8)
#define KP_RAIN_FROM_Q8(v) ((int32_t)(v) / KP_RAIN_Q8)

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
  int32_t head_q8[KP_DIGITAL_COLS];  /* falling head y, 1/256 layout units */
  int32_t speed_q8[KP_DIGITAL_COLS]; /* fall speed, 1/256 units per ms */
};

/* Where a recycled head restarts: somewhere above the top edge, so the columns
 * do not drop in unison. */
static int32_t kp_eff_digital_rain_start_q8(uint16_t min_y, uint16_t range_y) {
  return KP_RAIN_TO_Q8((int32_t)min_y - (int32_t)(sys_rand32_get() % range_y));
}

/* Fall speed for one column: it crosses the board height in `period / jitter`,
 * where jitter is 1.00x..2.98x so the columns drift apart. Tying it to the
 * period is what makes `duration` mean "shorter is faster". */
static int32_t kp_eff_digital_rain_speed_q8(uint16_t range_y, uint32_t period) {
  uint32_t frac = 50u + (sys_rand32_get() % 100u);
  uint32_t sp_q8 = (uint32_t)range_y * KP_RAIN_Q8 * frac / (period * 50u);
  return (int32_t)MAX(sp_q8, 1u);
}

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
    data->head_q8[c] = kp_eff_digital_rain_start_q8(min_y, (uint16_t)range_y);
    data->speed_q8[c] = kp_eff_digital_rain_speed_q8((uint16_t)range_y, duration);
  }
}

static void kp_eff_digital_rain_render(const struct device *dev,
                                       struct kp_rgb_frame *f) {
  struct kp_eff_digital_rain_data *data = dev->data;
  struct kp_rgb_hsb base = data->common.color;
  uint8_t pct = kp_rgb_brightness_pct(f);
  /* Tail length in layout units, from the row count above. Capped at the board
   * height so a short board fades out across its full height instead of glowing
   * uniformly (a nine-row tail cannot fit a four-row board). */
  uint16_t trail = MIN(KP_DIGITAL_RAIN_TRAIL_ROWS * KP_RGB_KEY_UNIT,
                       MAX(f->board_height, KP_RGB_KEY_UNIT));

  if (!data->seeded) {
    kp_eff_digital_rain_seed(dev, f);
    data->seeded = true;
  }

  /* Advance every column's head and recycle it once it has cleared the bottom. */
  uint16_t range_y = MAX((uint16_t)(data->max_y - data->min_y), 1u);
  int32_t recycle_q8 = KP_RAIN_TO_Q8((int32_t)data->max_y + (int32_t)trail);
  uint32_t period = kp_rgb_effect_period(dev);
  for (uint8_t c = 0; c < KP_DIGITAL_COLS; c++) {
    data->head_q8[c] += data->speed_q8[c] * (int32_t)f->elapsed;
    if (data->head_q8[c] > recycle_q8) {
      data->head_q8[c] = kp_eff_digital_rain_start_q8(data->min_y, range_y);
      data->speed_q8[c] = kp_eff_digital_rain_speed_q8(range_y, period);
    }
  }

  for (size_t i = 0; i < f->count; i++) {
    uint8_t c = data->col_of_led[i];
    int32_t d = KP_RAIN_FROM_Q8(data->head_q8[c]) - (int32_t)f->coords[i].y;
    /* Only the trail behind the falling head (the LEDs above it, smaller y) glow;
     * everything the head hasn't reached yet, and everything past the tail end, is
     * dark. */
    if (d < 0 || d > (int32_t)trail) {
      f->pixels[i] = (struct led_rgb){0, 0, 0};
      continue;
    }
    /* b: 255 right at the head, fading to 0 at the tail end. A 0..255 factor,
     * scaled onto the preset brightness -- hsb.b is 0..100, so assigning it
     * directly overflows. */
    uint32_t b = 255u * (uint32_t)(trail - d) / trail;
    struct kp_rgb_hsb hsb;
    hsb.h = base.h;
    /* Whiter at the head, full preset hue toward the tail. */
    hsb.s = (uint8_t)((255u - b) * KP_RGB_SAT_MAX / 255u);
    hsb.b = (uint8_t)((uint32_t)base.b * b / 255u);
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
