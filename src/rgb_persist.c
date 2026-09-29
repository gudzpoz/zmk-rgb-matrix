/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * On-flash record codec for the RGB matrix settings. Split out of
 * rgb_settings.c so a host build can unit-test it (tests/unit) without linking
 * the ZMK app.
 */

#include <string.h>

#include <zephyr/sys/util.h>

#include <zmk/rgb_persist.h>

BUILD_ASSERT(sizeof(struct kp_rgb_persist_effect) == 6,
             "the persisted effect gained padding");
BUILD_ASSERT(sizeof(struct kp_rgb_persist_blob) ==
                 6 + KP_RGB_PERSIST_MAX_EFFECTS *
                         sizeof(struct kp_rgb_persist_effect),
             "the persisted blob gained padding");

bool kp_rgb_persist_size_ok(size_t len) {
  return len == sizeof(struct kp_rgb_persist_blob);
}

bool kp_rgb_persist_version_ok(uint8_t version) {
  return version == KP_RGB_PERSIST_VERSION;
}

bool kp_rgb_persist_effect_valid(const struct kp_rgb_persist_effect *effect) {
  return effect->h <= KP_RGB_HUE_MAX && effect->s <= KP_RGB_SAT_MAX &&
         effect->b <= KP_RGB_BRT_MAX;
}

uint16_t kp_rgb_persist_clamp_duration(uint16_t duration_ms, uint16_t min_ms,
                                       uint16_t max_ms) {
  return (uint16_t)CLAMP((int32_t)duration_ms, (int32_t)min_ms,
                         (int32_t)max_ms);
}

void kp_rgb_persist_pack(uint16_t selected_index, bool user_on,
                         const struct kp_rgb_persist_effect *effects,
                         size_t count, struct kp_rgb_persist_blob *out) {
  memset(out, 0, sizeof(*out));
  out->version = KP_RGB_PERSIST_VERSION;
  out->selected_index = selected_index;
  out->user_on = user_on;
  count = MIN(count, (size_t)KP_RGB_PERSIST_MAX_EFFECTS);
  out->effect_count = (uint8_t)count;
  for (size_t i = 0; i < count; i++) {
    out->effects[i] = effects[i];
  }
}
