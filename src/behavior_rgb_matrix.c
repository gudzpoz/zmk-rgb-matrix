/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Per-device RGB matrix behavior state and effect registries. The physical
 * matrix engine is shared, while every behavior node owns its own controller
 * state, effects, tuning, indicators, and LED zone.
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

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define KP_RGB_MAX_EFFECTS(inst) MAX(1, DT_CHILD_NUM(DT_DRV_INST(inst)))

#define KP_RGB_EFFECT_DEVICE(node_id)                                          \
  COND_CODE_1(DT_NODE_HAS_STATUS(node_id, okay), (DEVICE_DT_GET(node_id), ),   \
              (NULL, ))

#define KP_RGB_NO_CYCLE_ONE(node_id) DT_PROP(node_id, no_cycle),

#define KP_RGB_BEHAVIOR_INDICATORS(inst)                                       \
  COND_CODE_1(                                                                 \
      DT_NODE_HAS_PROP(DT_DRV_INST(inst), indicators),                         \
      (static const struct device *const kp_rgb_indicators_##inst[] =          \
           {LISTIFY(DT_PROP_LEN(DT_DRV_INST(inst), indicators),                \
                    KP_RGB_INDICATORS_AT_IDX, (, ), DT_DRV_INST(inst))};),     \
      ())

#define KP_RGB_BEHAVIOR_LEDS(inst)                                             \
  static const uint32_t kp_rgb_leds_##inst[] =                                 \
      COND_CODE_1(DT_NODE_HAS_PROP(DT_DRV_INST(inst), leds),                   \
                  (DT_PROP(DT_DRV_INST(inst), leds)), ({0}))

#define KP_RGB_BEHAVIOR_DEFINE(inst)                                           \
  BUILD_ASSERT(sizeof(DEVICE_DT_NAME(DT_DRV_INST(inst))) <= 9,                 \
               "keypaw,behavior-rgb-matrix: node name must fit the 9-byte "    \
               "split behavior_dev field");                                    \
  BUILD_ASSERT(                                                                \
      DT_CHILD_NUM(DT_DRV_INST(inst)) <= KP_RGB_PERSIST_MAX_EFFECTS,           \
      "raise KP_RGB_PERSIST_MAX_EFFECTS for the larger effect registry");      \
  BUILD_ASSERT(DT_CHILD_NUM(DT_DRV_INST(inst)) <= UINT8_MAX,                   \
               "effect count must fit the blob's count byte");                 \
  KP_RGB_BEHAVIOR_INDICATORS(inst)                                             \
  KP_RGB_BEHAVIOR_LEDS(inst);                                                  \
  static const struct device *const kp_rgb_effects_##inst[KP_RGB_MAX_EFFECTS(  \
      inst)] = {DT_FOREACH_CHILD(DT_DRV_INST(inst), KP_RGB_EFFECT_DEVICE)};    \
  static const uint8_t kp_rgb_effect_no_cycle_##inst[KP_RGB_MAX_EFFECTS(       \
      inst)] = {DT_FOREACH_CHILD(DT_DRV_INST(inst), KP_RGB_NO_CYCLE_ONE)};     \
  static struct kp_rgb_behavior_context kp_rgb_context_##inst = {              \
      .effects = kp_rgb_effects_##inst,                                        \
      .no_cycle = kp_rgb_effect_no_cycle_##inst,                               \
      .effect_count = DT_CHILD_NUM(DT_DRV_INST(inst)),                         \
      .leds = kp_rgb_leds_##inst,                                              \
      .leds_len = DT_PROP_LEN_OR(DT_DRV_INST(inst), leds, 0),                  \
      .all_leds = !DT_NODE_HAS_PROP(DT_DRV_INST(inst), leds),                  \
      .indicators =                                                            \
          COND_CODE_1(DT_NODE_HAS_PROP(DT_DRV_INST(inst), indicators),         \
                      (kp_rgb_indicators_##inst), (kp_rgb_no_indicators)),     \
      .indicators_len = DT_PROP_LEN_OR(DT_DRV_INST(inst), indicators, 0),      \
      .zone_valid = true,                                                      \
      .tuning =                                                                \
          {                                                                    \
              .max_brightness =                                                \
                  DT_PROP_OR(DT_DRV_INST(inst), max_brightness, 40),           \
              .idle_brightness =                                               \
                  DT_PROP_OR(DT_DRV_INST(inst), idle_brightness, 30),          \
          },                                                                   \
  };                                                                           \
  static int kp_rgb_behavior_init_##inst(const struct device *dev) {           \
    struct kp_rgb_behavior_context *ctx = &kp_rgb_context_##inst;              \
    ctx->dev = dev;                                                            \
    ctx->state.on = DT_PROP_OR(DT_DRV_INST(inst), initial_on, 1);              \
    ctx->state.user_on = ctx->state.on;                                        \
    uint8_t brightness =                                                       \
        (uint8_t)CLAMP(DT_PROP_OR(DT_DRV_INST(inst), initial_brightness, 30),  \
                       0, KP_RGB_BRT_MAX);                                     \
    int32_t default_duration =                                                 \
        CLAMP(DT_PROP_OR(DT_DRV_INST(inst), initial_duration_ms, 1000),        \
              CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS,                        \
              CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);                       \
    for (size_t i = 0; i < ctx->effect_count; i++) {                           \
      const struct device *fx = ctx->effects[i];                               \
      if (fx == NULL) {                                                        \
        continue;                                                              \
      }                                                                        \
      struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(fx);         \
      data->color.b = brightness;                                              \
      if (data->duration_ms == 0) {                                            \
        data->duration_ms = (uint16_t)default_duration;                        \
      }                                                                        \
    }                                                                          \
    ctx->effect_index = DT_PROP_OR(DT_DRV_INST(inst), initial_effect, 0);      \
    if (kp_rgb_resolve_active(ctx) < 0) {                                      \
      LOG_WRN("Initial RGB effect unavailable for %s", dev->name);             \
    }                                                                          \
    LOG_DBG("Registered %u RGB matrix effects for %s",                         \
            (uint32_t)ctx->effect_count, dev->name);                           \
    return 0;                                                                  \
  }                                                                            \
  BEHAVIOR_DT_INST_DEFINE(inst, kp_rgb_behavior_init_##inst, NULL,              \
                          &kp_rgb_context_##inst, NULL, POST_KERNEL,          \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                  \
                          &behavior_rgb_matrix_driver_api);

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

static const struct behavior_driver_api behavior_rgb_matrix_driver_api;

static struct kp_rgb_behavior_context *
kp_rgb_context_from_binding(const struct zmk_behavior_binding *binding) {
  const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
  return kp_rgb_behavior_context_from_device(dev);
}

#define KP_RGB_CONTEXT_PTR(inst) &kp_rgb_context_##inst,
DT_INST_FOREACH_STATUS_OKAY(KP_RGB_BEHAVIOR_DEFINE)
static struct kp_rgb_behavior_context *const kp_rgb_contexts[] = {
    DT_INST_FOREACH_STATUS_OKAY(KP_RGB_CONTEXT_PTR)};

size_t kp_rgb_behavior_count(void) { return ARRAY_SIZE(kp_rgb_contexts); }

struct kp_rgb_behavior_context *kp_rgb_behavior_at(size_t index) {
  return index < ARRAY_SIZE(kp_rgb_contexts) ? kp_rgb_contexts[index] : NULL;
}

struct kp_rgb_behavior_context *
kp_rgb_behavior_context_from_device(const struct device *dev) {
  if (dev == NULL || dev->data == NULL) {
    return NULL;
  }
  struct kp_rgb_behavior_context *ctx = dev->data;
  return ctx->dev == dev ? ctx : NULL;
}

size_t kp_rgb_effect_count(const struct kp_rgb_behavior_context *ctx) {
  return ctx == NULL ? 0 : ctx->effect_count;
}

const struct device *kp_rgb_effect_at(const struct kp_rgb_behavior_context *ctx,
                                      size_t index) {
  return ctx != NULL && index < ctx->effect_count ? ctx->effects[index] : NULL;
}

size_t kp_rgb_selected_effect(const struct kp_rgb_behavior_context *ctx) {
  return ctx == NULL ? 0 : ctx->effect_index;
}

static bool kp_effect_cyclable(const struct kp_rgb_behavior_context *ctx,
                               size_t index) {
  return ctx != NULL && index < ctx->effect_count &&
         ctx->effects[index] != NULL && !ctx->no_cycle[index];
}

uint16_t kp_rgb_calc_effect_index(const struct kp_rgb_behavior_context *ctx,
                                  uint16_t current, int16_t delta) {
  if (ctx == NULL || ctx->effect_count == 0) {
    return current;
  }
  int16_t norm_delta = delta % (int16_t)ctx->effect_count;
  uint16_t start =
      (current + norm_delta + ctx->effect_count) % ctx->effect_count;
  if (kp_effect_cyclable(ctx, start)) {
    return start;
  }
  int direction = delta < 0 ? -1 : 1;
  for (size_t i = 1; i < ctx->effect_count; i++) {
    size_t index =
        (start + (i * direction) + ctx->effect_count) % ctx->effect_count;
    if (kp_effect_cyclable(ctx, index)) {
      return index;
    }
  }
  return current;
}

int kp_rgb_resolve_active(struct kp_rgb_behavior_context *ctx) {
  if (ctx == NULL || ctx->effect_count == 0) {
    return -ENOENT;
  }
  if (ctx->effect_index >= ctx->effect_count) {
    ctx->effect_index = 0;
  }
  ctx->effect_index = kp_rgb_calc_effect_index(ctx, ctx->effect_index, 0);
  const struct device *fx = ctx->effects[ctx->effect_index];
  if (fx == NULL) {
    ctx->state.active_fx = NULL;
    return -ENOENT;
  }
  ctx->state.active_fx = fx;
  return 0;
}

int kp_rgb_select_effect(struct kp_rgb_behavior_context *ctx, uint16_t index) {
  if (ctx == NULL || index >= ctx->effect_count) {
    return -EINVAL;
  }
  if (ctx->effects[index] == NULL) {
    return -ENOENT;
  }
  kp_rgb_matrix_lock();
  ctx->effect_index = index;
  ctx->state.active_fx = ctx->effects[index];
  kp_rgb_matrix_unlock();
  return 0;
}

int zmk_rgb_matrix_select_effect(const struct device *behavior,
                                 uint16_t index) {
  return kp_rgb_select_effect(kp_rgb_behavior_context_from_device(behavior),
                              index);
}

int zmk_rgb_matrix_cycle_effect(const struct device *behavior,
                                int16_t direction) {
  struct kp_rgb_behavior_context *ctx =
      kp_rgb_behavior_context_from_device(behavior);
  if (ctx == NULL) {
    return -ENODEV;
  }
  return kp_rgb_select_effect(
      ctx, kp_rgb_calc_effect_index(ctx, ctx->effect_index, direction));
}

int kp_rgb_calc_effect(struct kp_rgb_behavior_context *ctx, int16_t direction) {
  return (int)kp_rgb_calc_effect_index(ctx, ctx->effect_index, direction);
}

static struct kp_rgb_hsb
kp_active_hsb(const struct kp_rgb_behavior_context *ctx) {
  if (ctx == NULL || ctx->state.active_fx == NULL) {
    return (struct kp_rgb_hsb){.h = 0,
                               .s = 0,
                               .b = ctx == NULL ? KP_RGB_BRT_MAX
                                                : ctx->tuning.max_brightness};
  }
  return kp_rgb_effect_data(ctx->state.active_fx)->color;
}

int kp_rgb_set_hsb(struct kp_rgb_behavior_context *ctx,
                   struct kp_rgb_hsb color) {
  if (ctx == NULL || ctx->state.active_fx == NULL) {
    return -ENODEV;
  }
  if (color.h > KP_RGB_HUE_MAX || color.s > KP_RGB_SAT_MAX ||
      color.b > KP_RGB_BRT_MAX) {
    return -EINVAL;
  }
  kp_rgb_matrix_lock();
  kp_rgb_effect_data(ctx->state.active_fx)->color = color;
  kp_rgb_matrix_unlock();
  return 0;
}

struct kp_rgb_hsb kp_rgb_calc_hue(const struct kp_rgb_behavior_context *ctx,
                                  int8_t direction) {
  struct kp_rgb_hsb color = kp_active_hsb(ctx);
  color.h = (color.h + KP_RGB_HUE_MAX +
             (int32_t)direction * CONFIG_KEYPAW_RGB_MATRIX_HUE_STEP) %
            KP_RGB_HUE_MAX;
  return color;
}

struct kp_rgb_hsb kp_rgb_calc_sat(const struct kp_rgb_behavior_context *ctx,
                                  int8_t direction) {
  struct kp_rgb_hsb color = kp_active_hsb(ctx);
  color.s = (uint8_t)CLAMP((int)color.s + (int)direction *
                                              CONFIG_KEYPAW_RGB_MATRIX_SAT_STEP,
                           0, KP_RGB_SAT_MAX);
  return color;
}

struct kp_rgb_hsb kp_rgb_calc_brt(const struct kp_rgb_behavior_context *ctx,
                                  int8_t direction) {
  struct kp_rgb_hsb color = kp_active_hsb(ctx);
  color.b = (uint8_t)CLAMP((int)color.b + (int)direction *
                                              CONFIG_KEYPAW_RGB_MATRIX_BRT_STEP,
                           0, KP_RGB_BRT_MAX);
  return color;
}

int kp_rgb_change_hue(struct kp_rgb_behavior_context *ctx, int8_t direction) {
  return kp_rgb_set_hsb(ctx, kp_rgb_calc_hue(ctx, direction));
}
int kp_rgb_change_sat(struct kp_rgb_behavior_context *ctx, int8_t direction) {
  return kp_rgb_set_hsb(ctx, kp_rgb_calc_sat(ctx, direction));
}
int kp_rgb_change_brt(struct kp_rgb_behavior_context *ctx, int8_t direction) {
  return kp_rgb_set_hsb(ctx, kp_rgb_calc_brt(ctx, direction));
}

static uint32_t kp_active_duration(const struct kp_rgb_behavior_context *ctx) {
  return ctx == NULL || ctx->state.active_fx == NULL
             ? 0
             : kp_rgb_effect_period(ctx->state.active_fx);
}

int kp_rgb_set_duration(struct kp_rgb_behavior_context *ctx,
                        uint32_t duration_ms) {
  if (ctx == NULL || ctx->state.active_fx == NULL) {
    return -ENODEV;
  }
  int32_t duration = CLAMP((int32_t)duration_ms,
                           (int32_t)CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS,
                           (int32_t)CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
  kp_rgb_matrix_lock();
  kp_rgb_effect_data(ctx->state.active_fx)->duration_ms = (uint16_t)duration;
  kp_rgb_matrix_unlock();
  return 0;
}

int kp_rgb_change_duration(struct kp_rgb_behavior_context *ctx,
                           int16_t direction) {
  int32_t next = (int32_t)kp_active_duration(ctx) +
                 (int32_t)direction * CONFIG_KEYPAW_RGB_MATRIX_DURATION_STEP;
  return kp_rgb_set_duration(ctx, (uint32_t)MAX(next, 0));
}

int kp_rgb_effect_convert_central_state_dependent_params(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event) {
  ARG_UNUSED(event);
  const struct device *fx_dev = zmk_behavior_get_binding(binding->behavior_dev);
  if (fx_dev == NULL) {
    return -ENODEV;
  }
  const struct kp_rgb_effect_api *api =
      (const struct kp_rgb_effect_api *)fx_dev->api;
  const struct kp_rgb_effect_common_config *cfg = kp_rgb_effect_cfg(fx_dev);
  if (api->owner == NULL || cfg == NULL) {
    return -ENODEV;
  }
  binding->behavior_dev = api->owner->name;
  binding->param1 = RGB_EFS_CMD;
  binding->param2 = cfg->index;
  return 0;
}

static int on_keymap_binding_convert_central_state_dependent_params(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event) {
  ARG_UNUSED(event);
  struct kp_rgb_behavior_context *ctx = kp_rgb_context_from_binding(binding);
  if (ctx == NULL) {
    return -ENODEV;
  }
  switch (binding->param1) {
  case RGB_TOG_CMD: {
    bool state;
    int err = zmk_rgb_matrix_get_state(ctx->dev, &state);
    if (err)
      return err;
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
      color = kp_rgb_calc_brt(ctx, 1);
      break;
    case RGB_BRD_CMD:
      color = kp_rgb_calc_brt(ctx, -1);
      break;
    case RGB_HUI_CMD:
      color = kp_rgb_calc_hue(ctx, 1);
      break;
    case RGB_HUD_CMD:
      color = kp_rgb_calc_hue(ctx, -1);
      break;
    case RGB_SAI_CMD:
      color = kp_rgb_calc_sat(ctx, 1);
      break;
    default:
      color = kp_rgb_calc_sat(ctx, -1);
      break;
    }
    binding->param1 = RGB_COLOR_HSB_CMD;
    binding->param2 = RGB_COLOR_HSB_VAL(color.h, color.s, color.b);
    break;
  }
  case RGB_EFR_CMD:
    binding->param1 = RGB_EFS_CMD;
    binding->param2 = (uint32_t)kp_rgb_calc_effect(ctx, -1);
    break;
  case RGB_EFF_CMD:
    binding->param1 = RGB_EFS_CMD;
    binding->param2 = (uint32_t)kp_rgb_calc_effect(ctx, 1);
    break;
  case RGB_SPI_CMD:
  case RGB_SPD_CMD: {
    int32_t step = binding->param1 == RGB_SPI_CMD ? 1 : -1;
    int32_t duration =
        CLAMP((int32_t)kp_active_duration(ctx) +
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
  struct kp_rgb_behavior_context *ctx = kp_rgb_context_from_binding(binding);
  if (ctx == NULL)
    return -ENODEV;
  int ret;
  switch (binding->param1) {
  case RGB_TOG_CMD:
    kp_rgb_matrix_lock();
    ctx->state.user_on = !ctx->state.on;
    bool toggle_on = !ctx->state.on;
    kp_rgb_matrix_unlock();
    ret =
        toggle_on ? zmk_rgb_matrix_on(ctx->dev) : zmk_rgb_matrix_off(ctx->dev);
    break;
  case RGB_ON_CMD:
    kp_rgb_matrix_lock();
    ctx->state.user_on = true;
    kp_rgb_matrix_unlock();
    ret = zmk_rgb_matrix_on(ctx->dev);
    break;
  case RGB_OFF_CMD:
    kp_rgb_matrix_lock();
    ctx->state.user_on = false;
    kp_rgb_matrix_unlock();
    ret = zmk_rgb_matrix_off(ctx->dev);
    break;
  case RGB_HUI_CMD:
    ret = kp_rgb_change_hue(ctx, 1);
    break;
  case RGB_HUD_CMD:
    ret = kp_rgb_change_hue(ctx, -1);
    break;
  case RGB_SAI_CMD:
    ret = kp_rgb_change_sat(ctx, 1);
    break;
  case RGB_SAD_CMD:
    ret = kp_rgb_change_sat(ctx, -1);
    break;
  case RGB_BRI_CMD:
    ret = kp_rgb_change_brt(ctx, 1);
    break;
  case RGB_BRD_CMD:
    ret = kp_rgb_change_brt(ctx, -1);
    break;
  case RGB_SPI_CMD:
    ret = binding->param2 != 0 ? kp_rgb_set_duration(ctx, binding->param2)
                               : kp_rgb_change_duration(ctx, 1);
    break;
  case RGB_SPD_CMD:
    ret = kp_rgb_change_duration(ctx, -1);
    break;
  case RGB_EFS_CMD:
    ret = kp_rgb_select_effect(ctx, (uint16_t)binding->param2);
    break;
  case RGB_EFF_CMD:
    ret = zmk_rgb_matrix_cycle_effect(ctx->dev, 1);
    break;
  case RGB_EFR_CMD:
    ret = zmk_rgb_matrix_cycle_effect(ctx->dev, -1);
    break;
  case RGB_COLOR_HSB_CMD:
    ret = kp_rgb_set_hsb(
        ctx, (struct kp_rgb_hsb){.h = (binding->param2 >> 16) & 0xFFFF,
                                 .s = (binding->param2 >> 8) & 0xFF,
                                 .b = binding->param2 & 0xFF});
    break;
  case RGB_IND_STATE_CMD:
    /* Live indicator state pushed by the central, not a setting. Apply it and
     * return before the kp_rgb_save_state() that closes the switch, so a layer
     * change never schedules a flash write. */
    kp_rgb_indicator_set_word(RGB_IND_STATE_WORD(binding->param2),
                              RGB_IND_STATE_BITS(binding->param2));
    return 0;
  default:
    return -ENOTSUP;
  }
  if (ret < 0)
    return ret;
  kp_rgb_save_state(ctx);
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

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
