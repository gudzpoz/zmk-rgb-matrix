/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <drivers/behavior.h>
#include <zephyr/sys/util.h>
#include <zmk/behavior.h>

#include "../rgb_matrix_internal.h"
#include "../rgb_replication_internal.h"

int kp_rgb_selector_binding_convert(const struct device *selector,
                                    struct zmk_behavior_binding *binding) {
  kp_rgb_assert_thread_context();
  kp_rgb_matrix_lock();
  struct kp_rgb_selector *state =
      kp_rgb_selector_overlay_state_locked(selector);
  if (state == NULL) {
    kp_rgb_matrix_unlock();
    return -ENODEV;
  }
  int err = kp_rgb_selector_convert_locked(state, binding);
  if (err == 0)
    binding->behavior_dev = state->owner->name;
  kp_rgb_matrix_unlock();
  return err;
}

int kp_rgb_selector_binding_apply(const struct device *selector,
                                  uint32_t command, uint32_t argument) {
  kp_rgb_assert_thread_context();
  kp_rgb_matrix_lock();
  struct kp_rgb_selector *state =
      kp_rgb_selector_overlay_state_locked(selector);
  int err = state == NULL
                ? -ENODEV
                : kp_rgb_selector_dispatch_locked(state, command, argument);
  if (err >= 0 &&
      (command == RGB_EFF_CMD || command == RGB_EFR_CMD ||
       command == RGB_EFS_CMD || command == RGB_RESET_CMD))
    err = kp_rgb_save_selector_state(selector, command == RGB_RESET_CMD);
  kp_rgb_matrix_unlock();
  return err;
}

static const struct device *
kp_control_selector(const struct zmk_behavior_binding *binding) {
  return zmk_behavior_get_binding(binding->behavior_dev);
}

static int kp_selector_convert(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
  ARG_UNUSED(event);
  kp_rgb_assert_thread_context();
  if (binding->param1 & KP_RGB_INVOCATION_MARKER)
    return -ENOTSUP;
  int err = kp_rgb_selector_binding_convert(kp_control_selector(binding), binding);
  if (err == 0)
    err = kp_rgb_invocation_begin(binding);
  return err;
}

static int kp_selector_pressed(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
  ARG_UNUSED(event);
  kp_rgb_invocation_assert_thread(binding);
  uint32_t command = kp_rgb_invocation_command(binding->param1);
  int err = kp_rgb_selector_binding_apply(kp_control_selector(binding), command,
                                          binding->param2);
  kp_rgb_invocation_finish(binding, err);
  return err;
}

static int kp_selector_released(struct zmk_behavior_binding *binding,
                                struct zmk_behavior_binding_event event) {
  ARG_UNUSED(event);
  kp_rgb_invocation_assert_thread(binding);
  kp_rgb_invocation_finish(binding, ZMK_BEHAVIOR_OPAQUE);
  return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata
    kp_selector_no_arg_values[] = {
        {.display_name = "Hue Up",
         .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
         .value = RGB_HUI_CMD},
        {.display_name = "Hue Down",
         .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
         .value = RGB_HUD_CMD},
        {.display_name = "Saturation Up",
         .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
         .value = RGB_SAI_CMD},
        {.display_name = "Saturation Down",
         .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
         .value = RGB_SAD_CMD},
        {.display_name = "Duration Down",
         .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
         .value = RGB_SPD_CMD},
        {.display_name = "Next Effect",
         .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
         .value = RGB_EFF_CMD},
        {.display_name = "Previous Effect",
         .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
         .value = RGB_EFR_CMD},
        {.display_name = "Reset Selector",
         .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
         .value = RGB_RESET_CMD},
};
static const struct behavior_parameter_value_metadata
    kp_selector_slot_command[] = {
        {.display_name = "Select Effect",
         .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
         .value = RGB_EFS_CMD},
};
static const struct behavior_parameter_value_metadata kp_selector_slot_value[] =
    {
        {.display_name = "Local Slot",
         .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
         .range = {.min = 0, .max = UINT16_MAX}},
};
static const struct behavior_parameter_value_metadata
    kp_selector_period_command[] = {
        {.display_name = "Duration Up/Set",
         .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
         .value = RGB_SPI_CMD},
};
static const struct behavior_parameter_value_metadata
    kp_selector_period_value[] = {
        {.display_name = "Increase",
         .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
         .value = 0},
        {.display_name = "Period (ms)",
         .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
         .range = {.min = CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS,
                   .max = CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS}},
};
static const struct behavior_parameter_value_metadata
    kp_selector_color_command[] = {
        {.display_name = "Color (HSB)",
         .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
         .value = RGB_COLOR_HSB_CMD},
};
static const struct behavior_parameter_value_metadata
    kp_selector_color_value[] = {
        {.display_name = "Packed HSB",
         .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
         .range = {.min = 0, .max = RGB_COLOR_HSB_VAL(360, 100, 100)}},
};
static const struct behavior_parameter_metadata_set kp_selector_sets[] = {
    {.param1_values = kp_selector_no_arg_values,
     .param1_values_len = ARRAY_SIZE(kp_selector_no_arg_values)},
    {.param1_values = kp_selector_slot_command,
     .param1_values_len = ARRAY_SIZE(kp_selector_slot_command),
     .param2_values = kp_selector_slot_value,
     .param2_values_len = ARRAY_SIZE(kp_selector_slot_value)},
    {.param1_values = kp_selector_period_command,
     .param1_values_len = ARRAY_SIZE(kp_selector_period_command),
     .param2_values = kp_selector_period_value,
     .param2_values_len = ARRAY_SIZE(kp_selector_period_value)},
    {.param1_values = kp_selector_color_command,
     .param1_values_len = ARRAY_SIZE(kp_selector_color_command),
     .param2_values = kp_selector_color_value,
     .param2_values_len = ARRAY_SIZE(kp_selector_color_value)},
};
static const struct behavior_parameter_metadata kp_selector_metadata = {
    .sets_len = ARRAY_SIZE(kp_selector_sets),
    .sets = kp_selector_sets,
};
#endif

const struct behavior_driver_api kp_rgb_selector_behavior_api = {
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
    .binding_convert_central_state_dependent_params = kp_selector_convert,
    .binding_pressed = kp_selector_pressed,
    .binding_released = kp_selector_released,
    IF_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA,
               (.parameter_metadata = &kp_selector_metadata, ))};
