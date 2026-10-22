/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <dt-bindings/keypaw/rgb_matrix.h>
#include <zephyr/sys/util.h>
#include <zmk/behavior.h>
#include <zmk/rgb_matrix.h>

#include "rgb_selector_internal.h"

const struct device *kp_rgb_selector_at(const struct kp_rgb_selector *selector,
                                        size_t slot) {
  if (slot >= selector->effect_count)
    return NULL;
  const struct device *effect = selector->members[slot].effect;
  return effect != NULL && device_is_ready(effect) ? effect : NULL;
}

uint16_t kp_rgb_selector_calc_index(const struct kp_rgb_selector *selector,
                                    uint16_t current, int16_t delta) {
  size_t count = selector->effect_count;
  int64_t norm_delta = delta % (int64_t)count;
  size_t start =
      ((int64_t)current + norm_delta + (int64_t)count) % (int64_t)count;
  if (kp_rgb_selector_at(selector, start) != NULL) {
    return (uint16_t)start;
  }
  int direction = delta < 0 ? -1 : 1;
  for (size_t i = 1; i < count; i++) {
    size_t slot = ((int64_t)start + (int64_t)i * direction + (int64_t)count) %
                  (int64_t)count;
    if (kp_rgb_selector_at(selector, slot) != NULL) {
      return (uint16_t)slot;
    }
  }
  return current;
}

int kp_rgb_selector_resolve_locked(struct kp_rgb_selector *selector) {
  const struct device *previous = selector->active_fx;
  if (selector->effect_index >= selector->effect_count) {
    selector->effect_index = 0;
  }
  selector->effect_index =
      kp_rgb_selector_calc_index(selector, selector->effect_index, 0);
  selector->active_fx = kp_rgb_selector_at(selector, selector->effect_index);
  kp_rgb_selector_publish_locked(selector, previous);
  return selector->active_fx == NULL ? -ENOENT : 0;
}

void kp_rgb_selector_apply_defaults_locked(struct kp_rgb_selector *selector) {
  for (size_t i = 0; i < selector->effect_count; i++) {
    const struct device *fx = kp_rgb_selector_at(selector, i);
    if (fx == NULL) {
      continue;
    }
    *kp_rgb_effect_data(fx) = kp_rgb_effect_cfg(fx)->defaults;
  }
}

int kp_rgb_selector_select_locked(struct kp_rgb_selector *selector,
                                  uint16_t slot) {
  if (slot >= selector->effect_count) {
    return -EINVAL;
  }
  const struct device *fx = kp_rgb_selector_at(selector, slot);
  if (fx == NULL) {
    return -ENOENT;
  }
  if (selector->effect_index == slot && selector->active_fx == fx) {
    return 0;
  }
  const struct device *previous = selector->active_fx;
  selector->effect_index = slot;
  selector->active_fx = fx;
  kp_rgb_selector_publish_locked(selector, previous);
  return 1;
}

int kp_rgb_selector_member_slot(const struct kp_rgb_selector *selector,
                                const struct device *effect) {
  if (effect == NULL || selector->owner == NULL) {
    return -ENOENT;
  }
  const struct kp_rgb_effect_common_config *cfg = kp_rgb_effect_cfg(effect);
  if (cfg == NULL || cfg->owner != selector->owner ||
      kp_rgb_selector_at(selector, cfg->slot) != effect) {
    return -ENOENT;
  }
  return cfg->slot;
}

static bool kp_rgb_selector_command(uint32_t command) {
  switch (command) {
  case RGB_HUI_CMD:
  case RGB_HUD_CMD:
  case RGB_SAI_CMD:
  case RGB_SAD_CMD:
  case RGB_SPI_CMD:
  case RGB_SPD_CMD:
  case RGB_EFF_CMD:
  case RGB_EFR_CMD:
  case RGB_EFS_CMD:
  case RGB_COLOR_HSB_CMD:
  case RGB_SELECTOR_COLOR_CMD:
  case RGB_SELECTOR_PERIOD_CMD:
  case RGB_RESET_CMD:
    return true;
  default:
    return false;
  }
}

static int kp_rgb_selector_validate_color(uint32_t argument) {
  return (argument >> 16) > KP_RGB_HUE_MAX ||
                 ((argument >> 8) & 0xff) > KP_RGB_SAT_MAX ||
                 (argument & 0xff) > KP_RGB_BRT_MAX
             ? -EINVAL
             : 0;
}

int kp_rgb_selector_convert_locked(struct kp_rgb_selector *selector,
                                   struct zmk_behavior_binding *binding) {
  if (selector == NULL)
    return -ENODEV;
  uint32_t command = binding->param1;
  uint32_t argument = binding->param2;
  if (command == RGB_RESET_CMD)
    return argument == 0 ? 0 : -EINVAL;
  uint32_t opcode = command & KP_RGB_SELECTOR_COMMAND_MASK;
  if (!kp_rgb_selector_command(command) && opcode != RGB_SELECTOR_COLOR_CMD &&
      opcode != RGB_SELECTOR_PERIOD_CMD)
    return -ENOTSUP;

  uint16_t slot = selector->effect_index;
  bool addressed = opcode == RGB_SELECTOR_COLOR_CMD ||
                   opcode == RGB_SELECTOR_PERIOD_CMD;
  if (addressed) {
    slot = (uint16_t)(command >> 16);
    if (slot >= selector->effect_count)
      return -EINVAL;
    command = opcode;
  }

  if (command == RGB_EFS_CMD) {
    if (argument > UINT16_MAX || argument >= selector->effect_count)
      return -EINVAL;
    if (kp_rgb_selector_at(selector, argument) == NULL)
      return -ENOENT;
    return 0;
  }
  const struct device *effect =
      addressed ? kp_rgb_selector_at(selector, slot) : selector->active_fx;
  if (effect == NULL || !device_is_ready(effect))
    return addressed ? -ENOENT : -ENODEV;
  struct kp_rgb_hsb color = kp_rgb_effect_data(effect)->color;
  if (command == RGB_EFF_CMD || command == RGB_EFR_CMD) {
    if (argument != 0)
      return -EINVAL;
    argument = kp_rgb_selector_calc_index(selector, selector->effect_index,
                                         command == RGB_EFF_CMD ? 1 : -1);
    command = RGB_EFS_CMD;
  } else if (command == RGB_HUI_CMD || command == RGB_HUD_CMD) {
    if (argument != 0)
      return -EINVAL;
    int64_t hue = ((int64_t)color.h +
                   (command == RGB_HUI_CMD ? 1 : -1) *
                       (int64_t)CONFIG_KEYPAW_RGB_MATRIX_HUE_STEP) % KP_RGB_HUE_MAX;
    color.h = (uint16_t)(hue < 0 ? hue + KP_RGB_HUE_MAX : hue);
    argument = RGB_COLOR_HSB_VAL(color.h, color.s, color.b);
    command = RGB_SELECTOR_COLOR_CMD;
  } else if (command == RGB_SAI_CMD || command == RGB_SAD_CMD) {
    if (argument != 0)
      return -EINVAL;
    color.s = (uint8_t)CLAMP((int64_t)color.s +
                                 (command == RGB_SAI_CMD ? 1 : -1) *
                                     (int64_t)CONFIG_KEYPAW_RGB_MATRIX_SAT_STEP,
                             0, KP_RGB_SAT_MAX);
    argument = RGB_COLOR_HSB_VAL(color.h, color.s, color.b);
    command = RGB_SELECTOR_COLOR_CMD;
  } else if (command == RGB_SPI_CMD || command == RGB_SPD_CMD) {
    if (command == RGB_SPD_CMD && argument != 0)
      return -EINVAL;
    if (argument == 0) {
      int64_t duration = (int64_t)kp_rgb_effect_period(effect) +
                         (command == RGB_SPI_CMD ? 1 : -1) *
                             (int64_t)CONFIG_KEYPAW_RGB_MATRIX_DURATION_STEP;
      argument = (uint32_t)CLAMP(duration,
                                 CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS,
                                 CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
    }
    command = RGB_SELECTOR_PERIOD_CMD;
  } else if (command == RGB_COLOR_HSB_CMD) {
    command = RGB_SELECTOR_COLOR_CMD;
  }

  if (command == RGB_SELECTOR_COLOR_CMD) {
    int err = kp_rgb_selector_validate_color(argument);
    if (err < 0)
      return err;
  } else if (command == RGB_SELECTOR_PERIOD_CMD) {
    if (argument < CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS ||
        argument > CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS)
      return -EINVAL;
  }
  binding->param1 = command == RGB_EFS_CMD
                        ? command
                        : KP_RGB_SELECTOR_MEMBER_COMMAND(command, slot);
  binding->param2 = argument;
  return 0;
}

int kp_rgb_selector_dispatch_locked(struct kp_rgb_selector *selector,
                                    uint32_t command, uint32_t argument) {
  if (selector == NULL)
    return -ENODEV;
  struct zmk_behavior_binding binding = {.param1 = command, .param2 = argument};
  int err = kp_rgb_selector_convert_locked(selector, &binding);
  if (err < 0)
    return err;
  command = binding.param1;
  argument = binding.param2;
  if (command == RGB_RESET_CMD)
    return kp_rgb_selector_overlay_reset_locked(selector->owner);
  if (command == RGB_EFS_CMD) {
    err = kp_rgb_selector_select_locked(selector, (uint16_t)argument);
    if (err > 0)
      zmk_rgb_matrix_flush();
    return err < 0 ? err : 0;
  }
  uint32_t opcode = command & KP_RGB_SELECTOR_COMMAND_MASK;
  uint16_t slot = (uint16_t)(command >> 16);
  const struct device *effect = kp_rgb_selector_at(selector, slot);
  if (effect == NULL)
    return -ENOENT;
  switch (opcode) {
  case RGB_SELECTOR_COLOR_CMD: {
    struct kp_rgb_hsb color = {
        .h = argument >> 16, .s = (argument >> 8) & 0xff, .b = argument & 0xff};
    return kp_rgb_set_color_locked(effect, &color);
  }
  case RGB_SELECTOR_PERIOD_CMD:
    return kp_rgb_set_period_locked(effect, argument);
  default:
    return -ENOTSUP;
  }
}
