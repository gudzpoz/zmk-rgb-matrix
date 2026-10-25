/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#define DT_DRV_COMPAT keypaw_rgb_matrix

#include <drivers/behavior.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <dt-bindings/keypaw/rgb_matrix.h>
#include <zmk/keymap.h>
#include <zmk/rgb_matrix.h>

#include "rgb_matrix_internal.h"
#include "rgb_replication_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(sizeof(DEVICE_DT_NAME(KP_RGB_NODE)) <= 9,
             "RGB controller name must fit the split behavior_dev field");

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata no_arg_values[] = {
    {.display_name = "Toggle On/Off",
     .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = RGB_TOG_CMD},
    {.display_name = "Turn On",
     .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = RGB_ON_CMD},
    {.display_name = "Turn Off",
     .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = RGB_OFF_CMD},
    {.display_name = "Brightness Up",
     .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = RGB_BRI_CMD},
    {.display_name = "Brightness Down",
     .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = RGB_BRD_CMD},
    {.display_name = "Reset Settings",
     .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = RGB_RESET_CMD},
};
static const struct behavior_parameter_metadata_set sets[] = {
    {.param1_values = no_arg_values,
     .param1_values_len = ARRAY_SIZE(no_arg_values)},
};
static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(sets),
    .sets = sets,
};
#endif

static uint8_t kp_rgb_calc_brt_locked(int8_t direction) {
  return (uint8_t)CLAMP((int64_t)kp_rgb_global.brightness +
                           (int64_t)direction * CONFIG_KEYPAW_RGB_MATRIX_BRT_STEP,
                       0, KP_RGB_BRT_MAX);
}

static void kp_rgb_change_brt(int8_t direction) {
  kp_rgb_matrix_lock();
  uint8_t brightness = kp_rgb_calc_brt_locked(direction);
  if (kp_rgb_global.brightness != brightness) {
    kp_rgb_global.brightness = brightness;
    kp_rgb_replication_changed_locked();
  }
  zmk_rgb_matrix_flush();
  kp_rgb_matrix_unlock();
}

static int on_keymap_binding_convert_central_state_dependent_params(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event) {
  ARG_UNUSED(event);
  if (binding->param1 & KP_RGB_INVOCATION_MARKER)
    return -ENOTSUP;
  if (k_is_in_isr())
    return -EWOULDBLOCK;
  kp_rgb_matrix_lock();
  int ret = 0;
  uint32_t command = binding->param1;
  if ((command & 0xffffu) == RGB_OVL_STATE_CMD) {
    if (KP_RGB_OVERLAY_COMMAND_WORD(command) >= KP_RGB_OVERLAY_WORDS)
      ret = -EINVAL;
    command = RGB_OVL_STATE_CMD;
  }
  switch (command) {
  case RGB_TOG_CMD:
    binding->param1 = kp_rgb_global.user_on ? RGB_OFF_CMD : RGB_ON_CMD;
    break;
  case RGB_BRI_CMD:
  case RGB_BRD_CMD:
    binding->param2 =
        kp_rgb_calc_brt_locked(binding->param1 == RGB_BRI_CMD ? 1 : -1);
    binding->param1 = RGB_BRT_SET_CMD;
    break;
  case RGB_ON_CMD:
  case RGB_OFF_CMD:
  case RGB_BRT_SET_CMD:
  case RGB_RESET_CMD:
  case RGB_OVL_STATE_CMD:
  case RGB_CLOCK_CMD:
    break;
  default:
    ret = -ENOTSUP;
    break;
  }
  if (ret == 0)
    ret = kp_rgb_invocation_begin(binding);
  kp_rgb_matrix_unlock();
  return ret;
}

static int on_keymap_binding_pressed_command(uint32_t command,
                                             uint32_t argument) {
  if ((command & 0xffffu) == RGB_OVL_STATE_CMD) {
    size_t word = KP_RGB_OVERLAY_COMMAND_WORD(command);
    if (word >= KP_RGB_OVERLAY_WORDS)
      return -EINVAL;
    kp_rgb_overlay_set_word(word, argument);
    return 0;
  }
  int ret;
  switch (command) {
  case RGB_TOG_CMD:
    ret = zmk_rgb_matrix_toggle();
    break;
  case RGB_ON_CMD:
    ret = zmk_rgb_matrix_on();
    break;
  case RGB_OFF_CMD:
    ret = zmk_rgb_matrix_off();
    break;
  case RGB_BRI_CMD:
  case RGB_BRD_CMD:
    kp_rgb_change_brt(command == RGB_BRI_CMD ? 1 : -1);
    ret = 0;
    break;
  case RGB_BRT_SET_CMD:
    if (argument > KP_RGB_BRT_MAX)
      return -EINVAL;
    kp_rgb_matrix_lock();
    if (kp_rgb_global.brightness != argument) {
      kp_rgb_global.brightness = (uint8_t)argument;
      kp_rgb_replication_changed_locked();
      zmk_rgb_matrix_flush();
    }
    kp_rgb_matrix_unlock();
    ret = 0;
    break;
  case RGB_RESET_CMD:
    if (argument != 0)
      return -EINVAL;
    kp_rgb_reset_state();
    return 0;
  case RGB_CLOCK_CMD:
    /* Valid modulo 2^32 while the true offset is under 2^31 ms. */
    kp_rgb_set_clock_offset((int32_t)(argument - (uint32_t)k_uptime_get()));
    return 0;
  default:
    return -ENOTSUP;
  }
  if (ret < 0)
    return ret;
  kp_rgb_save_state();
  return ret;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
  ARG_UNUSED(event);
  int ret = k_is_in_isr()
                ? -EWOULDBLOCK
                : on_keymap_binding_pressed_command(
                      kp_rgb_invocation_command(binding->param1),
                      binding->param2);
  kp_rgb_invocation_finish(binding, ret);
  return ret;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
  ARG_UNUSED(event);
  kp_rgb_invocation_finish(binding, ZMK_BEHAVIOR_OPAQUE);
  return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_rgb_matrix_driver_api = {
    .binding_convert_central_state_dependent_params =
        on_keymap_binding_convert_central_state_dependent_params,
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

BEHAVIOR_DT_DEFINE(KP_RGB_NODE, NULL, NULL, NULL, NULL, POST_KERNEL,
                   KP_RGB_DEVICE_INIT_PRIORITY,
                   &behavior_rgb_matrix_driver_api);
