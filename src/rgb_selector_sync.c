/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include <dt-bindings/keypaw/rgb_matrix.h>
#include <zmk/behavior.h>

#include "rgb_selector_sync_internal.h"
#include <zmk/rgb_matrix.h>

int kp_rgb_selector_snapshot_locked(const struct device *selector,
                                    struct kp_rgb_selector_snapshot *snapshot) {
  if (snapshot == NULL)
    return -EINVAL;
  struct kp_rgb_selector *state =
      kp_rgb_selector_overlay_state_locked(selector);
  const struct device *control = kp_rgb_selector_control(selector);
  if (state == NULL || control == NULL || !device_is_ready(control) ||
      state->active_fx == NULL || !device_is_ready(state->active_fx))
    return -ENODEV;
  int slot = kp_rgb_selector_member_slot(state, state->active_fx);
  if (slot < 0 || slot > UINT16_MAX)
    return -EINVAL;
  const struct kp_rgb_effect_common_data *data =
      kp_rgb_effect_data(state->active_fx);
  *snapshot = (struct kp_rgb_selector_snapshot){.control = control,
                                                .slot = (uint16_t)slot,
                                                .color = data->color,
                                                .period_ms = data->duration_ms};
  return 0;
}

int kp_rgb_selector_next_snapshot_locked(const struct device *selector,
                                         size_t *cursor,
                                         struct kp_rgb_selector_snapshot *snapshot) {
  if (cursor == NULL || snapshot == NULL)
    return -EINVAL;
  struct kp_rgb_selector *state = kp_rgb_selector_overlay_state_locked(selector);
  const struct device *control = kp_rgb_selector_control(selector);
  if (state == NULL || control == NULL || !device_is_ready(control))
    return -ENODEV;
  while (*cursor < state->effect_count) {
    size_t slot = (*cursor)++;
    const struct device *effect = kp_rgb_selector_at(state, slot);
    if (effect == NULL)
      continue;
    if (slot > UINT16_MAX)
      return -EINVAL;
    const struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(effect);
    *snapshot = (struct kp_rgb_selector_snapshot){.control = control,
                                                 .slot = (uint16_t)slot,
                                                 .color = data->color,
                                                 .period_ms = data->duration_ms};
    return 1;
  }
  return 0;
}

int kp_rgb_selector_snapshot_binding(
    const struct kp_rgb_selector_snapshot *snapshot, uint8_t step,
    struct zmk_behavior_binding *binding) {
  if (snapshot == NULL || binding == NULL || snapshot->control == NULL)
    return -EINVAL;
  uint32_t command, argument;
  switch (step) {
  case 0:
    command =
        KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, snapshot->slot);
    argument = RGB_COLOR_HSB_VAL(snapshot->color.h, snapshot->color.s,
                                 snapshot->color.b);
    break;
  case 1:
    command =
        KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, snapshot->slot);
    argument = snapshot->period_ms;
    break;
  case 2:
    command = RGB_EFS_CMD;
    argument = snapshot->slot;
    break;
  default:
    return -EINVAL;
  }
  *binding =
      (struct zmk_behavior_binding){.behavior_dev = snapshot->control->name,
                                    .param1 = command,
                                    .param2 = argument};
  return 0;
}

int kp_rgb_selector_member_apply_locked(struct kp_rgb_selector *selector,
                                        uint32_t command, uint32_t argument) {
  if (selector == NULL)
    return -ENODEV;
  uint32_t opcode = command & KP_RGB_SELECTOR_COMMAND_MASK;
  if (opcode != RGB_SELECTOR_COLOR_CMD && opcode != RGB_SELECTOR_PERIOD_CMD)
    return -ENOTSUP;
  uint16_t slot = command >> 16;
  if (slot >= selector->effect_count)
    return -EINVAL;
  const struct device *effect = kp_rgb_selector_at(selector, slot);
  if (effect == NULL)
    return -ENOENT;
  if (opcode == RGB_SELECTOR_PERIOD_CMD)
    return kp_rgb_set_period_locked(effect, argument);
  if ((argument >> 16) > KP_RGB_HUE_MAX ||
      ((argument >> 8) & 0xff) > KP_RGB_SAT_MAX ||
      (argument & 0xff) > KP_RGB_BRT_MAX)
    return -EINVAL;
  struct kp_rgb_hsb color = {
      .h = argument >> 16, .s = (argument >> 8) & 0xff, .b = argument & 0xff};
  return kp_rgb_set_color_locked(effect, &color);
}
