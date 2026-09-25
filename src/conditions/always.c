/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Built-in "always" condition: unconditionally active. Declare it as the final
 * rule of a trigger table to express the "else" case.
 */

#define DT_DRV_COMPAT keypaw_rgb_condition_always

#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static bool kp_cond_always_active(const struct device *dev) {
  ARG_UNUSED(dev);
  return true;
}

#define KP_COND_ALWAYS_DEFINE(inst)                                            \
  KP_RGB_CONDITION_DEFINE(inst, kp_cond_always_active, NULL)

DT_INST_FOREACH_STATUS_OKAY(KP_COND_ALWAYS_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
