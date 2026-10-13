/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * On-flash record codec for RGB matrix settings.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <zmk/rgb_color.h>
#include <zmk/rgb_persist.h>

BUILD_ASSERT(sizeof(struct kp_rgb_persist_global_record) == 4,
             "global settings wire size changed");
BUILD_ASSERT(sizeof(struct kp_rgb_persist_selector_record) == 33,
             "selector settings wire size changed");
BUILD_ASSERT(sizeof(struct kp_rgb_persist_parameters_record) == 6,
             "parameter settings wire size changed");
BUILD_ASSERT(sizeof(struct kp_rgb_persist_selector_record) <=
                 SETTINGS_MAX_VAL_LEN,
             "selector settings record exceeds SETTINGS_MAX_VAL_LEN");
BUILD_ASSERT(sizeof(KP_RGB_PERSIST_SELECTOR_PREFIX) - 1 +
                     KP_RGB_PERSIST_MAX_SELECTOR_ID_LENGTH - 1 +
                     sizeof("/state") - 1 <=
                 SETTINGS_MAX_NAME_LEN,
             "selector settings key exceeds SETTINGS_MAX_NAME_LEN");


bool kp_rgb_persist_effect_valid(const struct kp_rgb_persist_effect *effect) {
  return effect != NULL && effect->h <= KP_RGB_HUE_MAX &&
         effect->s <= KP_RGB_SAT_MAX;
}

uint16_t kp_rgb_persist_clamp_duration(uint16_t duration_ms, uint16_t min_ms,
                                       uint16_t max_ms) {
  return (uint16_t)CLAMP((int32_t)duration_ms, (int32_t)min_ms,
                         (int32_t)max_ms);
}


bool kp_rgb_persist_id_valid(const char *id) {
  if (id == NULL || id[0] == '\0')
    return false;
  for (size_t i = 0; i < KP_RGB_PERSIST_MAX_ID_LENGTH; i++) {
    unsigned char c = id[i];
    if (c == '\0')
      return true;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
          c == '@'))
      return false;
  }
  return false;
}

int kp_rgb_persist_validate_ids(
    const struct kp_rgb_persist_selector_ids *selectors, size_t count) {
  if (count != 0 && selectors == NULL)
    return -EINVAL;
  for (size_t i = 0; i < count; i++) {
    const struct kp_rgb_persist_selector_ids *selector = &selectors[i];
    if (!kp_rgb_persist_id_valid(selector->id) ||
        (selector->effect_count != 0 && selector->effects == NULL))
      return -EINVAL;
    if (strlen(selector->id) >= KP_RGB_PERSIST_MAX_SELECTOR_ID_LENGTH)
      return -ENAMETOOLONG;
    for (size_t j = 0; j < i; j++) {
      if (strcmp(selector->id, selectors[j].id) == 0)
        return -EEXIST;
    }
    for (size_t j = 0; j < selector->effect_count; j++) {
      const struct kp_rgb_persist_effect_id *effect = &selector->effects[j];
      if (effect->id == NULL)
        continue;
      if (!kp_rgb_persist_id_valid(effect->id))
        return -EINVAL;
      if (effect->persist_parameters &&
          strlen(selector->id) + strlen(effect->id) >
              KP_RGB_PERSIST_EFFECT_ID_BUDGET)
        return -ENAMETOOLONG;
      for (size_t k = 0; k < j; k++) {
        const char *other = selector->effects[k].id;
        if (other != NULL && strcmp(effect->id, other) == 0)
          return -EEXIST;
      }
    }
  }
  return 0;
}


int kp_rgb_persist_encode_global(
    const struct kp_rgb_persist_global_state *state,
    struct kp_rgb_persist_global_record *out) {
  if (state == NULL || out == NULL || state->brightness > KP_RGB_BRT_MAX)
    return -EINVAL;
  *out = (struct kp_rgb_persist_global_record){
      .version = KP_RGB_PERSIST_GLOBAL_VERSION,
      .user_on = state->user_on,
      .brightness = state->brightness,
  };
  return 0;
}

int kp_rgb_persist_decode_global(const void *bytes, size_t len,
                                 struct kp_rgb_persist_global_state *out) {
  if (bytes == NULL || out == NULL ||
      len != sizeof(struct kp_rgb_persist_global_record))
    return -EINVAL;
  struct kp_rgb_persist_global_record record;
  memcpy(&record, bytes, sizeof(record));
  if (record.version != KP_RGB_PERSIST_GLOBAL_VERSION || record.user_on > 1 ||
      record.brightness > KP_RGB_BRT_MAX || record.reserved != 0)
    return -EINVAL;
  *out = (struct kp_rgb_persist_global_state){.user_on = record.user_on != 0,
                                              .brightness = record.brightness};
  return 0;
}

int kp_rgb_persist_encode_selector(const char *selected_id,
                                   struct kp_rgb_persist_selector_record *out) {
  if (out == NULL || !kp_rgb_persist_id_valid(selected_id))
    return -EINVAL;
  struct kp_rgb_persist_selector_record record = {
      .version = KP_RGB_PERSIST_SELECTOR_VERSION};
  memcpy(record.selected_id, selected_id, strlen(selected_id) + 1);
  *out = record;
  return 0;
}

int kp_rgb_persist_decode_selector(const void *bytes, size_t len,
                                   struct kp_rgb_persist_selector_record *out) {
  if (bytes == NULL || out == NULL || len != sizeof(*out))
    return -EINVAL;
  struct kp_rgb_persist_selector_record record;
  memcpy(&record, bytes, sizeof(record));
  if (record.version != KP_RGB_PERSIST_SELECTOR_VERSION ||
      !kp_rgb_persist_id_valid(record.selected_id))
    return -EINVAL;
  *out = record;
  return 0;
}

int kp_rgb_persist_encode_parameters(
    const struct kp_rgb_persist_effect *state,
    struct kp_rgb_persist_parameters_record *out) {
  if (out == NULL || !kp_rgb_persist_effect_valid(state))
    return -EINVAL;
  uint16_t hue = state->h == KP_RGB_HUE_MAX ? 0 : state->h;
  *out = (struct kp_rgb_persist_parameters_record){
      .version = KP_RGB_PERSIST_PARAMETERS_VERSION,
      .duration_le = {state->duration_ms & 0xff, state->duration_ms >> 8},
      .hue_le = {hue & 0xff, hue >> 8},
      .saturation = state->s,
  };
  return 0;
}

int kp_rgb_persist_decode_parameters(const void *bytes, size_t len,
                                     struct kp_rgb_persist_effect *out) {
  if (bytes == NULL || out == NULL ||
      len != sizeof(struct kp_rgb_persist_parameters_record))
    return -EINVAL;
  struct kp_rgb_persist_parameters_record record;
  memcpy(&record, bytes, sizeof(record));
  struct kp_rgb_persist_effect state = {
      .duration_ms =
          record.duration_le[0] | ((uint16_t)record.duration_le[1] << 8),
      .h = record.hue_le[0] | ((uint16_t)record.hue_le[1] << 8),
      .s = record.saturation,
  };
  if (record.version != KP_RGB_PERSIST_PARAMETERS_VERSION ||
      !kp_rgb_persist_effect_valid(&state))
    return -EINVAL;
  if (state.h == KP_RGB_HUE_MAX)
    state.h = 0;
  *out = state;
  return 0;
}
