/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

/* Adapted from QMK lib8tion/trig8.h (MIT license). */
static inline uint8_t kp_rgb_atan2_8(int32_t dy, int32_t dx) {
  if (dy == 0) {
    return dx >= 0 ? 0 : 128;
  }

  int32_t abs_y = dy > 0 ? dy : -dy;
  int32_t a;

  if (dx >= 0) {
    a = 32 - (32 * (dx - abs_y) / (dx + abs_y));
  } else {
    a = 96 - (32 * (dx + abs_y) / (abs_y - dx));
  }

  return dy < 0 ? (uint8_t)-a : (uint8_t)a;
}

/* Adapted from QMK lib8tion/sin8 (MIT license). `theta` spans one full cycle
 * over 0-255 and the output is 0-255 centred at 128: kp_rgb_sin8(0) = 128,
 * kp_rgb_sin8(64) = 255, kp_rgb_sin8(192) = 0. Use it instead of a linear
 * triangle for any breathing/oscillating value so it eases in and out rather
 * than reversing sharply at the peaks. */
static inline uint8_t kp_rgb_sin8(uint8_t theta) {
  static const uint8_t lut[256] = {
      128, 131, 134, 137, 140, 143, 146, 149, 152, 155, 158, 162, 165, 167,
      170, 173, 176, 179, 182, 185, 188, 190, 193, 196, 198, 201, 203, 206,
      208, 211, 213, 215, 218, 220, 222, 224, 226, 228, 230, 232, 234, 235,
      237, 238, 240, 241, 243, 244, 245, 246, 248, 249, 250, 250, 251, 252,
      253, 253, 254, 254, 254, 255, 255, 255, 255, 255, 255, 255, 254, 254,
      254, 253, 253, 252, 251, 250, 250, 249, 248, 246, 245, 244, 243, 241,
      240, 238, 237, 235, 234, 232, 230, 228, 226, 224, 222, 220, 218, 215,
      213, 211, 208, 206, 203, 201, 198, 196, 193, 190, 188, 185, 182, 179,
      176, 173, 170, 167, 165, 162, 158, 155, 152, 149, 146, 143, 140, 137,
      134, 131, 128, 124, 121, 118, 115, 112, 109, 106, 103, 100, 97,  93,
      90,  88,  85,  82,  79,  76,  73,  70,  67,  65,  62,  59,  57,  54,
      52,  49,  47,  44,  42,  40,  37,  35,  33,  31,  29,  27,  25,  23,
      21,  20,  18,  17,  15,  14,  12,  11,  10,  9,   7,   6,   5,   5,
      4,   3,   2,   2,   1,   1,   1,   0,   0,   0,   0,   0,   0,   0,
      1,   1,   1,   2,   2,   3,   4,   5,   5,   6,   7,   9,   10,  11,
      12,  14,  15,  17,  18,  20,  21,  23,  25,  27,  29,  31,  33,  35,
      37,  40,  42,  44,  47,  49,  52,  54,  57,  59,  62,  65,  67,  70,
      73,  76,  79,  82,  85,  88,  90,  93,  97,  100, 103, 106, 109, 112,
      115, 118, 121, 124};
  return lut[theta];
}

/* Temporal phase for an effect whose spatial coordinate wraps `arms` times
 * across the board. Dividing the (16.16) temporal phase by the arm count keeps
 * each wrap's front moving at the same physical rate as a single-wrap effect,
 * so multi-arm patterns do not outrun single-arm ones at the same period. */
static inline uint32_t kp_rgb_arm_phase(uint32_t phase01, uint32_t arms) {
  return arms > 1 ? phase01 / arms : phase01;
}

/* Adapted from QMK lib8tion/math8.h sqrt16 (MIT license). The input is widened
 * to uint32_t because spatial squared distances use 32-bit intermediates. */
static inline uint32_t kp_rgb_isqrt(uint32_t x) {
  if (x <= 1u) {
    return x;
  }

  uint32_t low = 1u;
  uint32_t hi = x > 2096864u ? UINT16_MAX : (x >> 5) + 8u;
  uint32_t mid;

  do {
    mid = (low + hi) >> 1;
    if (mid * mid > x) {
      hi = mid - 1u;
    } else {
      low = mid + 1u;
    }
  } while (hi >= low);

  return low - 1u;
}
