/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT keypaw_rgb_condition_latch

#include "rgb_latch.h"

#include <zephyr/sys/atomic.h>
#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static bool sample(const struct device *dev, int64_t now_ms,
                   int64_t *next_wakeup_ms) {
  const struct kp_rgb_latch_data *data = dev->data;
  ARG_UNUSED(now_ms);
  ARG_UNUSED(next_wakeup_ms);
  return data && atomic_get(&data->active);
}

#define DEFINE(inst)                                                           \
  static struct kp_rgb_latch_data latch_data_##inst;                           \
  static const struct kp_rgb_condition_api latch_api_##inst = {                \
      .scope = KP_RGB_CONDITION_ANY_SIDE, .sample = sample};                   \
  KP_RGB_CONDITION_DEFINE(inst, &latch_api_##inst, NULL, &latch_data_##inst)
DT_INST_FOREACH_STATUS_OKAY(DEFINE)

#endif
