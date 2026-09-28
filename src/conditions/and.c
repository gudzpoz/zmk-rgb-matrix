/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Built-in "and" condition: active only while every condition in its
 * `conditions = <&a &b ...>;` list is active. Operands may themselves be
 * combinators; the reference graph must be acyclic.
 */

#define DT_DRV_COMPAT keypaw_rgb_condition_and

#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#include <zmk/rgb_matrix.h>

#include "rgb_condition_list.h"

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static bool kp_cond_and_active(const struct device *dev) {
  const struct kp_rgb_condition_list_config *cfg = dev->config;
  for (size_t i = 0; i < cfg->count; i++) {
    const struct device *cond = cfg->conditions[i];
    const struct kp_rgb_condition_api *api =
        (const struct kp_rgb_condition_api *)cond->api;
    if (!api->active(cond)) {
      return false;
    }
  }
  return true;
}

#define KP_COND_AND_DEFINE(inst)                                               \
  KP_RGB_CONDITION_LIST_DEFINE(inst, kp_cond_and_active)

DT_INST_FOREACH_STATUS_OKAY(KP_COND_AND_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
