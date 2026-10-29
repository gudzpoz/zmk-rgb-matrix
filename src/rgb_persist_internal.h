/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KP_RGB_PERSIST_SUBTREE "keypaw/rgb_matrix"
#define KP_RGB_PERSIST_KEY KP_RGB_PERSIST_SUBTREE "/snapshot"
#define KP_RGB_PERSIST_VERSION 1u
#define KP_RGB_PERSIST_HEADER_SIZE 6u
#define KP_RGB_PERSIST_SIZE(count) (KP_RGB_PERSIST_HEADER_SIZE + 2u * (size_t)(count))

struct kp_rgb_persist_state {
  bool user_on;
  uint8_t brightness;
  uint16_t selection_count;
};

/* Wire bytes: version, intent, brightness, zero, LE16 count, then LE16 slots.
 * Encoding requires the exact wire length. Neither codec changes outputs on error.
 * Decoding validates record_len against its encoded count; prefix_len must contain
 * the header and min(count, capacity) slots. Excess slots need not be read. */
int kp_rgb_persist_encode(const struct kp_rgb_persist_state *state,
                          const uint16_t *selections, uint8_t *bytes, size_t len);
int kp_rgb_persist_decode(const uint8_t *prefix, size_t prefix_len, size_t record_len,
                          struct kp_rgb_persist_state *state, uint16_t *selections,
                          size_t capacity);
