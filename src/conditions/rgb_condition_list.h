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

#define KP_RGB_CONDITION_LIST_ASSERT_NON_EMPTY(node_id)                        \
  BUILD_ASSERT(DT_PROP_LEN_OR(node_id, conditions, 0) > 0,                     \
               "`conditions` must list at least one condition")

#define KP_RGB_CONDITION_LIST_DEFINE(inst, api)                                \
  KP_RGB_CONDITION_LIST_ASSERT_NON_EMPTY(DT_DRV_INST(inst));                   \
  KP_RGB_CONDITION_DEFINE(                                                     \
      inst, &api, &KP_RGB_CONDITION_REGISTRATION(DT_DRV_INST(inst)), NULL)
