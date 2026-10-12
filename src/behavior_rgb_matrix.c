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
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <dt-bindings/keypaw/rgb_matrix.h>
#include <zmk/keymap.h>
#include <zmk/rgb_matrix.h>

#include "rgb_matrix_internal.h"
#include "rgb_selector_sync_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
             "RGB matrix requires exactly one enabled controller");

#define KP_CONTROLLER DT_INST(0, keypaw_behavior_rgb_matrix)
#define KP_RGB_MAX_EFFECTS MAX(1, DT_CHILD_NUM(KP_CONTROLLER))

BUILD_ASSERT(!DT_NODE_HAS_PROP(KP_CONTROLLER, leds),
             "RGB controller always owns the whole strip; remove leds");
BUILD_ASSERT(sizeof(DEVICE_DT_NAME(KP_CONTROLLER)) <= 9,
             "RGB controller name must fit the split behavior_dev field");

#if DT_NODE_HAS_PROP(KP_CONTROLLER, selector)
BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_PHANDLE(KP_CONTROLLER, selector),
                                keypaw_rgb_overlay_selector),
             "RGB matrix selector alias must reference a selector overlay");
BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_PHANDLE(KP_CONTROLLER, selector), okay),
             "RGB matrix selector alias must reference an enabled selector");
#endif

const struct device *kp_rgb_selector_alias(void) {
  return COND_CODE_1(DT_NODE_HAS_PROP(KP_CONTROLLER, selector),
                     (DEVICE_DT_GET(DT_PHANDLE(KP_CONTROLLER, selector))),
                     (NULL));
}

#define KP_RGB_EFFECT_ID_TOKEN(node_id)                                        \
  COND_CODE_1(DT_NODE_HAS_PROP(node_id, persist_id),                           \
              (DT_STRING_TOKEN(node_id, persist_id)),                          \
              (DT_NODE_FULL_NAME_TOKEN(node_id)))

/* The persisted identity (persist-id, else the node name) must fit the settings
 * record and per-effect key buffers; reject an over-long one at build time. */
#define KP_RGB_EFFECT_ID_LENGTH_ASSERT(node_id)                                \
  BUILD_ASSERT(                                                                \
      sizeof(DT_PROP_OR(node_id, persist_id, DT_NODE_FULL_NAME(node_id))) <=   \
          KP_RGB_PERSIST_MAX_ID_LENGTH,                                        \
      "RGB effect persist-id or node name exceeds "                            \
      "KP_RGB_PERSIST_MAX_ID_LENGTH");

#define KP_RGB_PERSIST_UNIQUE(node_id)                                         \
  CONCAT(kp_rgb_persist_id_, KP_RGB_EFFECT_ID_TOKEN(node_id)),

DT_FOREACH_CHILD(KP_CONTROLLER, KP_RGB_EFFECT_ID_LENGTH_ASSERT)
enum {
  DT_FOREACH_CHILD(KP_CONTROLLER, KP_RGB_PERSIST_UNIQUE)
  __kp_rgb_persist_id_end,
};

#define KP_RGB_EFFECT_DEVICE(node_id)                                          \
  COND_CODE_1(DT_NODE_HAS_STATUS(node_id, okay), (DEVICE_DT_GET(node_id), ),   \
              (NULL, ))
#define KP_RGB_EFFECT_DEFAULT_ONE(node_id)                                     \
  {.color = KP_RGB_HSB_FROM_HEX(DT_PROP_OR(node_id, color, 0)),                \
   .duration_ms = DT_PROP_OR(node_id, duration, 0)},

static const struct device *const kp_rgb_effects[KP_RGB_MAX_EFFECTS] = {
    DT_FOREACH_CHILD(KP_CONTROLLER, KP_RGB_EFFECT_DEVICE)};
static const struct kp_rgb_effect_defaults
    kp_rgb_effect_defaults[KP_RGB_MAX_EFFECTS] = {
        DT_FOREACH_CHILD(KP_CONTROLLER, KP_RGB_EFFECT_DEFAULT_ONE)};

struct kp_rgb_global kp_rgb_global = {
    .initial_on = DT_PROP_OR(KP_CONTROLLER, initial_on, 1),
    .initial_brightness = DT_PROP_OR(KP_CONTROLLER, initial_brightness, 30),
    .tuning =
        {
            .max_brightness = DT_PROP_OR(KP_CONTROLLER, max_brightness, 40),
            .idle_brightness = DT_PROP_OR(KP_CONTROLLER, idle_brightness, 30),
        },
};

struct kp_rgb_controller kp_rgb_controller = {
    .selector =
        {
            .effects = kp_rgb_effects,
            .effect_defaults = kp_rgb_effect_defaults,
            .effect_count = DT_CHILD_NUM(KP_CONTROLLER),
            .initial_duration_ms =
                DT_PROP_OR(KP_CONTROLLER, initial_duration_ms, 1000),
            .initial_effect = DT_PROP_OR(KP_CONTROLLER, initial_effect, 0),
        },
};

static int kp_rgb_behavior_init(const struct device *dev) {
  kp_rgb_controller.dev = dev;
  kp_rgb_controller.selector.owner = dev;
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
static const struct behavior_parameter_value_metadata absolute_commands[] = {
    {.display_name = "Select Effect",
     .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = RGB_EFS_CMD},
    {.display_name = "Hue/Saturation",
     .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = RGB_COLOR_HSB_CMD},
    {.display_name = "Duration Up/Set",
     .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = RGB_SPI_CMD},
};
static const struct behavior_parameter_value_metadata absolute_values[] = {
    {.display_name = "Argument",
     .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
     .range = {.min = 0, .max = UINT32_MAX}},
};
static const struct behavior_parameter_metadata_set sets[] = {
    no_args_set,
    {.param1_values = absolute_commands,
     .param1_values_len = ARRAY_SIZE(absolute_commands),
     .param2_values = absolute_values,
     .param2_values_len = ARRAY_SIZE(absolute_values)},
};
static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(sets),
    .sets = sets,
};
#endif

size_t kp_rgb_effect_count(void) {
  return kp_rgb_selector_count(&kp_rgb_controller.selector);
}

const struct device *kp_rgb_effect_at(size_t index) {
  return kp_rgb_selector_at(&kp_rgb_controller.selector, index);
}

size_t kp_rgb_selected_effect(void) {
  return kp_rgb_controller.selector.effect_index;
}

uint16_t kp_rgb_calc_effect_index(uint16_t current, int16_t delta) {
  return kp_rgb_selector_calc_index(&kp_rgb_controller.selector, current,
                                    delta);
}

int kp_rgb_resolve_active(void) {
  return kp_rgb_selector_resolve_locked(&kp_rgb_controller.selector);
}

int kp_rgb_apply_defaults(void) {
  uint8_t brightness =
      (uint8_t)CLAMP(kp_rgb_global.initial_brightness, 0, KP_RGB_BRT_MAX);
  kp_rgb_matrix_lock();
  kp_rgb_global.user_on = kp_rgb_global.initial_on;
  kp_rgb_global.brightness = brightness;
  int ret = kp_rgb_selector_apply_defaults_locked(&kp_rgb_controller.selector);
  kp_rgb_reconcile_power_locked();
  zmk_rgb_matrix_flush();
  kp_rgb_matrix_unlock();
  return ret;
}

int kp_rgb_select_effect_locked(uint16_t index) {
  int ret = kp_rgb_selector_select_locked(&kp_rgb_controller.selector, index);
  if (ret > 0) {
    zmk_rgb_matrix_flush();
  }
  return ret < 0 ? ret : 0;
}

int kp_rgb_select_effect(uint16_t index) {
  if (k_is_in_isr()) {
    return -EWOULDBLOCK;
  }
  kp_rgb_matrix_lock();
  int ret = kp_rgb_select_effect_locked(index);
  kp_rgb_matrix_unlock();
  return ret;
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

int kp_rgb_cycle_effect(int16_t direction) {
  if (k_is_in_isr()) {
    return -EWOULDBLOCK;
  }
  kp_rgb_matrix_lock();
  int ret = kp_rgb_select_effect_locked(kp_rgb_calc_effect_index(
      kp_rgb_controller.selector.effect_index, direction));
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

int kp_rgb_calc_effect(int16_t direction) {
  kp_rgb_matrix_lock();
  int index = kp_rgb_calc_effect_index(kp_rgb_controller.selector.effect_index,
                                       direction);
  kp_rgb_matrix_unlock();
  return index;
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

int kp_rgb_set_hsb(struct kp_rgb_hsb color) {
  if (k_is_in_isr()) {
    return -EWOULDBLOCK;
  }
  kp_rgb_matrix_lock();
  int ret =
      kp_rgb_set_color_locked(kp_rgb_controller.selector.active_fx, &color);
  kp_rgb_matrix_unlock();
  return ret;
}

static struct kp_rgb_hsb kp_active_hsb(void) {
  if (!kp_rgb_parameter_target_ready(kp_rgb_controller.selector.active_fx)) {
    return (struct kp_rgb_hsb){
        .h = 0, .s = 0, .b = kp_rgb_global.tuning.max_brightness};
  }
  struct kp_rgb_hsb color =
      kp_rgb_effect_data(kp_rgb_controller.selector.active_fx)->color;
  color.b = kp_rgb_global.brightness;
  return color;
}

static struct kp_rgb_hsb kp_rgb_calc_hue_locked(int8_t direction) {
  struct kp_rgb_hsb color = kp_active_hsb();
  int64_t hue = ((int64_t)color.h +
                 (int64_t)direction * CONFIG_KEYPAW_RGB_MATRIX_HUE_STEP) %
                KP_RGB_HUE_MAX;
  color.h = (uint16_t)(hue < 0 ? hue + KP_RGB_HUE_MAX : hue);
  return color;
}

struct kp_rgb_hsb kp_rgb_calc_hue(int8_t direction) {
  kp_rgb_matrix_lock();
  struct kp_rgb_hsb color = kp_rgb_calc_hue_locked(direction);
  kp_rgb_matrix_unlock();
  return color;
}

int kp_rgb_change_hue(int8_t direction) {
  if (k_is_in_isr()) {
    return -EWOULDBLOCK;
  }
  kp_rgb_matrix_lock();
  struct kp_rgb_hsb color = kp_rgb_calc_hue_locked(direction);
  int ret =
      kp_rgb_set_color_locked(kp_rgb_controller.selector.active_fx, &color);
  kp_rgb_matrix_unlock();
  return ret;
}

static struct kp_rgb_hsb kp_rgb_calc_sat_locked(int8_t direction) {
  struct kp_rgb_hsb color = kp_active_hsb();
  color.s = (uint8_t)CLAMP(
      (int64_t)color.s + (int64_t)direction * CONFIG_KEYPAW_RGB_MATRIX_SAT_STEP,
      0, KP_RGB_SAT_MAX);
  return color;
}

struct kp_rgb_hsb kp_rgb_calc_sat(int8_t direction) {
  kp_rgb_matrix_lock();
  struct kp_rgb_hsb color = kp_rgb_calc_sat_locked(direction);
  kp_rgb_matrix_unlock();
  return color;
}

int kp_rgb_change_sat(int8_t direction) {
  if (k_is_in_isr()) {
    return -EWOULDBLOCK;
  }
  kp_rgb_matrix_lock();
  struct kp_rgb_hsb color = kp_rgb_calc_sat_locked(direction);
  int ret =
      kp_rgb_set_color_locked(kp_rgb_controller.selector.active_fx, &color);
  kp_rgb_matrix_unlock();
  return ret;
}

static struct kp_rgb_hsb kp_rgb_calc_brt_locked(int8_t direction) {
  struct kp_rgb_hsb color = kp_active_hsb();
  color.b =
      (uint8_t)CLAMP((int64_t)kp_rgb_global.brightness +
                         (int64_t)direction * CONFIG_KEYPAW_RGB_MATRIX_BRT_STEP,
                     0, KP_RGB_BRT_MAX);
  return color;
}

struct kp_rgb_hsb kp_rgb_calc_brt(int8_t direction) {
  kp_rgb_matrix_lock();
  struct kp_rgb_hsb color = kp_rgb_calc_brt_locked(direction);
  kp_rgb_matrix_unlock();
  return color;
}

int kp_rgb_change_brt(int8_t direction) {
  if (k_is_in_isr()) {
    return -EWOULDBLOCK;
  }
  kp_rgb_matrix_lock();
  struct kp_rgb_hsb color = kp_rgb_calc_brt_locked(direction);
  kp_rgb_global.brightness = color.b;
  zmk_rgb_matrix_flush();
  kp_rgb_matrix_unlock();
  return 0;
}

static uint32_t kp_active_duration(void) {
  return !kp_rgb_parameter_target_ready(kp_rgb_controller.selector.active_fx)
             ? 0
             : kp_rgb_effect_period(kp_rgb_controller.selector.active_fx);
}

static uint32_t kp_rgb_calc_duration_locked(int16_t direction) {
  int64_t next = (int64_t)kp_active_duration() +
                 (int64_t)direction * CONFIG_KEYPAW_RGB_MATRIX_DURATION_STEP;
  return (uint32_t)CLAMP(next, CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS,
                         CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
}

int kp_rgb_set_duration(uint32_t duration_ms) {
  if (k_is_in_isr()) {
    return -EWOULDBLOCK;
  }
  kp_rgb_matrix_lock();
  int ret = kp_rgb_set_period_locked(kp_rgb_controller.selector.active_fx,
                                     duration_ms);
  kp_rgb_matrix_unlock();
  return ret;
}

int kp_rgb_change_duration(int16_t direction) {
  if (k_is_in_isr()) {
    return -EWOULDBLOCK;
  }
  kp_rgb_matrix_lock();
  int ret = kp_rgb_set_period_locked(kp_rgb_controller.selector.active_fx,
                                     kp_rgb_calc_duration_locked(direction));
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
  if (cfg == NULL) {
    kp_rgb_matrix_unlock();
    return -ENODEV;
  }
  const struct device *owner = cfg->owner;
  bool legacy = owner == kp_rgb_controller.dev;
  struct kp_rgb_selector *selector =
      legacy ? &kp_rgb_controller.selector
             : kp_rgb_selector_overlay_state_locked(owner);
  const struct device *control =
      legacy ? kp_rgb_controller.dev : kp_rgb_selector_control(owner);
  int slot =
      selector == NULL ? -ENOENT : kp_rgb_selector_member_slot(selector, fx);
  int err =
      slot < 0 || control == NULL || !device_is_ready(control) ? -ENODEV : 0;
  if (err == 0) {
    binding->behavior_dev = control->name;
    binding->param1 = legacy ? RGB_LEGACY_EFS_CMD : RGB_EFS_CMD;
    binding->param2 = (uint32_t)slot;
  }
  kp_rgb_matrix_unlock();
  return err;
}

static int on_keymap_binding_convert_central_state_dependent_params(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event) {
  ARG_UNUSED(event);
  if (k_is_in_isr())
    return -EWOULDBLOCK;
  const struct device *alias = kp_rgb_selector_alias();
  if (kp_rgb_selector_command(binding->param1) && alias != NULL) {
    return kp_rgb_selector_binding_convert(alias, binding);
  }
  kp_rgb_matrix_lock();
  int ret = 0;
  switch (binding->param1) {
  case RGB_TOG_CMD:
    binding->param1 = kp_rgb_global.user_on ? RGB_OFF_CMD : RGB_ON_CMD;
    break;
  case RGB_BRI_CMD:
  case RGB_BRD_CMD:
    binding->param2 =
        kp_rgb_calc_brt_locked(binding->param1 == RGB_BRI_CMD ? 1 : -1).b;
    binding->param1 = RGB_BRT_SET_CMD;
    break;
  default:
    if (kp_rgb_selector_command(binding->param1)) {
      ret =
          kp_rgb_selector_convert_locked(&kp_rgb_controller.selector, binding);
    }
    break;
  }
  kp_rgb_matrix_unlock();
  return ret;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
  ARG_UNUSED(event);
  if (k_is_in_isr())
    return -EWOULDBLOCK;
  uint32_t command = binding->param1;
  uint32_t argument = binding->param2;
  const struct device *alias = kp_rgb_selector_alias();
  if (kp_rgb_selector_command(command) && alias != NULL) {
    return kp_rgb_selector_binding_apply(alias, command, argument);
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
    ret = kp_rgb_change_brt(command == RGB_BRI_CMD ? 1 : -1);
    break;
  case RGB_BRT_SET_CMD:
    if (argument > KP_RGB_BRT_MAX)
      return -EINVAL;
    kp_rgb_matrix_lock();
    if (kp_rgb_global.brightness != argument) {
      kp_rgb_global.brightness = (uint8_t)argument;
      zmk_rgb_matrix_flush();
    }
    kp_rgb_matrix_unlock();
    ret = 0;
    break;
  case RGB_RESET_CMD:
    if (argument != 0)
      return -EINVAL;
    kp_rgb_reset_state();
    kp_rgb_matrix_lock();
    kp_rgb_split_selectors_dirty_locked();
    kp_rgb_matrix_unlock();
    return 0;
  case RGB_OVL_STATE_CMD:
    kp_rgb_overlay_set_word(RGB_OVL_STATE_WORD(argument),
                            RGB_OVL_STATE_BITS(argument));
    return 0;
  case RGB_CLOCK_CMD:
    /* Valid modulo 2^32 while the true offset is under 2^31 ms. */
    kp_rgb_set_clock_offset((int32_t)(argument - (uint32_t)k_uptime_get()));
    return 0;
  default:
    if (command == RGB_LEGACY_EFS_CMD)
      command = RGB_EFS_CMD;
    else if (command == RGB_LEGACY_COLOR_CMD)
      command = RGB_COLOR_HSB_CMD;
    else if (command == RGB_LEGACY_PERIOD_CMD) {
      if (argument == 0)
        return -EINVAL;
      command = RGB_SPI_CMD;
    }
    if (!kp_rgb_selector_command(command))
      return -ENOTSUP;
    kp_rgb_matrix_lock();
    ret = kp_rgb_selector_dispatch_locked(&kp_rgb_controller.selector, command,
                                          argument);
    kp_rgb_matrix_unlock();
    break;
  }
  if (ret < 0)
    return ret;
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
