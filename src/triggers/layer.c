/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Built-in "layer" trigger: matches while a given keymap layer is active.
 *
 * The condition is sampled, never cached: the evaluator re-runs on every
 * zmk_layer_state_changed event, so there is nothing to keep in sync. Layer
 * state only exists where the keymap does, which is why the whole trigger
 * subsystem is central-only (see rgb_triggers.c).
 */

#define DT_DRV_COMPAT keypaw_rgb_trigger_layer

#include <stdbool.h>

#include <zephyr/device.h>

#include <zmk/keymap.h>
#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct kp_trig_layer_config {
  struct kp_rgb_trigger_common_config common;
  uint16_t layer;
};

static bool kp_trig_layer_active(const struct device *dev) {
  const struct kp_trig_layer_config *cfg = dev->config;
  return zmk_keymap_layer_active((zmk_keymap_layer_id_t)cfg->layer);
}

#define KP_TRIG_LAYER_DEFINE(inst)                                              \
  KP_RGB_TRIGGER_BINDING_ARRAY(inst, kp_trig_layer_##inst);                     \
  static const struct kp_trig_layer_config kp_trig_layer_##inst##_cfg = {       \
      .common = KP_RGB_TRIGGER_COMMON(DT_DRV_INST(inst), kp_trig_layer_##inst), \
      .layer = DT_PROP(DT_DRV_INST(inst), layer),                               \
  };                                                                            \
  KP_RGB_TRIGGER_DEFINE(inst, kp_trig_layer_active, kp_trig_layer_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_TRIG_LAYER_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
