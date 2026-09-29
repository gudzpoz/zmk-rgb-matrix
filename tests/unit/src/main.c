/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Host unit tests for the pure colour and math helpers. No ZMK app, no
 * devicetree: these translate units only.
 */

#include <zephyr/ztest.h>
#include <zephyr/sys/util.h>

#include <zmk/rgb_color.h>
#include <zmk/rgb_matrix_math.h>
#include <zmk/rgb_persist.h>

static void assert_rgb(struct led_rgb got, uint8_t r, uint8_t g, uint8_t b) {
  zassert_equal(got.r, r, "r: got %u want %u", got.r, r);
  zassert_equal(got.g, g, "g: got %u want %u", got.g, g);
  zassert_equal(got.b, b, "b: got %u want %u", got.b, b);
}

ZTEST(kp_rgb_color, hsb_to_rgb_primaries) {
  assert_rgb(kp_rgb_hsb_to_rgb((struct kp_rgb_hsb){.h = 0, .s = 100, .b = 100}),
             255, 0, 0);
  assert_rgb(
      kp_rgb_hsb_to_rgb((struct kp_rgb_hsb){.h = 60, .s = 100, .b = 100}), 255,
      255, 0);
  assert_rgb(
      kp_rgb_hsb_to_rgb((struct kp_rgb_hsb){.h = 120, .s = 100, .b = 100}), 0,
      255, 0);
  assert_rgb(
      kp_rgb_hsb_to_rgb((struct kp_rgb_hsb){.h = 180, .s = 100, .b = 100}), 0,
      255, 255);
  assert_rgb(
      kp_rgb_hsb_to_rgb((struct kp_rgb_hsb){.h = 240, .s = 100, .b = 100}), 0, 0,
      255);
  assert_rgb(
      kp_rgb_hsb_to_rgb((struct kp_rgb_hsb){.h = 300, .s = 100, .b = 100}), 255,
      0, 255);
}

ZTEST(kp_rgb_color, hsb_to_rgb_edges) {
  /* Hue wraps: 360 is 0, and an out-of-range hue is reduced too. */
  assert_rgb(
      kp_rgb_hsb_to_rgb((struct kp_rgb_hsb){.h = 360, .s = 100, .b = 100}), 255,
      0, 0);
  assert_rgb(
      kp_rgb_hsb_to_rgb((struct kp_rgb_hsb){.h = 720, .s = 100, .b = 100}), 255,
      0, 0);
  /* Zero saturation is white at full value; zero brightness is black. */
  assert_rgb(kp_rgb_hsb_to_rgb((struct kp_rgb_hsb){.h = 0, .s = 0, .b = 100}),
             255, 255, 255);
  assert_rgb(kp_rgb_hsb_to_rgb((struct kp_rgb_hsb){.h = 0, .s = 100, .b = 0}), 0,
             0, 0);
  /* Off-primary hue: integer rounding is pinned. */
  assert_rgb(
      kp_rgb_hsb_to_rgb((struct kp_rgb_hsb){.h = 0, .s = 50, .b = 100}), 255,
      128, 128);
  assert_rgb(
      kp_rgb_hsb_to_rgb((struct kp_rgb_hsb){.h = 210, .s = 100, .b = 100}), 0,
      128, 255);
}

ZTEST(kp_rgb_color, hsb_scale_only_changes_brightness) {
  struct kp_rgb_hsb c = {.h = 200, .s = 50, .b = 100};

  struct kp_rgb_hsb half = kp_rgb_hsb_scale(c, 50);
  zassert_equal(half.h, 200);
  zassert_equal(half.s, 50);
  zassert_equal(half.b, 50);

  /* pct above KP_RGB_BRT_MAX clamps to 100, so brightness is unchanged. */
  zassert_equal(kp_rgb_hsb_scale(c, 150).b, 100);
  zassert_equal(kp_rgb_hsb_scale(c, 0).b, 0);
  /* Rounding truncates toward zero. */
  zassert_equal(kp_rgb_hsb_scale((struct kp_rgb_hsb){.h = 0, .s = 0, .b = 37}, 50).b,
                18);
}

ZTEST(kp_rgb_color, rgb_scale_clamps) {
  struct led_rgb c = {.r = 200, .g = 100, .b = 50};

  assert_rgb(kp_rgb_rgb_scale(c, 50), 100, 50, 25);
  assert_rgb(kp_rgb_rgb_scale((struct led_rgb){.r = 255, .g = 255, .b = 255}, 50),
             127, 127, 127);
  assert_rgb(kp_rgb_rgb_scale((struct led_rgb){.r = 1, .g = 2, .b = 3}, 50), 0,
             1, 1);

  assert_rgb(kp_rgb_rgb_scale(c, 0), 0, 0, 0);
  assert_rgb(kp_rgb_rgb_scale(c, 100), 200, 100, 50);
  /* Above 100 clamps rather than brightening past the input. */
  assert_rgb(kp_rgb_rgb_scale(c, 150), 200, 100, 50);
}

ZTEST(kp_rgb_color, rgb_mix_endpoints_and_truncation) {
  struct led_rgb black = {0, 0, 0};
  struct led_rgb white = {255, 255, 255};

  assert_rgb(kp_rgb_rgb_mix(black, white, 0), 0, 0, 0);
  assert_rgb(kp_rgb_rgb_mix(black, white, 100), 255, 255, 255);
  assert_rgb(kp_rgb_rgb_mix(black, white, 150), 255, 255, 255);

  /* The blend is asymmetric: (over - base) truncates toward zero, so a 50%
   * mix up from black lands on 127 while the reverse lands on 128. */
  assert_rgb(kp_rgb_rgb_mix(black, white, 50), 127, 127, 127);
  assert_rgb(kp_rgb_rgb_mix(white, black, 50), 128, 128, 128);

  assert_rgb(kp_rgb_rgb_mix((struct led_rgb){1, 1, 1},
                            (struct led_rgb){2, 2, 2}, 50),
             1, 1, 1);
  assert_rgb(kp_rgb_rgb_mix((struct led_rgb){10, 10, 10}, black, 50), 5, 5, 5);
}

ZTEST(kp_rgb_color, scale_macro) {
  zassert_equal(KP_RGB_SCALE(200, 50), 100);
  zassert_equal(KP_RGB_SCALE(1, 50), 0);
  zassert_equal(KP_RGB_SCALE(255, 100), 255);
}

ZTEST(kp_rgb_math, isqrt) {
  zassert_equal(kp_rgb_isqrt(0), 0);
  zassert_equal(kp_rgb_isqrt(1), 1);
  zassert_equal(kp_rgb_isqrt(2), 1);
  zassert_equal(kp_rgb_isqrt(4), 2);
  zassert_equal(kp_rgb_isqrt(9), 3);
  zassert_equal(kp_rgb_isqrt(16), 4);
  zassert_equal(kp_rgb_isqrt(255), 15);
  zassert_equal(kp_rgb_isqrt(256), 16);
  zassert_equal(kp_rgb_isqrt(1000), 31);
  zassert_equal(kp_rgb_isqrt(1000000), 1000);
  zassert_equal(kp_rgb_isqrt(UINT32_MAX), 65535);
}

ZTEST(kp_rgb_math, sin8_core) {
  zassert_equal(kp_rgb_sin8(0), 128);
  zassert_equal(kp_rgb_sin8(64), 255);
  zassert_equal(kp_rgb_sin8(128), 128);
  zassert_equal(kp_rgb_sin8(192), 0);
}

ZTEST(kp_rgb_math, atan2_8_cardinals) {
  zassert_equal(kp_rgb_atan2_8(0, 1), 0);
  zassert_equal(kp_rgb_atan2_8(0, -1), 128);
  zassert_equal(kp_rgb_atan2_8(1, 0), 64);
  zassert_equal(kp_rgb_atan2_8(-1, 0), 192);
  zassert_equal(kp_rgb_atan2_8(1, 1), 32);
  zassert_equal(kp_rgb_atan2_8(1, -1), 96);
  zassert_equal(kp_rgb_atan2_8(-1, 1), 224);
  zassert_equal(kp_rgb_atan2_8(-5, -5), 160);
  zassert_equal(kp_rgb_atan2_8(5, -5), 96);
}

ZTEST(kp_rgb_math, arm_phase) {
  zassert_equal(kp_rgb_arm_phase(100, 1), 100);
  zassert_equal(kp_rgb_arm_phase(100, 2), 50);
  zassert_equal(kp_rgb_arm_phase(100, 3), 33);
  zassert_equal(kp_rgb_arm_phase(100, 0), 100);
  zassert_equal(kp_rgb_arm_phase(100, 4), 25);
}

ZTEST(kp_rgb_persist, rejects_wrong_size) {
  zassert_true(kp_rgb_persist_size_ok(sizeof(struct kp_rgb_persist_blob)));
  zassert_false(kp_rgb_persist_size_ok(sizeof(struct kp_rgb_persist_blob) - 1));
  zassert_false(kp_rgb_persist_size_ok(0));
}

ZTEST(kp_rgb_persist, rejects_wrong_version) {
  zassert_true(kp_rgb_persist_version_ok(KP_RGB_PERSIST_VERSION));
  zassert_false(kp_rgb_persist_version_ok(0));
  zassert_false(kp_rgb_persist_version_ok(KP_RGB_PERSIST_VERSION + 1));
}

ZTEST(kp_rgb_persist, effect_range_check) {
  zassert_true(kp_rgb_persist_effect_valid(
      &(struct kp_rgb_persist_effect){.h = KP_RGB_HUE_MAX,
                                      .s = KP_RGB_SAT_MAX,
                                      .b = KP_RGB_BRT_MAX}));
  zassert_false(kp_rgb_persist_effect_valid(
      &(struct kp_rgb_persist_effect){.h = KP_RGB_HUE_MAX + 1}));
  zassert_false(kp_rgb_persist_effect_valid(
      &(struct kp_rgb_persist_effect){.s = KP_RGB_SAT_MAX + 1}));
  zassert_false(kp_rgb_persist_effect_valid(
      &(struct kp_rgb_persist_effect){.b = KP_RGB_BRT_MAX + 1}));
}

ZTEST(kp_rgb_persist, duration_clamp) {
  zassert_equal(kp_rgb_persist_clamp_duration(50, 100, 10000), 100);
  zassert_equal(kp_rgb_persist_clamp_duration(20000, 100, 10000), 10000);
  zassert_equal(kp_rgb_persist_clamp_duration(500, 100, 10000), 500);
}

ZTEST(kp_rgb_persist, pack_round_trip) {
  const struct kp_rgb_persist_effect in[2] = {
      {.duration_ms = 500, .h = 10, .s = 20, .b = 30},
      {.duration_ms = 1000, .h = 40, .s = 50, .b = 60},
  };
  struct kp_rgb_persist_blob blob;

  kp_rgb_persist_pack(7, true, in, 2, &blob);
  zassert_equal(blob.version, KP_RGB_PERSIST_VERSION);
  zassert_equal(blob.selected_index, 7);
  zassert_equal(blob.user_on, 1);
  zassert_equal(blob.effect_count, 2);
  zassert_true(kp_rgb_persist_size_ok(sizeof(blob)));
  zassert_true(kp_rgb_persist_version_ok(blob.version));
  zassert_mem_equal(&blob.effects[0], &in[0], sizeof(in[0]));
  zassert_mem_equal(&blob.effects[1], &in[1], sizeof(in[1]));
  /* Unused slots stay zeroed. */
  zassert_equal(blob.effects[2].duration_ms, 0);
  zassert_equal(blob.effects[2].h, 0);

  /* Over-long input is truncated to the registry cap, never overflowing. */
  struct kp_rgb_persist_effect many[KP_RGB_PERSIST_MAX_EFFECTS + 4] = {0};
  kp_rgb_persist_pack(0, false, many, ARRAY_SIZE(many), &blob);
  zassert_equal(blob.effect_count, KP_RGB_PERSIST_MAX_EFFECTS);
  zassert_equal(blob.user_on, 0);
}

ZTEST_SUITE(kp_rgb_color, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(kp_rgb_math, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(kp_rgb_persist, NULL, NULL, NULL, NULL, NULL);
