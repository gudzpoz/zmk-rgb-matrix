/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT keypaw_rgb_condition_or

#include "rgb_condition_list.h"
#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static bool sample(const struct device *dev) {
  const struct kp_rgb_condition_registration *cfg = dev->config;
  for (size_t i = 0; i < cfg->dep_count; ++i) {
    if (kp_rgb_condition_value(cfg->deps[i]->dev)) {
      return true;
    }
  }
  return false;
}

static const struct kp_rgb_condition_api api = {
    .scope = KP_RGB_CONDITION_ANY_SIDE, .sample = sample};
#define DEFINE(inst) KP_RGB_CONDITION_LIST_DEFINE(inst, api)
DT_INST_FOREACH_STATUS_OKAY(DEFINE)

#endif
