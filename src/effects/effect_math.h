/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <zmk/rgb_matrix_math.h>

#define KP_RGB_EFFECT_PENDING_EVENTS 16

/* Coordinate deltas are in -65535..65535. */
static inline uint64_t kp_rgb_coord_distance_squared(int32_t dx, int32_t dy) {
  return (uint64_t)((int64_t)dx * dx) + (uint64_t)((int64_t)dy * dy);
}

/* squared <= 2 * 65535^2; the largest resulting distance is 92680. */
static inline uint32_t kp_rgb_coord_distance(uint64_t squared) {
  if (squared <= UINT32_MAX) {
    return kp_rgb_isqrt((uint32_t)squared);
  }
  uint32_t low = 65536u;
  uint32_t high = 92680u;
  while (low <= high) {
    uint32_t mid = low + (high - low) / 2u;
    if ((uint64_t)mid * mid > squared) {
      high = mid - 1u;
    } else {
      low = mid + 1u;
    }
  }
  return high;
}

static inline uint32_t kp_rgb_spatial_linear(uint32_t position, uint32_t extent) {
  if (position <= UINT16_MAX) {
    return position * 65536u / extent;
  }
  return (uint32_t)((uint64_t)position * 65536u / extent);
}

static inline uint32_t kp_rgb_spatial_angle(int32_t dy, int32_t dx) {
  return (uint32_t)kp_rgb_atan2_8(dy, dx) * 256u;
}

static inline uint32_t kp_rgb_spatial_radius(int32_t dx, int32_t dy, uint32_t max_radius) {
  return kp_rgb_spatial_linear(kp_rgb_coord_distance(kp_rgb_coord_distance_squared(dx, dy)),
                               max_radius);
}

/* divisor is in 1..65535; scaled_elapsed is at most UINT32_MAX * 255. */
static inline uint8_t kp_rgb_decay_u8(uint8_t value, uint16_t *remainder,
                                    uint64_t scaled_elapsed, uint32_t divisor) {
  uint64_t total = scaled_elapsed + *remainder;
  uint64_t loss = total / divisor;
  if (loss >= value) {
    *remainder = 0;
    return 0;
  }
  *remainder = (uint16_t)(total % divisor);
  return (uint8_t)(value - loss);
}
