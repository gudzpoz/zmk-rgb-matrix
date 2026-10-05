/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * On-flash record codec for RGB matrix settings.
 */

#include <string.h>

#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <zmk/rgb_matrix.h>
#include <zmk/rgb_persist.h>

#define KP_RGB_PERSIST_EFFECT_PATH_LEN                                         \
  (sizeof("keypaw/rgb_matrix/state/effects/") - 1)

BUILD_ASSERT(sizeof(struct kp_rgb_persist_blob) <= SETTINGS_MAX_VAL_LEN,
             "the persisted global record exceeds SETTINGS_MAX_VAL_LEN");
BUILD_ASSERT(sizeof(struct kp_rgb_persist_effect) <= SETTINGS_MAX_VAL_LEN,
             "the persisted effect record exceeds SETTINGS_MAX_VAL_LEN");
BUILD_ASSERT(KP_RGB_PERSIST_EFFECT_PATH_LEN + KP_RGB_PERSIST_MAX_ID_LENGTH <=
                 SETTINGS_MAX_NAME_LEN,
             "the persisted effect settings key exceeds SETTINGS_MAX_NAME_LEN");

bool kp_rgb_persist_size_ok(size_t len) {
  return len == sizeof(struct kp_rgb_persist_blob);
}

bool kp_rgb_persist_version_ok(uint8_t version) {
  return version == KP_RGB_PERSIST_VERSION;
}

bool kp_rgb_persist_effect_valid(const struct kp_rgb_persist_effect *effect) {
  return effect != NULL && effect->h <= KP_RGB_HUE_MAX &&
         effect->s <= KP_RGB_SAT_MAX;
}

uint16_t kp_rgb_persist_clamp_duration(uint16_t duration_ms, uint16_t min_ms,
                                       uint16_t max_ms) {
  return (uint16_t)CLAMP((int32_t)duration_ms, (int32_t)min_ms,
                         (int32_t)max_ms);
}

void kp_rgb_persist_pack(bool user_on, uint8_t brightness, const char *selected_id,
                         struct kp_rgb_persist_blob *out) {
  memset(out, 0, sizeof(*out));
  out->version = KP_RGB_PERSIST_VERSION;
  out->user_on = user_on;
  out->brightness = MIN(brightness, (uint8_t)KP_RGB_BRT_MAX);
  if (selected_id != NULL && selected_id[0] != '\0') {
    out->has_selected = 1;
    strncpy(out->selected_id, selected_id, sizeof(out->selected_id) - 1);
  }
}
