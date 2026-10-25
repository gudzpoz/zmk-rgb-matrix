/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>

#include "rgb_persist_internal.h"

static uint16_t kp_read_u16(const uint8_t *bytes) {
  return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static void kp_write_u16(uint8_t *bytes, uint16_t value) {
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8);
}

int kp_rgb_persist_encode(const struct kp_rgb_persist_state *state,
                          const uint16_t *selections, uint8_t *bytes, size_t len) {
  if (state == NULL || bytes == NULL || state->brightness > 100 ||
      (state->selection_count != 0 && selections == NULL) ||
      len != KP_RGB_PERSIST_SIZE(state->selection_count))
    return -EINVAL;

  bytes[0] = KP_RGB_PERSIST_VERSION;
  bytes[1] = state->user_on ? 1 : 0;
  bytes[2] = state->brightness;
  bytes[3] = 0;
  kp_write_u16(bytes + 4, state->selection_count);
  for (size_t i = 0; i < state->selection_count; i++)
    kp_write_u16(bytes + KP_RGB_PERSIST_HEADER_SIZE + 2 * i, selections[i]);
  return 0;
}

int kp_rgb_persist_decode(const uint8_t *prefix, size_t prefix_len, size_t record_len,
                          struct kp_rgb_persist_state *state, uint16_t *selections,
                          size_t capacity) {
  if (prefix == NULL || state == NULL || prefix_len < KP_RGB_PERSIST_HEADER_SIZE ||
      prefix_len > record_len || prefix[0] != KP_RGB_PERSIST_VERSION ||
      prefix[1] > 1 || prefix[2] > 100 || prefix[3] != 0)
    return -EINVAL;

  uint16_t count = kp_read_u16(prefix + 4);
  size_t copied = count < capacity ? count : capacity;
  if (record_len != KP_RGB_PERSIST_SIZE(count) ||
      prefix_len < KP_RGB_PERSIST_SIZE(copied) ||
      (copied != 0 && selections == NULL))
    return -EINVAL;

  for (size_t i = 0; i < copied; i++)
    selections[i] = kp_read_u16(prefix + KP_RGB_PERSIST_HEADER_SIZE + 2 * i);
  *state = (struct kp_rgb_persist_state){
      .user_on = prefix[1] != 0,
      .brightness = prefix[2],
      .selection_count = count,
  };
  return 0;
}
