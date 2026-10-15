/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/settings/settings.h>

#include <zmk/rgb_color.h>

/* ID buffers include the terminating NUL. */
#define KP_RGB_PERSIST_MAX_ID_LENGTH 32
#define KP_RGB_PERSIST_MAX_SELECTOR_ID_LENGTH 31
#define KP_RGB_PERSIST_GLOBAL_VERSION 3
#define KP_RGB_PERSIST_SELECTOR_VERSION 1
#define KP_RGB_PERSIST_SUBTREE "keypaw/rgb_matrix"
#define KP_RGB_PERSIST_SELECTOR_PREFIX KP_RGB_PERSIST_SUBTREE "/selectors/"

struct kp_rgb_persist_global_record {
  uint8_t version;
  uint8_t user_on;
  uint8_t brightness;
  uint8_t reserved;
};

struct kp_rgb_persist_selector_record {
  uint8_t version;
  char selected_id[KP_RGB_PERSIST_MAX_ID_LENGTH];
};

struct kp_rgb_persist_global_state {
  bool user_on;
  uint8_t brightness;
};

struct kp_rgb_persist_effect_id {
  const char *id; /* NULL skips a disabled child. */
};

struct kp_rgb_persist_selector_ids {
  const char *id;
  const struct kp_rgb_persist_effect_id *effects;
  size_t effect_count;
};

/* All codecs return 0 on success or -EINVAL for an invalid record. They leave
 * out unchanged on failure. Decoders require exact lengths and current versions,
 * canonical Booleans and zero reserved bytes. Selected IDs must be nonempty,
 * NUL-terminated strings of at most 31 bytes; characters and uniqueness are unchecked.
 */
int kp_rgb_persist_encode_global(
    const struct kp_rgb_persist_global_state *state,
    struct kp_rgb_persist_global_record *out);
int kp_rgb_persist_decode_global(const void *bytes, size_t len,
                                 struct kp_rgb_persist_global_state *out);
int kp_rgb_persist_encode_selector(const char *selected_id,
                                   struct kp_rgb_persist_selector_record *out);
int kp_rgb_persist_decode_selector(const void *bytes, size_t len,
                                   struct kp_rgb_persist_selector_record *out);
