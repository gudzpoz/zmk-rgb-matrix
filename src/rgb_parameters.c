/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <drivers/behavior.h>
#include <zmk/rgb_matrix.h>

#include "rgb_matrix_internal.h"

#if DT_HAS_COMPAT_STATUS_OKAY(keypaw_behavior_rgb_matrix)
#define KP_GLOBAL_CONTROL DT_INST(0, keypaw_behavior_rgb_matrix)
#if DT_NODE_HAS_PROP(KP_GLOBAL_CONTROL, selector)
BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_PHANDLE(KP_GLOBAL_CONTROL, selector),
                                keypaw_rgb_overlay_selector),
             "RGB matrix selector alias must reference a selector overlay");
BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_PHANDLE(KP_GLOBAL_CONTROL, selector), okay),
             "RGB matrix selector alias must reference an enabled selector");
#endif
#endif

const struct device *kp_rgb_selector_alias(void) {
#if DT_HAS_COMPAT_STATUS_OKAY(keypaw_behavior_rgb_matrix)
  return COND_CODE_1(DT_NODE_HAS_PROP(KP_GLOBAL_CONTROL, selector),
                     (DEVICE_DT_GET(DT_PHANDLE(KP_GLOBAL_CONTROL, selector))),
                     (NULL));
#else
  return NULL;
#endif
}

int kp_rgb_apply_defaults(void) {
  kp_rgb_matrix_lock();
  kp_rgb_global.user_on = kp_rgb_global.initial_on;
  kp_rgb_global.brightness =
      (uint8_t)CLAMP(kp_rgb_global.initial_brightness, 0, KP_RGB_BRT_MAX);
  kp_rgb_reconcile_power_locked();
  zmk_rgb_matrix_flush();
  kp_rgb_matrix_unlock();
  return 0;
}

int zmk_rgb_matrix_select_effect(uint16_t index) {
  if (k_is_in_isr())
    return -EWOULDBLOCK;
  kp_rgb_matrix_lock();
  struct kp_rgb_selector *selector =
      kp_rgb_selector_overlay_state_locked(kp_rgb_selector_alias());
  int ret = kp_rgb_selector_dispatch_locked(selector, RGB_EFS_CMD, index);
  kp_rgb_matrix_unlock();
  return ret;
}

int zmk_rgb_matrix_cycle_effect(int16_t direction) {
  if (k_is_in_isr())
    return -EWOULDBLOCK;
  kp_rgb_matrix_lock();
  struct kp_rgb_selector *selector =
      kp_rgb_selector_overlay_state_locked(kp_rgb_selector_alias());
  int ret = selector == NULL
                ? -ENODEV
                : kp_rgb_selector_dispatch_locked(
                      selector, RGB_EFS_CMD,
                      kp_rgb_selector_calc_index(
                          selector, selector->effect_index, direction));
  kp_rgb_matrix_unlock();
  return ret;
}

static bool kp_rgb_parameter_target_ready(const struct device *fx) {
  return fx != NULL && device_is_ready(fx);
}

int kp_rgb_set_color_locked(const struct device *fx,
                           const struct kp_rgb_hsb *color) {
  if (!kp_rgb_parameter_target_ready(fx)) {
    return -ENODEV;
  }
  if (color == NULL || color->h > KP_RGB_HUE_MAX || color->s > KP_RGB_SAT_MAX ||
      color->b > KP_RGB_BRT_MAX) {
    return -EINVAL;
  }
  struct kp_rgb_hsb next = *color;
  next.h %= KP_RGB_HUE_MAX;
  struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(fx);
  if (data->color.h != next.h || data->color.s != next.s ||
      data->color.b != KP_RGB_BRT_MAX) {
    data->color.h = next.h;
    data->color.s = next.s;
    data->color.b = KP_RGB_BRT_MAX;
    zmk_rgb_matrix_flush();
  }
  return 0;
}

int kp_rgb_set_period_locked(const struct device *fx, uint32_t period_ms) {
  if (!kp_rgb_parameter_target_ready(fx)) {
    return -ENODEV;
  }
  if (period_ms < CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS ||
      period_ms > CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS) {
    return -EINVAL;
  }
  struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(fx);
  if (data->duration_ms != period_ms) {
    data->duration_ms = (uint16_t)period_ms;
    zmk_rgb_matrix_flush();
  }
  return 0;
}

int zmk_rgb_matrix_set_color(const struct device *fx,
                            const struct kp_rgb_hsb *color) {
  if (k_is_in_isr()) {
    return -EWOULDBLOCK;
  }
  kp_rgb_matrix_lock();
  int ret = kp_rgb_set_color_locked(fx, color);
  kp_rgb_matrix_unlock();
  return ret;
}

int zmk_rgb_matrix_set_period(const struct device *fx, uint32_t period_ms) {
  if (k_is_in_isr()) {
    return -EWOULDBLOCK;
  }
  kp_rgb_matrix_lock();
  int ret = kp_rgb_set_period_locked(fx, period_ms);
  kp_rgb_matrix_unlock();
  return ret;
}

int kp_rgb_effect_convert_central_state_dependent_params(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event) {
  ARG_UNUSED(event);
  if (k_is_in_isr())
    return -EWOULDBLOCK;
  const struct device *fx = zmk_behavior_get_binding(binding->behavior_dev);
  if (!kp_rgb_parameter_target_ready(fx))
    return -ENODEV;
  kp_rgb_matrix_lock();
  const struct kp_rgb_effect_common_config *cfg = kp_rgb_effect_cfg(fx);
  struct kp_rgb_selector *selector =
      cfg == NULL ? NULL : kp_rgb_selector_overlay_state_locked(cfg->owner);
  const struct device *control =
      cfg == NULL ? NULL : kp_rgb_selector_control(cfg->owner);
  int slot = selector == NULL ? -ENOENT : kp_rgb_selector_member_slot(selector, fx);
  int err = slot < 0 || control == NULL || !device_is_ready(control) ? -ENODEV : 0;
  if (err == 0) {
    binding->behavior_dev = control->name;
    binding->param1 = RGB_EFS_CMD;
    binding->param2 = (uint32_t)slot;
  }
  kp_rgb_matrix_unlock();
  return err;
}
