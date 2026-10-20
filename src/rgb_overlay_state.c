/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zmk/behavior.h>

#include "rgb_matrix_internal.h"

/* State words are written by the central control worker or received split commands.
 * Local overlay gates are published by the local control worker.
 */
#define KP_OVERLAY_SLOTS MAX(1, KP_RGB_OVERLAY_COUNT)

#define KP_OVERLAY_ENTRY(node_id) DEVICE_DT_GET(node_id),
static const struct device *const kp_overlay_registry[KP_OVERLAY_SLOTS] = {
    DT_FOREACH_CHILD_STATUS_OKAY(KP_RGB_NODE, KP_OVERLAY_ENTRY)};
static uint16_t kp_overlay_state[KP_RGB_OVERLAY_WORDS];

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/* What every peripheral was last told; dispatch() sends only differing words. A
 * mirror rather than a changed-word list means a change also survives a tick
 * that skipped its dispatch: `state != sent` stays true. Central only. */
static uint16_t kp_overlay_sent[KP_RGB_OVERLAY_WORDS];

#endif

const struct device *const *kp_rgb_overlay_list(void) {
  return kp_overlay_registry;
}

size_t kp_rgb_overlay_count(void) { return KP_RGB_OVERLAY_COUNT; }

uint16_t kp_rgb_overlay_word_count(void) { return KP_RGB_OVERLAY_WORDS; }

bool kp_rgb_overlay_set_word(uint16_t word, uint16_t value) {
  kp_rgb_matrix_lock();
  bool changed = word < KP_RGB_OVERLAY_WORDS && kp_overlay_state[word] != value;
  if (changed) {
    kp_overlay_state[word] = value;
    zmk_rgb_matrix_flush();
  }
  kp_rgb_matrix_unlock();
  return changed;
}

uint16_t kp_rgb_overlay_get_word(uint16_t word) {
  kp_rgb_matrix_lock();
  uint16_t value = word < KP_RGB_OVERLAY_WORDS ? kp_overlay_state[word] : 0;
  kp_rgb_matrix_unlock();
  return value;
}

bool kp_rgb_overlay_covers_all(const struct device *dev) {
  const struct kp_rgb_overlay_common_config *cfg = dev->config;
  const struct kp_rgb_overlay_api *api = dev->api;
  return api->replaces_target && cfg->opacity == 100 && KP_LED_COUNT > 0 &&
         kp_rgb_overlay_target_count(dev) == KP_LED_COUNT;
}

bool kp_rgb_overlay_gate(const struct device *dev) {
  const struct kp_rgb_overlay_common_data *data = dev->data;
  const struct kp_rgb_overlay_common_config *cfg = dev->config;
  if (cfg->opacity == 0 || kp_rgb_overlay_target_count(dev) == 0) return false;

  if (data->local) {
    return data->gate;
  }

  /* Central-evaluated: the bit was filled by refresh() on the central or by the
   * RGB_OVL_STATE_CMD handler on a peripheral. */
  return (kp_overlay_state[data->index / 16] >> (data->index % 16)) & 1u;
}

void kp_rgb_overlay_conditions_init(void) {
  for (size_t i = 0; i < KP_RGB_OVERLAY_COUNT; i++) {
    const struct device *dev = kp_overlay_registry[i];
    const struct kp_rgb_overlay_api *api = dev->api;
    const struct kp_rgb_overlay_common_data *data = dev->data;
    if (data->local || !IS_ENABLED(CONFIG_ZMK_SPLIT) ||
        IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) {
      kp_rgb_condition_require(api->condition);
    }
  }
}

bool kp_rgb_overlay_refresh(void) {
  kp_rgb_matrix_lock();
  bool changed = false;
  uint16_t next[KP_RGB_OVERLAY_WORDS] = {0};

  for (size_t i = 0; i < KP_RGB_OVERLAY_COUNT; i++) {
    const struct device *dev = kp_overlay_registry[i];
    const struct kp_rgb_overlay_api *api = dev->api;
    struct kp_rgb_overlay_common_data *data = dev->data;
    if (!data->local && IS_ENABLED(CONFIG_ZMK_SPLIT) &&
        !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) continue;

    bool active = kp_rgb_condition_valid(api->condition) &&
                  (!api->condition || kp_rgb_condition_value(api->condition));
    changed |= data->gate != active;
    data->gate = active;
    if (!data->local && active) {
      next[data->index / 16] |= (uint16_t)BIT(data->index % 16);
    }
  }

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  for (uint16_t w = 0; w < KP_RGB_OVERLAY_WORDS; w++) {
    changed |= kp_overlay_state[w] != next[w];
    kp_overlay_state[w] = next[w];
  }
#else
  ARG_UNUSED(next);
#endif

  kp_rgb_matrix_unlock();
  return changed;
}

void kp_rgb_overlay_dispatch(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  struct zmk_behavior_binding_event event = {.timestamp = k_uptime_get()};
  for (uint16_t w = 0; w < KP_RGB_OVERLAY_WORDS; w++) {
    uint16_t bits = kp_rgb_overlay_get_word(w);
    if (bits == kp_overlay_sent[w]) {
      continue;
    }

    struct zmk_behavior_binding binding = {
        .behavior_dev = DEVICE_DT_NAME(KP_RGB_NODE),
        .param1 = RGB_OVL_STATE_CMD,
        .param2 = RGB_OVL_STATE_VAL(w, bits),
    };
    zmk_behavior_invoke_binding(&binding, event, true);
    /* GLOBAL locality reports no per-peripheral result, so a split drop is
     * invisible here: this records the local attempt, and the connect-time sync
     * is what repairs a peripheral that missed it. */
    kp_overlay_sent[w] = bits;
  }
#endif
}
