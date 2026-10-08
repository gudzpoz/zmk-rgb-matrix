/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/sys/util.h>
#include <zmk/rgb_matrix.h>

#include "rgb_selector_internal.h"

size_t kp_rgb_selector_count(const struct kp_rgb_selector *selector) {
  return selector->effect_count;
}

const struct device *kp_rgb_selector_at(const struct kp_rgb_selector *selector, size_t slot) {
  return slot < selector->effect_count ? selector->effects[slot] : NULL;
}

uint16_t kp_rgb_selector_calc_index(const struct kp_rgb_selector *selector,
                                    uint16_t current, int16_t delta) {
  size_t count = kp_rgb_selector_count(selector);
  if (count == 0) {
    return current;
  }
  int64_t norm_delta = delta % (int64_t)count;
  size_t start = ((int64_t)current + norm_delta + (int64_t)count) % (int64_t)count;
  if (kp_rgb_selector_at(selector, start) != NULL) {
    return (uint16_t)start;
  }
  int direction = delta < 0 ? -1 : 1;
  for (size_t i = 1; i < count; i++) {
    size_t slot = ((int64_t)start + (int64_t)i * direction + (int64_t)count) %
                  (int64_t)count;
    if (kp_rgb_selector_at(selector, slot) != NULL) {
      return (uint16_t)slot;
    }
  }
  return current;
}

int kp_rgb_selector_resolve_locked(struct kp_rgb_selector *selector) {
  if (kp_rgb_selector_count(selector) == 0) {
    return -ENOENT;
  }
  if (selector->effect_index >= kp_rgb_selector_count(selector)) {
    selector->effect_index = 0;
  }
  selector->effect_index =
      kp_rgb_selector_calc_index(selector, selector->effect_index, 0);
  selector->active_fx = kp_rgb_selector_at(selector, selector->effect_index);
  return selector->active_fx == NULL ? -ENOENT : 0;
}

int kp_rgb_selector_apply_defaults_locked(struct kp_rgb_selector *selector) {
  uint16_t duration = (uint16_t)CLAMP(selector->initial_duration_ms,
                                     CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS,
                                     CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
  for (size_t i = 0; i < kp_rgb_selector_count(selector); i++) {
    const struct device *fx = kp_rgb_selector_at(selector, i);
    if (fx == NULL) {
      continue;
    }
    const struct kp_rgb_effect_defaults *def = &selector->effect_defaults[i];
    struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(fx);
    data->color = def->color;
    data->color.b = KP_RGB_BRT_MAX;
    data->duration_ms =
        def->duration_ms != 0
            ? (uint16_t)CLAMP(def->duration_ms,
                              CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS,
                              CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS)
            : duration;
  }
  selector->effect_index = selector->initial_effect;
  return kp_rgb_selector_resolve_locked(selector);
}

int kp_rgb_selector_select_locked(struct kp_rgb_selector *selector, uint16_t slot) {
  if (slot >= kp_rgb_selector_count(selector)) {
    return -EINVAL;
  }
  const struct device *fx = kp_rgb_selector_at(selector, slot);
  if (fx == NULL) {
    return -ENOENT;
  }
  if (selector->effect_index == slot && selector->active_fx == fx) {
    return 0;
  }
  selector->effect_index = slot;
  selector->active_fx = fx;
  return 1;
}

int kp_rgb_selector_member_slot(const struct kp_rgb_selector *selector,
                                const struct device *effect) {
  if (effect == NULL || selector->owner == NULL) {
    return -ENOENT;
  }
  const struct kp_rgb_effect_common_config *cfg = kp_rgb_effect_cfg(effect);
  if (cfg == NULL || cfg->owner != selector->owner ||
      kp_rgb_selector_at(selector, cfg->slot) != effect) {
    return -ENOENT;
  }
  return cfg->slot;
}
