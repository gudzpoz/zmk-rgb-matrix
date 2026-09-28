/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * The toggle half of a keypaw,rgb-condition-latch, declared as its child so it
 * can reach the condition with DT_PARENT (a phandle from the parent would be a
 * devicetree cycle).
 *
 * The binding carries no parameters; "flip" is resolved to an absolute value on
 * the central and forwarded, so every half agrees. The state is volatile.
 */

#define DT_DRV_COMPAT keypaw_behavior_rgb_overlay_toggle

#include <stddef.h>

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/rgb_matrix.h>

#include "../conditions/rgb_latch.h"

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/* Internal wire opcode, replacing the binding's 0/0 after central conversion.
 * Deliberately absent from behavior_parameter_metadata: advertising a value
 * would make the metadata check reject the zero-parameter binding. */
#define KP_RGB_OVL_LATCH_SET 1u

struct kp_ovl_toggle_config {
  const struct device *condition; /* the parent latch */
};

/* The convert hook and the pressed handler get no `dev`, only the binding, so
 * resolve the device from its name. */
static const struct kp_ovl_toggle_config *
kp_ovl_toggle_cfg(const struct zmk_behavior_binding *binding) {
  const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
  return dev == NULL ? NULL : (const struct kp_ovl_toggle_config *)dev->config;
}

static struct kp_rgb_latch_data *
kp_ovl_toggle_latch(const struct kp_ovl_toggle_config *cfg) {
  return cfg == NULL ? NULL : (struct kp_rgb_latch_data *)cfg->condition->data;
}

/* Central only (zmk_behavior_invoke_binding runs it once, before the GLOBAL
 * switch). It must compute the target, never flip: the same converted binding
 * is then applied locally, so a side effect here would double-toggle. */
static int kp_ovl_toggle_convert(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event) {
  ARG_UNUSED(event);
  const struct kp_ovl_toggle_config *cfg = kp_ovl_toggle_cfg(binding);
  const struct kp_rgb_latch_data *lat = kp_ovl_toggle_latch(cfg);
  if (lat == NULL) {
    return -ENODEV;
  }
  binding->param1 = KP_RGB_OVL_LATCH_SET;
  binding->param2 = lat->active ? 0u : 1u;
  return 0;
}

/* Runs on every executing half: locally with the converted binding, and on each
 * peripheral from the forwarded payload. Absolute, so the halves cannot end up
 * inverted. */
static int kp_ovl_toggle_pressed(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event) {
  ARG_UNUSED(event);
  if (binding->param1 != KP_RGB_OVL_LATCH_SET) {
    return -EINVAL;
  }
  struct kp_rgb_latch_data *lat = kp_ovl_toggle_latch(kp_ovl_toggle_cfg(binding));
  if (lat == NULL) {
    return -ENODEV;
  }
  lat->active = binding->param2 != 0;

  /* Repaint now instead of waiting up to a tick, so a toggle feels instant. */
  zmk_rgb_matrix_flush();
  return ZMK_BEHAVIOR_OPAQUE;
}

static int kp_ovl_toggle_released(struct zmk_behavior_binding *binding,
                                  struct zmk_behavior_binding_event event) {
  ARG_UNUSED(binding);
  ARG_UNUSED(event);
  return ZMK_BEHAVIOR_OPAQUE;
}

#define KP_OVL_TOGGLE_DEFINE(inst)                                             \
  BUILD_ASSERT(sizeof(DEVICE_DT_NAME(DT_DRV_INST(inst))) <= 9,                 \
               "keypaw,behavior-rgb-overlay-toggle: node name must fit the "   \
               "9-byte split behavior_dev field (<= 8 characters)");           \
  BUILD_ASSERT(                                                                \
      DT_NODE_HAS_COMPAT(DT_PARENT(DT_DRV_INST(inst)),                         \
                         keypaw_rgb_condition_latch),                          \
      "keypaw,behavior-rgb-overlay-toggle must be a child of a "               \
      "keypaw,rgb-condition-latch");                                           \
  static const struct kp_ovl_toggle_config kp_ovl_toggle_##inst##_cfg = {      \
      .condition = DEVICE_DT_GET(DT_PARENT(DT_DRV_INST(inst))),                \
  };                                                                           \
  IF_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA,                                     \
             (static const struct behavior_parameter_metadata                  \
                  kp_ovl_toggle_##inst##_metadata = {.sets_len = 0};))         \
  static const struct behavior_driver_api kp_ovl_toggle_##inst##_api = {       \
      .locality = BEHAVIOR_LOCALITY_GLOBAL,                                    \
      .binding_convert_central_state_dependent_params =                        \
          kp_ovl_toggle_convert,                                               \
      .binding_pressed = kp_ovl_toggle_pressed,                                \
      .binding_released = kp_ovl_toggle_released,                              \
      IF_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA,                                 \
                 (.parameter_metadata = &kp_ovl_toggle_##inst##_metadata,))};  \
  BEHAVIOR_DT_DEFINE(DT_DRV_INST(inst), NULL, NULL, NULL,                      \
                     &kp_ovl_toggle_##inst##_cfg, POST_KERNEL,                 \
                     CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                      \
                     &kp_ovl_toggle_##inst##_api)

DT_INST_FOREACH_STATUS_OKAY(KP_OVL_TOGGLE_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
