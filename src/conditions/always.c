/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT keypaw_rgb_condition_always

#include <zephyr/sys/util.h>
#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static bool sample(const struct device *dev, int64_t now_ms,
                   int64_t *next_wakeup_ms) {
  ARG_UNUSED(dev);
  ARG_UNUSED(now_ms);
  ARG_UNUSED(next_wakeup_ms);
  return true;
}

static const struct kp_rgb_condition_api api = {
    .scope = KP_RGB_CONDITION_ANY_SIDE,
    .sample = sample,
};

#define DEFINE(inst) KP_RGB_CONDITION_DEFINE(inst, &api, NULL, NULL)
DT_INST_FOREACH_STATUS_OKAY(DEFINE)

#endif
