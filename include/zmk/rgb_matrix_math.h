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
