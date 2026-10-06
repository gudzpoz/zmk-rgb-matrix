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

struct kp_rgb_controller kp_rgb_controller = {
    .effects = kp_rgb_effects,
    .effect_defaults = kp_rgb_effect_defaults,
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

uint16_t kp_rgb_calc_effect_index(uint16_t current, int16_t delta) {
  size_t count = kp_rgb_effect_count();
  if (count == 0) {
    return current;
  }
  int16_t norm_delta = delta % (int16_t)count;
  uint16_t start = (current + norm_delta + count) % count;
  if (kp_rgb_effect_at(start) != NULL) {
    return start;
  }
  int direction = delta < 0 ? -1 : 1;
  for (size_t i = 1; i < count; i++) {
    size_t index = (start + (i * direction) + count) % count;
    if (kp_rgb_effect_at(index) != NULL) {
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
  kp_rgb_controller.brightness = brightness;
  for (size_t i = 0; i < kp_rgb_effect_count(); i++) {
    const struct device *fx = kp_rgb_effect_at(i);
    if (fx == NULL) {
      continue;
    }
    const struct kp_rgb_effect_defaults *def =
        &kp_rgb_controller.effect_defaults[i];
    struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(fx);
    data->color = def->color;
    data->color.b = KP_RGB_BRT_MAX;
    data->duration_ms =
        def->duration_ms != 0
            ? (uint16_t)CLAMP(def->duration_ms,
                              CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS,
                              CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS)
            : duration;
  }
  kp_rgb_controller.effect_index = kp_rgb_controller.initial_effect;
  int ret = kp_rgb_resolve_active();
  kp_rgb_reconcile_power_locked();
  zmk_rgb_matrix_flush();
  kp_rgb_matrix_unlock();
  return ret;
}

int kp_rgb_select_effect_locked(uint16_t index) {
  if (index >= kp_rgb_effect_count()) {
    return -EINVAL;
  }
  const struct device *fx = kp_rgb_effect_at(index);
  if (fx == NULL) {
    return -ENOENT;
  }
  if (kp_rgb_controller.effect_index != index ||
      kp_rgb_controller.state.active_fx != fx) {
    kp_rgb_controller.effect_index = index;
    kp_rgb_controller.state.active_fx = fx;
    zmk_rgb_matrix_flush();
  }
  return 0;
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
  return kp_rgb_select_effect(index);
}

int zmk_rgb_matrix_cycle_effect(int16_t direction) {
  if (k_is_in_isr()) {
    return -EWOULDBLOCK;
  }
  kp_rgb_matrix_lock();
  int ret = kp_rgb_select_effect_locked(
      kp_rgb_calc_effect_index(kp_rgb_controller.effect_index, direction));
  kp_rgb_matrix_unlock();
  return ret;
}

int kp_rgb_calc_effect(int16_t direction) {
  kp_rgb_matrix_lock();
  int index = kp_rgb_calc_effect_index(kp_rgb_controller.effect_index, direction);
  kp_rgb_matrix_unlock();
  return index;
}

static bool kp_rgb_parameter_target_ready(const struct device *fx) {
  return fx != NULL && device_is_ready(fx);
}

static int kp_rgb_set_color_locked(const struct device *fx,
                                   const struct kp_rgb_hsb *color) {
  if (!kp_rgb_parameter_target_ready(fx)) {
    return -ENODEV;
  }
  if (color == NULL || color->h > KP_RGB_HUE_MAX ||
      color->s > KP_RGB_SAT_MAX || color->b > KP_RGB_BRT_MAX) {
    return -EINVAL;
  }
  struct kp_rgb_hsb next = *color;
  next.h %= KP_RGB_HUE_MAX;
  struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(fx);
  if (data->color.h != next.h || data->color.s != next.s
  ) {
    data->color.h = next.h;
    data->color.s = next.s;
    data->color.b = KP_RGB_BRT_MAX;
    zmk_rgb_matrix_flush();
  }
  return 0;
}

static int kp_rgb_set_period_locked(const struct device *fx, uint32_t period_ms) {
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
  int ret = kp_rgb_set_color_locked(kp_rgb_controller.state.active_fx, &color);
  kp_rgb_matrix_unlock();
  return ret;
}

static struct kp_rgb_hsb kp_active_hsb(void) {
  if (!kp_rgb_parameter_target_ready(kp_rgb_controller.state.active_fx)) {
    return (struct kp_rgb_hsb){
        .h = 0, .s = 0, .b = kp_rgb_controller.tuning.max_brightness};
  }
  struct kp_rgb_hsb color =
      kp_rgb_effect_data(kp_rgb_controller.state.active_fx)->color;
  color.b = kp_rgb_controller.brightness;
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
  int ret = kp_rgb_set_color_locked(kp_rgb_controller.state.active_fx, &color);
  kp_rgb_matrix_unlock();
  return ret;
}

static struct kp_rgb_hsb kp_rgb_calc_sat_locked(int8_t direction) {
  struct kp_rgb_hsb color = kp_active_hsb();
  color.s = (uint8_t)CLAMP((int64_t)color.s + (int64_t)direction *
                                                  CONFIG_KEYPAW_RGB_MATRIX_SAT_STEP,
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
  int ret = kp_rgb_set_color_locked(kp_rgb_controller.state.active_fx, &color);
  kp_rgb_matrix_unlock();
  return ret;
}

static struct kp_rgb_hsb kp_rgb_calc_brt_locked(int8_t direction) {
  struct kp_rgb_hsb color = kp_active_hsb();
  color.b =
      (uint8_t)CLAMP((int64_t)kp_rgb_controller.brightness +
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
  kp_rgb_controller.brightness = color.b;
  zmk_rgb_matrix_flush();
  kp_rgb_matrix_unlock();
  return 0;
}

static uint32_t kp_active_duration(void) {
  return !kp_rgb_parameter_target_ready(kp_rgb_controller.state.active_fx)
             ? 0
             : kp_rgb_effect_period(kp_rgb_controller.state.active_fx);
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
  int ret = kp_rgb_set_period_locked(kp_rgb_controller.state.active_fx, duration_ms);
  kp_rgb_matrix_unlock();
  return ret;
}

int kp_rgb_change_duration(int16_t direction) {
  if (k_is_in_isr()) {
    return -EWOULDBLOCK;
  }
  kp_rgb_matrix_lock();
  int ret = kp_rgb_set_period_locked(kp_rgb_controller.state.active_fx,
                                     kp_rgb_calc_duration_locked(direction));
  kp_rgb_matrix_unlock();
  return ret;
}

int kp_rgb_effect_convert_central_state_dependent_params(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event) {
  ARG_UNUSED(event);
  const struct device *fx = zmk_behavior_get_binding(binding->behavior_dev);
  if (!kp_rgb_parameter_target_ready(fx)) {
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
  if (k_is_in_isr()) {
    return -EWOULDBLOCK;
  }
  kp_rgb_matrix_lock();
  switch (binding->param1) {
  case RGB_TOG_CMD: {
    binding->param1 = kp_rgb_controller.state.user_on ? RGB_OFF_CMD : RGB_ON_CMD;
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
      color = kp_rgb_calc_brt_locked(1);
      break;
    case RGB_BRD_CMD:
      color = kp_rgb_calc_brt_locked(-1);
      break;
    case RGB_HUI_CMD:
      color = kp_rgb_calc_hue_locked(1);
      break;
    case RGB_HUD_CMD:
      color = kp_rgb_calc_hue_locked(-1);
      break;
    case RGB_SAI_CMD:
      color = kp_rgb_calc_sat_locked(1);
      break;
    default:
      color = kp_rgb_calc_sat_locked(-1);
      break;
    }
    binding->param1 = RGB_COLOR_HSB_CMD;
    binding->param2 = RGB_COLOR_HSB_VAL(color.h, color.s, color.b);
    break;
  }
  case RGB_EFR_CMD:
    binding->param1 = RGB_EFS_CMD;
    binding->param2 = (uint32_t)kp_rgb_calc_effect_index(
        kp_rgb_controller.effect_index, -1);
    break;
  case RGB_EFF_CMD:
    binding->param1 = RGB_EFS_CMD;
    binding->param2 = (uint32_t)kp_rgb_calc_effect_index(
        kp_rgb_controller.effect_index, 1);
    break;
  case RGB_SPI_CMD:
  case RGB_SPD_CMD: {
    uint32_t duration = kp_rgb_calc_duration_locked(
        binding->param1 == RGB_SPI_CMD ? 1 : -1);
    binding->param1 = RGB_SPI_CMD;
    binding->param2 = (uint32_t)duration;
    break;
  }
  default:
    break;
  }
  kp_rgb_matrix_unlock();
  return 0;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
  ARG_UNUSED(event);
  int ret;
  switch (binding->param1) {
  case RGB_TOG_CMD:
    ret = zmk_rgb_matrix_toggle();
    break;
  case RGB_ON_CMD:
    ret = zmk_rgb_matrix_on();
    break;
  case RGB_OFF_CMD:
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
    kp_rgb_overlay_set_word(RGB_OVL_STATE_WORD(binding->param2),
                            RGB_OVL_STATE_BITS(binding->param2));
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
