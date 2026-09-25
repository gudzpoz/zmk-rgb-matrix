/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * The generic trigger kind: a condition plus the &kprgb bindings to invoke when
 * that condition wins. It carries no mutable state; the evaluator
 * (src/rgb_triggers.c) owns the table, the precedence order, and the
 * winner-change edge. Sharing the condition with overlays is the point -- one
 * keypaw,rgb-condition-* node serves a view and a state rule.
 */

#define DT_DRV_COMPAT keypaw_rgb_trigger

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct kp_trig_config {
  struct kp_rgb_trigger_common_config common;
  const struct device *condition; /* NULL = always active */
};

static bool kp_trig_active(const struct device *dev) {
  const struct kp_trig_config *cfg = dev->config;

  if (cfg->condition == NULL) {
    return true;
  }
  const struct kp_rgb_condition_api *cond =
      (const struct kp_rgb_condition_api *)cfg->condition->api;
  return cond->active(cfg->condition);
}

#define KP_TRIG_DEFINE(inst)                                                   \
  KP_RGB_TRIGGER_BINDING_ARRAY(inst, kp_trig_##inst);                          \
  static const struct kp_trig_config kp_trig_##inst##_cfg = {                  \
      .common = KP_RGB_TRIGGER_COMMON(DT_DRV_INST(inst), kp_trig_##inst),      \
      .condition = KP_RGB_CONDITION_PTR(DT_DRV_INST(inst)),                    \
  };                                                                           \
  KP_RGB_TRIGGER_DEFINE(inst, kp_trig_active, kp_trig_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_TRIG_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
