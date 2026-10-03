/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT keypaw_rgb_condition_or

#include "rgb_condition_list.h"
#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static bool sample(const struct device *dev, int64_t now_ms,
                   int64_t *next_wakeup_ms) {
  const struct kp_rgb_condition_list_config *cfg = dev->config;
  ARG_UNUSED(now_ms);
  ARG_UNUSED(next_wakeup_ms);
  for (size_t i = 0; i < cfg->count; ++i) {
    if (kp_rgb_condition_value(cfg->conditions[i])) {
      return true;
    }
  }
  return false;
}

#define DEFINE(inst) KP_RGB_CONDITION_LIST_DEFINE(inst, sample)
DT_INST_FOREACH_STATUS_OKAY(DEFINE)

#endif
