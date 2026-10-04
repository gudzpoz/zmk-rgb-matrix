/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#define DT_DRV_COMPAT keypaw_behavior_rgb_matrix

#include <drivers/behavior.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <dt-bindings/keypaw/rgb_matrix.h>
#include <zmk/keymap.h>
#include <zmk/rgb_matrix.h>

#include "rgb_matrix_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
             "RGB matrix requires exactly one enabled controller");

#define KP_CONTROLLER DT_INST(0, keypaw_behavior_rgb_matrix)
#define KP_RGB_MAX_EFFECTS MAX(1, DT_CHILD_NUM(KP_CONTROLLER))

BUILD_ASSERT(!DT_NODE_HAS_PROP(KP_CONTROLLER, leds),
             "RGB controller always owns the whole strip; remove leds");
BUILD_ASSERT(sizeof(DEVICE_DT_NAME(KP_CONTROLLER)) <= 9,
             "RGB controller name must fit the split behavior_dev field");
BUILD_ASSERT(DT_CHILD_NUM(KP_CONTROLLER) <= KP_RGB_MAX_REGISTRY_EFFECTS,
             "RGB effect registry exceeds persistence capacity");
BUILD_ASSERT(DT_CHILD_NUM(KP_CONTROLLER) <= UINT8_MAX,
             "RGB effect count must fit the blob count byte");

#define KP_RGB_EFFECT_DEVICE(node_id)                                          \
  COND_CODE_1(DT_NODE_HAS_STATUS(node_id, okay), (DEVICE_DT_GET(node_id), ),   \
              (NULL, ))
#define KP_RGB_NO_CYCLE_ONE(node_id) DT_PROP(node_id, no_cycle),
#define KP_RGB_EFFECT_DEFAULT_ONE(node_id)                                     \
  {.color = KP_RGB_HSB_FROM_HEX(DT_PROP_OR(node_id, color, 0)),                \
   .duration_ms = (uint16_t)DT_PROP_OR(node_id, duration, 0)},

static const struct device *const kp_rgb_effects[KP_RGB_MAX_EFFECTS] = {
    DT_FOREACH_CHILD(KP_CONTROLLER, KP_RGB_EFFECT_DEVICE)};
static const uint8_t kp_rgb_effect_no_cycle[KP_RGB_MAX_EFFECTS] = {
    DT_FOREACH_CHILD(KP_CONTROLLER, KP_RGB_NO_CYCLE_ONE)};
static const struct kp_rgb_effect_defaults
    kp_rgb_effect_defaults[KP_RGB_MAX_EFFECTS] = {
        DT_FOREACH_CHILD(KP_CONTROLLER, KP_RGB_EFFECT_DEFAULT_ONE)};

struct kp_rgb_controller kp_rgb_controller = {
    .effects = kp_rgb_effects,
    .effect_defaults = kp_rgb_effect_defaults,
    .no_cycle = kp_rgb_effect_no_cycle,
    .effect_count = DT_CHILD_NUM(KP_CONTROLLER),
    .initial_on = DT_PROP_OR(KP_CONTROLLER, initial_on, 1),
    .initial_brightness = DT_PROP_OR(KP_CONTROLLER, initial_brightness, 30),
    .initial_duration_ms = DT_PROP_OR(KP_CONTROLLER, initial_duration_ms, 1000),
    .initial_effect = DT_PROP_OR(KP_CONTROLLER, initial_effect, 0),
    .tuning =
        {
            .max_brightness = DT_PROP_OR(KP_CONTROLLER, max_brightness, 40),
            .idle_brightness = DT_PROP_OR(KP_CONTROLLER, idle_brightness, 30),
        },
};

static int kp_rgb_behavior_init(const struct device *dev) {
  kp_rgb_controller.dev = dev;
  kp_rgb_controller.state.on =
      kp_rgb_effective_on(kp_rgb_controller.initial_on,
                          IS_ENABLED(CONFIG_KEYPAW_RGB_MATRIX_AUTO_OFF_IDLE));
  if (kp_rgb_apply_defaults() < 0) {
    LOG_WRN("Initial RGB effect unavailable");
  }
  return 0;
}
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
    {.display_name = "Brightness Up",
     .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = RGB_BRI_CMD},
    {.display_name = "Brightness Down",
     .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = RGB_BRD_CMD},
    {.display_name = "Duration Up",
     .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = RGB_SPI_CMD},
    {.display_name = "Duration Down",
     .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = RGB_SPD_CMD},
    {.display_name = "Next Effect",
     .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = RGB_EFF_CMD},
    {.display_name = "Previous Effect",
     .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = RGB_EFR_CMD},
    {.display_name = "Reset Settings",
     .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = RGB_RESET_CMD},
};
static const struct behavior_parameter_metadata_set no_args_set = {
    .param1_values = no_arg_values,
    .param1_values_len = ARRAY_SIZE(no_arg_values),
};
static const struct behavior_parameter_metadata sets[] = {no_args_set};
static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(sets),
    .sets = sets,
};
#endif

size_t kp_rgb_effect_count(void) { return kp_rgb_controller.effect_count; }

const struct device *kp_rgb_effect_at(size_t index) {
  return index < kp_rgb_controller.effect_count
             ? kp_rgb_controller.effects[index]
             : NULL;
}

size_t kp_rgb_selected_effect(void) { return kp_rgb_controller.effect_index; }

static bool kp_effect_cyclable(size_t index) {
  return kp_rgb_effect_at(index) != NULL && !kp_rgb_controller.no_cycle[index];
}

uint16_t kp_rgb_calc_effect_index(uint16_t current, int16_t delta) {
  size_t count = kp_rgb_effect_count();
  if (count == 0) {
    return current;
  }
  int16_t norm_delta = delta % (int16_t)count;
  uint16_t start = (current + norm_delta + count) % count;
  if (kp_effect_cyclable(start)) {
    return start;
  }
  int direction = delta < 0 ? -1 : 1;
  for (size_t i = 1; i < count; i++) {
    size_t index = (start + (i * direction) + count) % count;
    if (kp_effect_cyclable(index)) {
      return index;
    }
  }
  return current;
}

int kp_rgb_resolve_active(void) {
  if (kp_rgb_effect_count() == 0) {
    return -ENOENT;
  }
  if (kp_rgb_controller.effect_index >= kp_rgb_effect_count()) {
    kp_rgb_controller.effect_index = 0;
  }
  kp_rgb_controller.effect_index =
      kp_rgb_calc_effect_index(kp_rgb_controller.effect_index, 0);
  kp_rgb_controller.state.active_fx =
      kp_rgb_effect_at(kp_rgb_controller.effect_index);
  return kp_rgb_controller.state.active_fx == NULL ? -ENOENT : 0;
}

int kp_rgb_apply_defaults(void) {
  uint8_t brightness =
      (uint8_t)CLAMP(kp_rgb_controller.initial_brightness, 0, KP_RGB_BRT_MAX);
  uint16_t duration = (uint16_t)CLAMP(kp_rgb_controller.initial_duration_ms,
                                      CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS,
                                      CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
  kp_rgb_matrix_lock();
  kp_rgb_controller.state.user_on = kp_rgb_controller.initial_on;
  for (size_t i = 0; i < kp_rgb_effect_count(); i++) {
    const struct device *fx = kp_rgb_effect_at(i);
    if (fx == NULL) {
      continue;
    }
    const struct kp_rgb_effect_defaults *def =
        &kp_rgb_controller.effect_defaults[i];
    struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(fx);
    data->color = def->color;
    data->color.b = brightness;
    data->duration_ms = def->duration_ms != 0 ? def->duration_ms : duration;
  }
  kp_rgb_controller.effect_index = kp_rgb_controller.initial_effect;
  int ret = kp_rgb_resolve_active();
  kp_rgb_matrix_unlock();
  return ret;
}

int kp_rgb_select_effect(uint16_t index) {
  if (index >= kp_rgb_effect_count()) {
    return -EINVAL;
  }
  if (kp_rgb_effect_at(index) == NULL) {
    return -ENOENT;
  }
  kp_rgb_matrix_lock();
  kp_rgb_controller.effect_index = index;
  kp_rgb_controller.state.active_fx = kp_rgb_effect_at(index);
  kp_rgb_matrix_unlock();
  return 0;
}

int zmk_rgb_matrix_select_effect(uint16_t index) {
  return kp_rgb_select_effect(index);
}

int zmk_rgb_matrix_cycle_effect(int16_t direction) {
  return kp_rgb_select_effect(
      kp_rgb_calc_effect_index(kp_rgb_controller.effect_index, direction));
}

int kp_rgb_calc_effect(int16_t direction) {
  return (int)kp_rgb_calc_effect_index(kp_rgb_controller.effect_index,
                                       direction);
}

static struct kp_rgb_hsb kp_active_hsb(void) {
  if (kp_rgb_controller.state.active_fx == NULL) {
    return (struct kp_rgb_hsb){
        .h = 0, .s = 0, .b = kp_rgb_controller.tuning.max_brightness};
  }
  return kp_rgb_effect_data(kp_rgb_controller.state.active_fx)->color;
}

int kp_rgb_set_hsb(struct kp_rgb_hsb color) {
  if (kp_rgb_controller.state.active_fx == NULL) {
    return -ENODEV;
  }
  if (color.h > KP_RGB_HUE_MAX || color.s > KP_RGB_SAT_MAX ||
      color.b > KP_RGB_BRT_MAX) {
    return -EINVAL;
  }
  kp_rgb_matrix_lock();
  kp_rgb_effect_data(kp_rgb_controller.state.active_fx)->color = color;
  kp_rgb_matrix_unlock();
  return 0;
}

struct kp_rgb_hsb kp_rgb_calc_hue(int8_t direction) {
  struct kp_rgb_hsb color = kp_active_hsb();
  color.h = (color.h + KP_RGB_HUE_MAX +
             (int32_t)direction * CONFIG_KEYPAW_RGB_MATRIX_HUE_STEP) %
            KP_RGB_HUE_MAX;
  return color;
}

struct kp_rgb_hsb kp_rgb_calc_sat(int8_t direction) {
  struct kp_rgb_hsb color = kp_active_hsb();
  color.s = (uint8_t)CLAMP((int)color.s + (int)direction *
                                              CONFIG_KEYPAW_RGB_MATRIX_SAT_STEP,
                           0, KP_RGB_SAT_MAX);
  return color;
}

struct kp_rgb_hsb kp_rgb_calc_brt(int8_t direction) {
  struct kp_rgb_hsb color = kp_active_hsb();
  color.b = (uint8_t)CLAMP((int)color.b + (int)direction *
                                              CONFIG_KEYPAW_RGB_MATRIX_BRT_STEP,
                           0, KP_RGB_BRT_MAX);
  return color;
}

int kp_rgb_change_hue(int8_t direction) {
  return kp_rgb_set_hsb(kp_rgb_calc_hue(direction));
}

int kp_rgb_change_sat(int8_t direction) {
  return kp_rgb_set_hsb(kp_rgb_calc_sat(direction));
}

int kp_rgb_change_brt(int8_t direction) {
  return kp_rgb_set_hsb(kp_rgb_calc_brt(direction));
}

static uint32_t kp_active_duration(void) {
  return kp_rgb_controller.state.active_fx == NULL
             ? 0
             : kp_rgb_effect_period(kp_rgb_controller.state.active_fx);
}

int kp_rgb_set_duration(uint32_t duration_ms) {
  if (kp_rgb_controller.state.active_fx == NULL) {
    return -ENODEV;
  }
  int32_t duration = CLAMP((int32_t)duration_ms,
                           (int32_t)CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS,
                           (int32_t)CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
  kp_rgb_matrix_lock();
  kp_rgb_effect_data(kp_rgb_controller.state.active_fx)->duration_ms =
      (uint16_t)duration;
  kp_rgb_matrix_unlock();
  return 0;
}

int kp_rgb_change_duration(int16_t direction) {
  int32_t next = (int32_t)kp_active_duration() +
                 (int32_t)direction * CONFIG_KEYPAW_RGB_MATRIX_DURATION_STEP;
  return kp_rgb_set_duration((uint32_t)MAX(next, 0));
}

int kp_rgb_effect_convert_central_state_dependent_params(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event) {
  ARG_UNUSED(event);
  const struct device *fx = zmk_behavior_get_binding(binding->behavior_dev);
  if (fx == NULL) {
    return -ENODEV;
  }
  const struct kp_rgb_effect_common_config *cfg = kp_rgb_effect_cfg(fx);
  if (cfg == NULL || kp_rgb_effect_at(cfg->index) != fx) {
    return -ENODEV;
  }
  binding->behavior_dev = kp_rgb_controller.dev->name;
  binding->param1 = RGB_EFS_CMD;
  binding->param2 = cfg->index;
  return 0;
}

static int on_keymap_binding_convert_central_state_dependent_params(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event) {
  ARG_UNUSED(event);
  switch (binding->param1) {
  case RGB_TOG_CMD: {
    bool state;
    int err = zmk_rgb_matrix_get_state(&state);
    if (err) {
      return err;
    }
    binding->param1 = state ? RGB_OFF_CMD : RGB_ON_CMD;
    break;
  }
  case RGB_BRI_CMD:
  case RGB_BRD_CMD:
  case RGB_HUI_CMD:
  case RGB_HUD_CMD:
  case RGB_SAI_CMD:
  case RGB_SAD_CMD: {
    struct kp_rgb_hsb color;
    switch (binding->param1) {
    case RGB_BRI_CMD:
      color = kp_rgb_calc_brt(1);
      break;
    case RGB_BRD_CMD:
      color = kp_rgb_calc_brt(-1);
      break;
    case RGB_HUI_CMD:
      color = kp_rgb_calc_hue(1);
      break;
    case RGB_HUD_CMD:
      color = kp_rgb_calc_hue(-1);
      break;
    case RGB_SAI_CMD:
      color = kp_rgb_calc_sat(1);
      break;
    default:
      color = kp_rgb_calc_sat(-1);
      break;
    }
    binding->param1 = RGB_COLOR_HSB_CMD;
    binding->param2 = RGB_COLOR_HSB_VAL(color.h, color.s, color.b);
    break;
  }
  case RGB_EFR_CMD:
    binding->param1 = RGB_EFS_CMD;
    binding->param2 = (uint32_t)kp_rgb_calc_effect(-1);
    break;
  case RGB_EFF_CMD:
    binding->param1 = RGB_EFS_CMD;
    binding->param2 = (uint32_t)kp_rgb_calc_effect(1);
    break;
  case RGB_SPI_CMD:
  case RGB_SPD_CMD: {
    int32_t step = binding->param1 == RGB_SPI_CMD ? 1 : -1;
    int32_t duration =
        CLAMP((int32_t)kp_active_duration() +
                  step * (int32_t)CONFIG_KEYPAW_RGB_MATRIX_DURATION_STEP,
              (int32_t)CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS,
              (int32_t)CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
    binding->param1 = RGB_SPI_CMD;
    binding->param2 = (uint32_t)duration;
    break;
  }
  default:
    return 0;
  }
  return 0;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
  ARG_UNUSED(event);
  int ret;
  switch (binding->param1) {
  case RGB_TOG_CMD: {
    kp_rgb_matrix_lock();
    bool toggle_on = !kp_rgb_controller.state.on;
    kp_rgb_controller.state.user_on = toggle_on;
    kp_rgb_matrix_unlock();
    ret = toggle_on ? zmk_rgb_matrix_on() : zmk_rgb_matrix_off();
    break;
  }
  case RGB_ON_CMD:
    kp_rgb_matrix_lock();
    kp_rgb_controller.state.user_on = true;
    kp_rgb_matrix_unlock();
    ret = zmk_rgb_matrix_on();
    break;
  case RGB_OFF_CMD:
    kp_rgb_matrix_lock();
    kp_rgb_controller.state.user_on = false;
    kp_rgb_matrix_unlock();
    ret = zmk_rgb_matrix_off();
    break;
  case RGB_HUI_CMD:
    ret = kp_rgb_change_hue(1);
    break;
  case RGB_HUD_CMD:
    ret = kp_rgb_change_hue(-1);
    break;
  case RGB_SAI_CMD:
    ret = kp_rgb_change_sat(1);
    break;
  case RGB_SAD_CMD:
    ret = kp_rgb_change_sat(-1);
    break;
  case RGB_BRI_CMD:
    ret = kp_rgb_change_brt(1);
    break;
  case RGB_BRD_CMD:
    ret = kp_rgb_change_brt(-1);
    break;
  case RGB_SPI_CMD:
    ret = binding->param2 != 0 ? kp_rgb_set_duration(binding->param2)
                               : kp_rgb_change_duration(1);
    break;
  case RGB_SPD_CMD:
    ret = kp_rgb_change_duration(-1);
    break;
  case RGB_EFS_CMD:
    ret = kp_rgb_select_effect((uint16_t)binding->param2);
    break;
  case RGB_EFF_CMD:
    ret = zmk_rgb_matrix_cycle_effect(1);
    break;
  case RGB_EFR_CMD:
    ret = zmk_rgb_matrix_cycle_effect(-1);
    break;
  case RGB_COLOR_HSB_CMD:
    ret = kp_rgb_set_hsb(
        (struct kp_rgb_hsb){.h = (binding->param2 >> 16) & 0xFFFF,
                            .s = (binding->param2 >> 8) & 0xFF,
                            .b = binding->param2 & 0xFF});
    break;
  case RGB_RESET_CMD:
    kp_rgb_reset_state();
    return 0;
  case RGB_OVL_STATE_CMD:
    if (kp_rgb_overlay_set_word(RGB_OVL_STATE_WORD(binding->param2),
                                RGB_OVL_STATE_BITS(binding->param2))) {
      zmk_rgb_matrix_flush();
    }
    return 0;
  default:
    return -ENOTSUP;
  }
  if (ret < 0) {
    return ret;
  }
  kp_rgb_save_state();
  return ret;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
  ARG_UNUSED(binding);
  ARG_UNUSED(event);
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

BEHAVIOR_DT_DEFINE(KP_CONTROLLER, kp_rgb_behavior_init, NULL, NULL, NULL,
                   POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                   &behavior_rgb_matrix_driver_api);
