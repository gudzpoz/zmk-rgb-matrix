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

#define KP_RGB_PERSIST_VERSION 2
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

/* Legacy records retain their original layout and validation rules. */
struct kp_rgb_persist_effect {
  uint16_t duration_ms;
  uint16_t h;
  uint8_t s;
  uint8_t reserved;
};

struct kp_rgb_persist_blob {
  uint8_t version;
  uint8_t user_on;
  uint8_t brightness;
  uint8_t has_selected;
  char selected_id[KP_RGB_PERSIST_MAX_ID_LENGTH];
};

/* New records contain only bytes: duration and hue are little-endian. */
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

bool kp_rgb_persist_size_ok(size_t len);
bool kp_rgb_persist_version_ok(uint8_t version);
bool kp_rgb_persist_effect_valid(const struct kp_rgb_persist_effect *effect);
uint16_t kp_rgb_persist_clamp_duration(uint16_t duration_ms, uint16_t min_ms,
                                       uint16_t max_ms);
void kp_rgb_persist_pack(bool user_on, uint8_t brightness,
                         const char *selected_id,
                         struct kp_rgb_persist_blob *out);

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
 * out unchanged on failure. Decode exact-length records before resolving them.
 * Legacy decoders accept the existing format, including nonzero user_on values
 * and ignored effect reserved bytes. New records require canonical Booleans,
 * zero reserved bytes and valid IDs. Duration clamping belongs to restoration.
 */
int kp_rgb_persist_decode_legacy(const void *bytes, size_t len,
                                 struct kp_rgb_persist_blob *out);
int kp_rgb_persist_decode_legacy_effect(const void *bytes, size_t len,
                                        struct kp_rgb_persist_effect *out);
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

/* Supply valid defaults and decoded records, or NULL for absent/rejected
 * records. default_id must name an enabled member. New records take
 * precedence over legacy records regardless of load order. Unknown selection
 * IDs use default_id, not legacy selection. Members with NULL IDs are skipped.
 * Only the explicit global-behavior alias may import legacy selector state;
 * parameters also require persist_parameters=true. Return true when legacy
 * state was imported. Retain legacy storage until the imported scoped records
 * have been successfully saved; these helpers perform no I/O or live mutation.
 */
bool kp_rgb_persist_resolve_global(
    const struct kp_rgb_persist_global_state *defaults,
    const struct kp_rgb_persist_blob *legacy,
    const struct kp_rgb_persist_global_state *scoped,
    struct kp_rgb_persist_global_state *out);
bool kp_rgb_persist_resolve_selector(
    const char *default_id, const struct kp_rgb_persist_blob *legacy,
    bool alias, const struct kp_rgb_persist_selector_record *scoped,
    const struct kp_rgb_persist_effect_id *members, size_t count,
    struct kp_rgb_persist_selector_record *out);
bool kp_rgb_persist_resolve_parameters(
    const struct kp_rgb_persist_effect *defaults,
    const struct kp_rgb_persist_effect *legacy,
    const struct kp_rgb_persist_effect *scoped, bool alias,
    bool persist_parameters, struct kp_rgb_persist_effect *out);
