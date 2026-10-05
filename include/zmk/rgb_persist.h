#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/settings/settings.h>

#include <zmk/rgb_color.h>

/* One behavior's persisted RGB state. The format is intentionally separate from
 * devicetree declaration order; bump the version when this contract changes. */
#define KP_RGB_PERSIST_VERSION 2
#define KP_RGB_PERSIST_MAX_ID_LENGTH 16

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

bool kp_rgb_persist_size_ok(size_t len);
bool kp_rgb_persist_version_ok(uint8_t version);
bool kp_rgb_persist_effect_valid(const struct kp_rgb_persist_effect *effect);
uint16_t kp_rgb_persist_clamp_duration(uint16_t duration_ms, uint16_t min_ms,
                                       uint16_t max_ms);
void kp_rgb_persist_pack(bool user_on, uint8_t brightness, const char *selected_id,
                         struct kp_rgb_persist_blob *out);
