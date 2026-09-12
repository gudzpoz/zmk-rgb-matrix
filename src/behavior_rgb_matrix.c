/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Keymap behavior for the keypaw RGB matrix. Modelled on
 * zmk/app/src/behaviors/behavior_rgb_underglow.c, including the
 * convert_central_state_dependent_params hook that resolves relative commands
 * (toggle, step, cycle) into absolute ones on the split central so every half
 * ends up applying the same value.
 *
 * Effects themselves are child nodes of &kprgb, each its own zero-param behavior
 * (e.g. &fx_solid). Selecting one from a keymap runs that effect node's
 * convert hook, which rewrites the binding to (kprgb, RGB_EFS_CMD, index) so
 * only the short &kprgb name + the effect's explicit `index` cross the split
 * link. This file owns the effect registry built from those children, the
 * apply/cycle/resolve helpers, the animation tunables, and the &kprgb control
 * behavior (RGB_TOG, RGB_HUI, ...).
 */

#include <stddef.h>
#include <stdint.h>

#define DT_DRV_COMPAT keypaw_behavior_rgb_matrix

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <dt-bindings/keypaw/rgb_matrix.h>
#include <zmk/keymap.h>
#include <zmk/rgb_matrix.h>

#include "rgb_matrix_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define KP_RGB_BEHAVIOR DT_DRV_INST(0)
#define KP_RGB_NEFFECTS DT_CHILD_NUM(KP_RGB_BEHAVIOR)

/* The animation tunables live on the behavior node, so the behavior owns the
 * tuning object the engine hands to effects through the frame. */
struct kp_rgb_tuning kp_rgb_tuning = {
  .max_brightness = DT_PROP_OR(KP_RGB_BEHAVIOR, max_brightness, 40),
  .idle_brightness = DT_PROP_OR(KP_RGB_BEHAVIOR, idle_brightness, 30),
};

static const struct device *kp_effects[KP_RGB_NEFFECTS];
static size_t effect_index;

/* Effects excluded from RGB_EFF/EFR cycling, indexed by effect index (which the
 * registry BUILD_ASSERT pins to the child position). Device presence is not
 * enough: a `no-cycle` effect is registered and selectable by index, it just
 * never shows up while cycling. */
#define KP_RGB_NO_CYCLE_ONE(node_id) DT_PROP(node_id, no_cycle),
static const uint8_t kp_effect_no_cycle[] = {
  DT_FOREACH_CHILD(KP_RGB_BEHAVIOR, KP_RGB_NO_CYCLE_ONE)};

static bool kp_effect_cyclable(size_t index) {
  return kp_effects[index] != NULL && !kp_effect_no_cycle[index];
}

uint16_t kp_rgb_calc_effect_index(uint16_t current, int16_t delta) {
  int16_t norm_delta = delta % (int16_t)KP_RGB_NEFFECTS;
  uint16_t start = (current + norm_delta + KP_RGB_NEFFECTS) % KP_RGB_NEFFECTS;

  if (kp_effect_cyclable(start)) {
    return start;
  }

  int direction = (delta < 0) ? -1 : 1;
  for (int i = 1; i < KP_RGB_NEFFECTS; i++) {
    size_t index = (start + (i * direction) + KP_RGB_NEFFECTS) % KP_RGB_NEFFECTS;
    if (kp_effect_cyclable(index)) {
      return index;
    }
  }

  return current;
}

int kp_rgb_resolve_active(void) {
  if (effect_index >= KP_RGB_NEFFECTS) {
    effect_index = 0;
  }

  effect_index = kp_rgb_calc_effect_index(effect_index, 0);
  const struct device *fx = kp_effects[effect_index];
  if (fx == NULL) {
    return -ENOENT;
  }

  kp_rgb_state.active_fx = fx;
  return 0;
}

/* Shared convert hook used by every effect device (see KP_RGB_EFFECT_DEFINE).
 * The binding arrives here as (effect_name, 0, 0); we rewrite it to
 * (kprgb, RGB_EFS_CMD, index) so the short &kprgb name + the stable index cross
 * the split link rather than a potentially long effect node name. */
int kp_rgb_effect_convert_central_state_dependent_params(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event) {
  ARG_UNUSED(event);

  const struct device *fx_dev = zmk_behavior_get_binding(binding->behavior_dev);
  if (fx_dev == NULL) {
    return -ENODEV;
  }

  /* struct kp_rgb_effect_common_config should be the first member. */
  const struct kp_rgb_effect_common_config *cfg = kp_rgb_effect_cfg(fx_dev);
  if (cfg == NULL) {
    return -ENODEV;
  }

  BUILD_ASSERT(sizeof(DEVICE_DT_NAME(KP_RGB_BEHAVIOR)) <= 9,
               "keypaw,behavior-rgb-matrix: node name must fit the 9-byte split "
               "behavior_dev field");
  binding->behavior_dev = DEVICE_DT_NAME(KP_RGB_BEHAVIOR);
  binding->param1 = RGB_EFS_CMD;
  binding->param2 = cfg->index;

  return 0;
}

int zmk_rgb_matrix_select_effect(uint16_t index) {
  if (index >= KP_RGB_NEFFECTS) {
    return -EINVAL;
  }

  const struct device *fx = kp_effects[index];
  if (fx == NULL) {
    return -ENOENT;
  }

  kp_rgb_matrix_lock();
  effect_index = index;
  kp_rgb_state.active_fx = fx;
  kp_rgb_matrix_unlock();
  return 0;
}

int zmk_rgb_matrix_cycle_effect(int16_t direction) {
  return zmk_rgb_matrix_select_effect(
      kp_rgb_calc_effect_index(effect_index, direction));
}

int zmk_rgb_matrix_calc_effect(int16_t direction) {
  return (int)kp_rgb_calc_effect_index(effect_index, direction);
}

/* Colour commands operate on the active effect's stored colour, so each effect
 * keeps its own hue/saturation. */
static struct kp_rgb_hsb kp_active_hsb(void) {
  if (kp_rgb_state.active_fx == NULL) {
    return (struct kp_rgb_hsb){.h = 0, .s = 0, .b = kp_rgb_tuning.max_brightness};
  }
  return kp_rgb_hex_to_hsb(kp_rgb_effect_data(kp_rgb_state.active_fx)->color_hex);
}

int zmk_rgb_matrix_set_hsb(struct kp_rgb_hsb color) {
  if (kp_rgb_state.active_fx == NULL) {
    return -ENODEV;
  }
  if (color.h > KP_RGB_HUE_MAX || color.s > KP_RGB_SAT_MAX || color.b > KP_RGB_BRT_MAX) {
    return -EINVAL;
  }

  kp_rgb_matrix_lock();
  kp_rgb_effect_data(kp_rgb_state.active_fx)->color_hex = kp_rgb_hsb_to_hex(color);
  kp_rgb_matrix_unlock();
  return 0;
}

struct kp_rgb_hsb zmk_rgb_matrix_calc_hue(int8_t direction) {
  struct kp_rgb_hsb color = kp_active_hsb();

  color.h = (color.h + KP_RGB_HUE_MAX +
             (int32_t)direction * CONFIG_KEYPAW_RGB_MATRIX_HUE_STEP) %
            KP_RGB_HUE_MAX;

  return color;
}

struct kp_rgb_hsb zmk_rgb_matrix_calc_sat(int8_t direction) {
  struct kp_rgb_hsb color = kp_active_hsb();

  color.s = (uint8_t)CLAMP((int)color.s + (int)direction * CONFIG_KEYPAW_RGB_MATRIX_SAT_STEP, 0,
                           KP_RGB_SAT_MAX);

  return color;
}

struct kp_rgb_hsb zmk_rgb_matrix_calc_brt(int8_t direction) {
  struct kp_rgb_hsb color = kp_active_hsb();

  color.b = (uint8_t)CLAMP((int)color.b + (int)direction * CONFIG_KEYPAW_RGB_MATRIX_BRT_STEP, 0,
                           KP_RGB_BRT_MAX);

  return color;
}

int zmk_rgb_matrix_change_hue(int8_t direction) {
  return zmk_rgb_matrix_set_hsb(zmk_rgb_matrix_calc_hue(direction));
}

int zmk_rgb_matrix_change_sat(int8_t direction) {
  return zmk_rgb_matrix_set_hsb(zmk_rgb_matrix_calc_sat(direction));
}

int zmk_rgb_matrix_change_brt(int8_t direction) {
  return zmk_rgb_matrix_set_hsb(zmk_rgb_matrix_calc_brt(direction));
}

/* Animation duration replaces the old 1..5 "speed": a shorter period animates
 * faster. Stored per effect and seeded with a non-zero value at init. */
static uint32_t kp_active_duration(void) {
  if (kp_rgb_state.active_fx == NULL) {
    /* Only reachable with an empty effect registry, where set_duration() below
     * rejects the command anyway. */
    return 0;
  }

  return kp_rgb_effect_period(kp_rgb_state.active_fx);
}

int zmk_rgb_matrix_set_duration(uint32_t duration_ms) {
  if (kp_rgb_state.active_fx == NULL) {
    return -ENODEV;
  }

  int32_t duration = CLAMP((int32_t)duration_ms, (int32_t)CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS,
                           (int32_t)CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
  kp_rgb_matrix_lock();
  kp_rgb_effect_data(kp_rgb_state.active_fx)->duration_ms = (uint16_t)duration;
  kp_rgb_matrix_unlock();
  return 0;
}

int zmk_rgb_matrix_change_duration(int16_t direction) {
  int32_t next = (int32_t)kp_active_duration() +
                 (int32_t)direction * CONFIG_KEYPAW_RGB_MATRIX_DURATION_STEP;
  return zmk_rgb_matrix_set_duration((uint32_t)MAX(next, 0));
}

/* Register one effect child node. The status check has to happen in the
 * preprocessor, not at runtime: Zephyr only predeclares devices for status
 * "okay" nodes (DT_FOREACH_STATUS_OKAY_NODE in <zephyr/device.h>), so a disabled
 * child would make DEVICE_DT_GET below reference an undeclared symbol. */
#define KP_RGB_REGISTRY_ONE_OKAY(node_id)       \
  do {                                          \
    uint8_t idx = DT_PROP(node_id, index);      \
    kp_effects[idx] = DEVICE_DT_GET(node_id);   \
  } while (0);

#define KP_RGB_REGISTRY_ONE(node_id)                                           \
  COND_CODE_1(DT_NODE_HAS_STATUS(node_id, okay),                               \
              (KP_RGB_REGISTRY_ONE_OKAY(node_id)), ())

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata no_arg_values[] = {
  {
    .display_name = "Toggle On/Off",
    .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
    .value = RGB_TOG_CMD,
  },
  {
    .display_name = "Turn On",
    .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
    .value = RGB_ON_CMD,
  },
  {
    .display_name = "Turn Off",
    .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
    .value = RGB_OFF_CMD,
  },
  {
    .display_name = "Hue Up",
    .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
    .value = RGB_HUI_CMD,
  },
  {
    .display_name = "Hue Down",
    .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
    .value = RGB_HUD_CMD,
  },
  {
    .display_name = "Saturation Up",
    .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
    .value = RGB_SAI_CMD,
  },
  {
    .display_name = "Saturation Down",
    .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
    .value = RGB_SAD_CMD,
  },
  {
    .display_name = "Brightness Up",
    .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
    .value = RGB_BRI_CMD,
  },
  {
    .display_name = "Brightness Down",
    .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
    .value = RGB_BRD_CMD,
  },
  {
    .display_name = "Duration Up",
    .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
    .value = RGB_SPI_CMD,
  },
  {
    .display_name = "Duration Down",
    .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
    .value = RGB_SPD_CMD,
  },
  {
    .display_name = "Next Effect",
    .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
    .value = RGB_EFF_CMD,
  },
  {
    .display_name = "Previous Effect",
    .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
    .value = RGB_EFR_CMD,
  },
};
static const struct behavior_parameter_metadata_set no_args_set = {
  .param1_values = no_arg_values,
  .param1_values_len = ARRAY_SIZE(no_arg_values),
};
static const struct behavior_parameter_metadata sets[] = {
  no_args_set,
};
static const struct behavior_parameter_metadata metadata = {
  .sets_len = ARRAY_SIZE(sets),
  .sets = sets,
};
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static int kp_effects_init(const struct device *dev) {
  ARG_UNUSED(dev);

  DT_FOREACH_CHILD(KP_RGB_BEHAVIOR, KP_RGB_REGISTRY_ONE);
  LOG_DBG("Registered %u RGB matrix effects from devicetree", (uint32_t)KP_RGB_NEFFECTS);

  kp_rgb_state.on = DT_PROP_OR(KP_RGB_BEHAVIOR, initial_on, 1);

  /* Seed the per-effect defaults across every registered effect, not just the
   * one selected at boot: otherwise switching to an effect whose preset colour
   * is at full value (or which has no `duration`) would jump in brightness or
   * change period. A devicetree `duration` of 0 means "unset", so it takes
   * initial-duration-ms; clamping that also guarantees the period is never 0. */
  uint8_t brightness =
      (uint8_t)CLAMP(DT_PROP_OR(KP_RGB_BEHAVIOR, initial_brightness, 30), 0, KP_RGB_BRT_MAX);
  int32_t default_duration =
      CLAMP(DT_PROP_OR(KP_RGB_BEHAVIOR, initial_duration_ms, 1000),
            CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS, CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
  for (size_t i = 0; i < KP_RGB_NEFFECTS; i++) {
    if (kp_effects[i] == NULL) {
      continue;
    }
    struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(kp_effects[i]);
    struct kp_rgb_hsb color = kp_rgb_hex_to_hsb(data->color_hex);
    color.b = brightness;
    data->color_hex = kp_rgb_hsb_to_hex(color);
    if (data->duration_ms == 0) {
      data->duration_ms = (uint16_t)default_duration;
    }
  }

  uint16_t initial = DT_PROP_OR(KP_RGB_BEHAVIOR, initial_effect, 0);
  if (zmk_rgb_matrix_select_effect(initial) < 0) {
    LOG_WRN("Initial RGB effect %u unavailable; falling back to the first one", initial);
    kp_rgb_resolve_active();
  }

  return 0;
}

static int
on_keymap_binding_convert_central_state_dependent_params(struct zmk_behavior_binding *binding,
                                                         struct zmk_behavior_binding_event event) {
  ARG_UNUSED(event);

  switch (binding->param1) {
  case RGB_TOG_CMD: {
    bool state;
    int err = zmk_rgb_matrix_get_state(&state);
    if (err) {
      LOG_ERR("Failed to get RGB matrix state (err %d)", err);
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
      color = zmk_rgb_matrix_calc_brt(1);
      break;
    case RGB_BRD_CMD:
      color = zmk_rgb_matrix_calc_brt(-1);
      break;
    case RGB_HUI_CMD:
      color = zmk_rgb_matrix_calc_hue(1);
      break;
    case RGB_HUD_CMD:
      color = zmk_rgb_matrix_calc_hue(-1);
      break;
    case RGB_SAI_CMD:
      color = zmk_rgb_matrix_calc_sat(1);
      break;
    default:
      color = zmk_rgb_matrix_calc_sat(-1);
      break;
    }

    binding->param1 = RGB_COLOR_HSB_CMD;
    binding->param2 = RGB_COLOR_HSB_VAL(color.h, color.s, color.b);
    break;
  }
  case RGB_EFR_CMD: {
    binding->param1 = RGB_EFS_CMD;
    binding->param2 = (uint32_t)zmk_rgb_matrix_calc_effect(-1);
    break;
  }
  case RGB_EFF_CMD: {
    binding->param1 = RGB_EFS_CMD;
    binding->param2 = (uint32_t)zmk_rgb_matrix_calc_effect(1);
    break;
  }
  case RGB_SPI_CMD:
  case RGB_SPD_CMD: {
    /* Resolve the relative step to an absolute duration so both halves converge
     * even if a relative packet is dropped on the split link. The absolute value
     * rides in param2 (never 0 after clamping), which is why RGB_SPI_CMD doubles
     * as "set duration" -- an internal encoding that deliberately stays out of
     * the behavior metadata. */
    int32_t step = (binding->param1 == RGB_SPI_CMD ? 1 : -1) *
                   (int32_t)CONFIG_KEYPAW_RGB_MATRIX_DURATION_STEP;
    int32_t duration = CLAMP((int32_t)kp_active_duration() + step,
                             (int32_t)CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS,
                             (int32_t)CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
    binding->param1 = RGB_SPI_CMD;
    binding->param2 = (uint32_t)duration;
    break;
  }
  default:
    /* Already absolute: RGB_EFS_CMD, RGB_COLOR_HSB_CMD, RGB_ON_CMD/OFF_CMD, the
     * duration carried in RGB_SPI_CMD, and anything unknown. */
    return 0;
  }

  LOG_DBG("RGB matrix relative converted to absolute (%d/%d)", binding->param1, binding->param2);
  return 0;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
  ARG_UNUSED(event);

  switch (binding->param1) {
  case RGB_TOG_CMD:
    return zmk_rgb_matrix_toggle();
  case RGB_ON_CMD:
    return zmk_rgb_matrix_on();
  case RGB_OFF_CMD:
    return zmk_rgb_matrix_off();
  case RGB_HUI_CMD:
    return zmk_rgb_matrix_change_hue(1);
  case RGB_HUD_CMD:
    return zmk_rgb_matrix_change_hue(-1);
  case RGB_SAI_CMD:
    return zmk_rgb_matrix_change_sat(1);
  case RGB_SAD_CMD:
    return zmk_rgb_matrix_change_sat(-1);
  case RGB_BRI_CMD:
    return zmk_rgb_matrix_change_brt(1);
  case RGB_BRD_CMD:
    return zmk_rgb_matrix_change_brt(-1);
  case RGB_SPI_CMD:
    /* A non-zero param2 is the absolute duration resolved by the convert hook; a
     * bare &kprgb RGB_SPI (param2 == 0) is a local relative step. */
    return binding->param2 != 0 ? zmk_rgb_matrix_set_duration(binding->param2)
                                : zmk_rgb_matrix_change_duration(1);
  case RGB_SPD_CMD:
    return zmk_rgb_matrix_change_duration(-1);
  case RGB_EFS_CMD:
    return zmk_rgb_matrix_select_effect((uint16_t)binding->param2);
  case RGB_EFF_CMD:
    return zmk_rgb_matrix_cycle_effect(1);
  case RGB_EFR_CMD:
    return zmk_rgb_matrix_cycle_effect(-1);
  case RGB_COLOR_HSB_CMD:
    return zmk_rgb_matrix_set_hsb((struct kp_rgb_hsb){.h = (binding->param2 >> 16) & 0xFFFF,
                                                      .s = (binding->param2 >> 8) & 0xFF,
                                                      .b = binding->param2 & 0xFF});
  }

  return -ENOTSUP;
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
  /* Each half drives its own physical strip, so the command has to reach
   * every half: CENTRAL would leave the peripheral's strip dead and
   * EVENT_SOURCE would only light the half the key was pressed on. */
  .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
  .parameter_metadata = &metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, kp_effects_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_rgb_matrix_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
