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
BUILD_ASSERT(sizeof(struct kp_rgb_persist_selector_record) <=
                 SETTINGS_MAX_VAL_LEN,
             "selector settings record exceeds SETTINGS_MAX_VAL_LEN");
BUILD_ASSERT(sizeof(KP_RGB_PERSIST_SELECTOR_PREFIX) - 1 +
                     KP_RGB_PERSIST_MAX_SELECTOR_ID_LENGTH - 1 +
                     sizeof("/state") - 1 <=
                 SETTINGS_MAX_NAME_LEN,
             "selector settings key exceeds SETTINGS_MAX_NAME_LEN");

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
  if (out == NULL || selected_id == NULL)
    return -EINVAL;
  size_t length = 0;
  while (length < KP_RGB_PERSIST_MAX_ID_LENGTH && selected_id[length] != '\0')
    length++;
  if (length == 0 || length == KP_RGB_PERSIST_MAX_ID_LENGTH)
    return -EINVAL;
  struct kp_rgb_persist_selector_record record = {
      .version = KP_RGB_PERSIST_SELECTOR_VERSION};
  memcpy(record.selected_id, selected_id, length + 1);
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
      record.selected_id[0] == '\0' ||
      memchr(record.selected_id, '\0', sizeof(record.selected_id)) == NULL)
    return -EINVAL;
  *out = record;
  return 0;
}
