/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Built-in "latch" condition: a manually toggled bool, the one stateful kind.
 * The state lives in dev->data and is flipped by a keypaw,behavior-rgb-overlay-
 * toggle nested under the same devicetree node.
 */

#define DT_DRV_COMPAT keypaw_rgb_condition_latch

#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#include <zmk/rgb_matrix.h>

#include "rgb_latch.h"

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static bool kp_cond_latch_active(const struct device *dev) {
  const struct kp_rgb_latch_data *data = dev->data;
  return data != NULL && data->active;
}

/* Static data zero-initialises `active` to false, so the latch starts off and
 * needs no init function. */
#define KP_COND_LATCH_DEFINE(inst)                                             \
  static struct kp_rgb_latch_data kp_cond_latch_##inst##_data;                 \
  KP_RGB_CONDITION_DEFINE(inst, kp_cond_latch_active, NULL,                    \
                          &kp_cond_latch_##inst##_data)

DT_INST_FOREACH_STATUS_OKAY(KP_COND_LATCH_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
