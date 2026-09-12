/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Built-in "always" trigger: unconditionally active. Use it as the final entry
 * of the &kprgb `triggers` list to express the "else" case, so something is
 * asserting when no conditional trigger matched.
 */

#define DT_DRV_COMPAT keypaw_rgb_trigger_always

#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct kp_trig_always_config {
  struct kp_rgb_trigger_common_config common;
};

static bool kp_trig_always_active(const struct device *dev) {
  ARG_UNUSED(dev);
  return true;
}

#define KP_TRIG_ALWAYS_DEFINE(inst)                                              \
  KP_RGB_TRIGGER_BINDING_ARRAY(inst, kp_trig_always_##inst);                     \
  static const struct kp_trig_always_config kp_trig_always_##inst##_cfg = {      \
      .common = KP_RGB_TRIGGER_COMMON(DT_DRV_INST(inst), kp_trig_always_##inst), \
  };                                                                             \
  KP_RGB_TRIGGER_DEFINE(inst, kp_trig_always_active, kp_trig_always_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_TRIG_ALWAYS_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
