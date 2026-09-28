/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#include <zmk/rgb_matrix.h>

/* Shared plumbing for the and/or combinators. */

struct kp_rgb_condition_list_config {
  const struct device *const *conditions;
  size_t count;
};

#define KP_RGB_CONDITION_LIST_ASSERT_NON_EMPTY(node_id)                        \
  BUILD_ASSERT(DT_PROP_LEN_OR(node_id, conditions, 0) > 0,                     \
               "`conditions` must list at least one condition")
#define KP_RGB_CONDITION_AT_IDX(idx, node_id)                                  \
  DEVICE_DT_GET(DT_PROP_BY_IDX(node_id, conditions, idx))
#define KP_RGB_CONDITION_LIST_DEFINE(inst, active_fn)                          \
  KP_RGB_CONDITION_LIST_ASSERT_NON_EMPTY(DT_DRV_INST(inst));                   \
  static const struct device *const kp_rgb_cond_##inst##_conditions[] = {      \
      LISTIFY(DT_PROP_LEN(DT_DRV_INST(inst), conditions),                      \
              KP_RGB_CONDITION_AT_IDX, (, ), DT_DRV_INST(inst))};              \
  static const struct kp_rgb_condition_list_config kp_rgb_cond_##inst##_cfg =  \
      {                                                                        \
          .conditions = kp_rgb_cond_##inst##_conditions,                       \
          .count = DT_PROP_LEN(DT_DRV_INST(inst), conditions),                 \
  };                                                                           \
  KP_RGB_CONDITION_DEFINE(inst, active_fn, &kp_rgb_cond_##inst##_cfg, NULL)
