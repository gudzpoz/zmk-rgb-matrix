/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include <dt-bindings/keypaw/rgb_matrix.h>
#include <zephyr/sys/__assert.h>
#include <zmk/behavior.h>

#include "rgb_selector_sync_internal.h"
#include <zmk/rgb_matrix.h>

int kp_rgb_selector_snapshot_locked(const struct device *selector,
                                    struct kp_rgb_selector_snapshot *snapshot) {
  if (snapshot == NULL)
    return -EINVAL;
  struct kp_rgb_selector *state =
      kp_rgb_selector_overlay_state_locked(selector);
  if (state == NULL || !device_is_ready(state->owner) ||
      state->active_fx == NULL || !device_is_ready(state->active_fx))
    return -ENODEV;
  size_t slot = state->effect_index;
  if (slot >= state->effect_count || state->members[slot].effect != state->active_fx)
    return -EINVAL;
  const struct kp_rgb_effect_common_data *data =
      kp_rgb_effect_data(state->active_fx);
  *snapshot = (struct kp_rgb_selector_snapshot){.control = state->owner,
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
  if (state == NULL || !device_is_ready(state->owner))
    return -ENODEV;
  while (*cursor < state->effect_count) {
    size_t slot = (*cursor)++;
    const struct device *effect = kp_rgb_selector_at(state, slot);
    if (effect == NULL)
      continue;
    const struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(effect);
    *snapshot = (struct kp_rgb_selector_snapshot){.control = state->owner,
                                                 .slot = (uint16_t)slot,
                                                 .color = data->color,
                                                 .period_ms = data->duration_ms};
    return 1;
  }
  return 0;
}

void kp_rgb_selector_snapshot_binding(
    const struct kp_rgb_selector_snapshot *snapshot, uint8_t step,
    struct zmk_behavior_binding *binding) {
  __ASSERT_NO_MSG(snapshot != NULL && snapshot->control != NULL &&
                  binding != NULL && step < 3);
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
  default:
    command = RGB_EFS_CMD;
    argument = snapshot->slot;
    break;
  }
  *binding =
      (struct zmk_behavior_binding){.behavior_dev = snapshot->control->name,
                                    .param1 = command,
                                    .param2 = argument};
}
