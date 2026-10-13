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
#define KP_RGB_PERSIST_PARAMETERS_VERSION 1
#define KP_RGB_PERSIST_SUBTREE "keypaw/rgb_matrix"
#define KP_RGB_PERSIST_SELECTOR_PREFIX KP_RGB_PERSIST_SUBTREE "/selectors/"
#define KP_RGB_PERSIST_EFFECT_ID_BUDGET                                        \
  (SETTINGS_MAX_NAME_LEN - (sizeof(KP_RGB_PERSIST_SELECTOR_PREFIX) - 1) -      \
   (sizeof("/effects/") - 1))

struct kp_rgb_persist_effect {
  uint16_t duration_ms;
  uint16_t h;
  uint8_t s;
};

/* Duration and hue in parameter records are little-endian. */
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

struct kp_rgb_persist_parameters_record {
  uint8_t version;
  uint8_t duration_le[2];
  uint8_t hue_le[2];
  uint8_t saturation;
};

struct kp_rgb_persist_global_state {
  bool user_on;
  uint8_t brightness;
};

struct kp_rgb_persist_effect_id {
  const char *id; /* NULL skips a disabled child. */
  bool persist_parameters;
};

struct kp_rgb_persist_selector_ids {
  const char *id;
  const struct kp_rgb_persist_effect_id *effects;
  size_t effect_count;
};

bool kp_rgb_persist_effect_valid(const struct kp_rgb_persist_effect *effect);
uint16_t kp_rgb_persist_clamp_duration(uint16_t duration_ms, uint16_t min_ms,
                                       uint16_t max_ms);

/* IDs use ASCII letters, digits, '_', '-', '.', or '@'. Selector IDs are
 * 1..30 characters and unique globally; enabled member IDs are 1..31 characters
 * and unique within their selector.
 * Opted-in parameter keys must fit SETTINGS_MAX_NAME_LEN, including both IDs.
 * validate_ids returns 0, -EINVAL for invalid syntax/tables, -EEXIST for
 * duplicate IDs, or -ENAMETOOLONG for an oversized settings key.
 */
bool kp_rgb_persist_id_valid(const char *id);
int kp_rgb_persist_validate_ids(
    const struct kp_rgb_persist_selector_ids *selectors, size_t count);

/* All codecs return 0 on success or -EINVAL for an invalid record. They leave
 * out unchanged on failure. Decoders require exact lengths and current versions,
 * canonical Booleans, zero reserved bytes and valid IDs where applicable.
 * Duration clamping belongs to restoration.
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
int kp_rgb_persist_encode_parameters(
    const struct kp_rgb_persist_effect *state,
    struct kp_rgb_persist_parameters_record *out);
int kp_rgb_persist_decode_parameters(const void *bytes, size_t len,
                                     struct kp_rgb_persist_effect *out);
