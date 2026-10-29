/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include <zmk/behavior.h>

#include "rgb_matrix_internal.h"
#include "rgb_replication_internal.h"

/* State is written by central control or received split commands. Local overlay
 * gates are published by the local control worker.
 */
#define KP_OVERLAY_SLOTS MAX(1, KP_RGB_OVERLAY_COUNT)

#define KP_OVERLAY_ENTRY(node_id) DEVICE_DT_GET(node_id),
static const struct device *const kp_overlay_registry[KP_OVERLAY_SLOTS] = {
    DT_FOREACH_CHILD_STATUS_OKAY(KP_RGB_NODE, KP_OVERLAY_ENTRY)};
static uint32_t kp_overlay_state[KP_RGB_OVERLAY_WORDS];

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* Mirror attempted dispatches; a skipped dispatch remains observable. */
static uint32_t kp_overlay_sent[KP_RGB_OVERLAY_WORDS];
#endif

const struct device *const *kp_rgb_overlay_list(void) {
  return kp_overlay_registry;
}

bool kp_rgb_overlay_set_word(size_t index, uint32_t bits) {
  __ASSERT_NO_MSG(index < KP_RGB_OVERLAY_WORDS);
  kp_rgb_matrix_lock();
  bool changed = kp_overlay_state[index] != bits;
  if (changed) {
    kp_overlay_state[index] = bits;
    kp_rgb_replication_changed_locked();
    zmk_rgb_matrix_flush();
  }
  kp_rgb_matrix_unlock();
  return changed;
}

uint32_t kp_rgb_overlay_get_word(size_t index) {
  __ASSERT_NO_MSG(index < KP_RGB_OVERLAY_WORDS);
  kp_rgb_matrix_lock();
  uint32_t bits = kp_overlay_state[index];
  kp_rgb_matrix_unlock();
  return bits;
}

bool kp_rgb_overlay_covers_all(const struct device *dev) {
  const struct kp_rgb_overlay_common_config *cfg = dev->config;
  return cfg->replaces_target && cfg->opacity == 100 && KP_LED_COUNT > 0 &&
         kp_rgb_overlay_target_count(dev) == KP_LED_COUNT;
}

bool kp_rgb_overlay_gate(const struct device *dev) {
  const struct kp_rgb_overlay_common_data *data = dev->data;
  const struct kp_rgb_overlay_common_config *cfg = dev->config;
  if (cfg->opacity == 0 || kp_rgb_overlay_target_count(dev) == 0) return false;

  if (data->local) {
    return data->gate;
  }

  /* Central-evaluated: refreshed centrally or received from the central half. */
  return (kp_overlay_state[data->index / 32] >> (data->index % 32)) & 1u;
}

void kp_rgb_overlay_conditions_init(void) {
  for (size_t i = 0; i < KP_RGB_OVERLAY_COUNT; i++) {
    const struct device *dev = kp_overlay_registry[i];
    const struct kp_rgb_overlay_common_config *cfg = kp_rgb_overlay_cfg(dev);
    const struct kp_rgb_overlay_common_data *data = dev->data;
    if (data->local || !IS_ENABLED(CONFIG_ZMK_SPLIT) ||
        IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) {
      kp_rgb_condition_require(cfg->condition);
    }
  }
}

bool kp_rgb_overlay_refresh(void) {
  kp_rgb_matrix_lock();
  bool changed = false;
  bool synchronized_changed = false;

  for (size_t word = 0; word < KP_RGB_OVERLAY_WORDS; word++) {
    uint32_t next = 0;
    for (size_t i = word * 32; i < MIN((word + 1) * 32, KP_RGB_OVERLAY_COUNT); i++) {
      const struct device *dev = kp_overlay_registry[i];
      const struct kp_rgb_overlay_common_config *cfg = kp_rgb_overlay_cfg(dev);
      struct kp_rgb_overlay_common_data *data = dev->data;
      if (!data->local && IS_ENABLED(CONFIG_ZMK_SPLIT) &&
          !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) continue;

      bool active = kp_rgb_condition_valid(cfg->condition) &&
                    (!cfg->condition || kp_rgb_condition_value(cfg->condition));
      changed |= data->gate != active;
      data->gate = active;
      if (!data->local && active) {
        next |= BIT(i % 32);
      }
    }

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    synchronized_changed |= kp_overlay_state[word] != next;
    kp_overlay_state[word] = next;
#else
    ARG_UNUSED(next);
#endif
  }
  if (synchronized_changed) {
    kp_rgb_replication_changed_locked();
  }
  kp_rgb_matrix_unlock();
  return changed || synchronized_changed;
}

void kp_rgb_overlay_dispatch(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  for (size_t word = 0; word < KP_RGB_OVERLAY_WORDS; word++) {
    uint32_t bits = kp_rgb_overlay_get_word(word);
    if (bits == kp_overlay_sent[word]) continue;
    struct zmk_behavior_binding binding = {
        .behavior_dev = DEVICE_DT_NAME(KP_RGB_NODE),
        .param1 = KP_RGB_OVERLAY_WORD_COMMAND(word),
        .param2 = bits,
    };
    struct zmk_behavior_binding_event event = {.timestamp = k_uptime_get()};
    zmk_behavior_invoke_binding(&binding, event, true);
    /* GLOBAL locality reports no per-peripheral result. Connect-time sync repairs drops. */
    kp_overlay_sent[word] = bits;
  }
#endif
}
