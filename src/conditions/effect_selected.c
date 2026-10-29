/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT keypaw_rgb_condition_effect_selected

#include <zmk/rgb_matrix.h>
#include <zmk/rgb_selector.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct selected_cfg {
  const struct device *selector;
  const struct device *effect;
};

static bool sample(const struct device *dev) {
  const struct selected_cfg *cfg = dev->config;
  return zmk_rgb_selector_get_selected(cfg->selector) == cfg->effect;
}

static const struct kp_rgb_condition_api api = {
    .scope = KP_RGB_CONDITION_ANY_SIDE, .sample = sample};

#define DEFINE(inst)                                                           \
  BUILD_ASSERT(                                                                \
      KP_RGB_IS_CONTROLLED_OVERLAY(DT_INST_PHANDLE(inst, selector)) &&         \
          DT_NODE_HAS_STATUS(DT_INST_PHANDLE(inst, selector), okay) &&         \
          DT_SAME_NODE(DT_PARENT(DT_INST_PHANDLE(inst, selector)),             \
                       KP_RGB_NODE),                                           \
      "selected-effect condition requires an enabled selector overlay");       \
  BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_INST_PHANDLE(inst, effect), okay) &&      \
                   DT_SAME_NODE(DT_PARENT(DT_INST_PHANDLE(inst, effect)),      \
                                DT_INST_PHANDLE(inst, selector)),              \
               "selected-effect condition requires an enabled effect member "  \
               "of its selector");                                             \
  static const struct selected_cfg cfg_##inst = {                              \
      .selector = DEVICE_DT_GET(DT_INST_PHANDLE(inst, selector)),              \
      .effect = DEVICE_DT_GET(DT_INST_PHANDLE(inst, effect)),                  \
  };                                                                           \
  KP_RGB_CONDITION_DEFINE(inst, &api, &cfg_##inst, NULL)

DT_INST_FOREACH_STATUS_OKAY(DEFINE)

#endif
