/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/sys/util.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <drivers/behavior.h>

/* The engine's devicetree node. Effects are separate device instances (their own
 * DT_DRV_COMPAT), so they cannot use DT_DRV_INST() to reach the strip: an
 * effect's own DT_DRV_INST(0) would resolve to the effect node. Spell the
 * compatible out instead, so any effect TU (in any module) can size its state
 * from the real LED count.
 */
#if DT_HAS_COMPAT_STATUS_OKAY(keypaw_rgb_matrix)
#define KP_RGB_NODE DT_INST(0, keypaw_rgb_matrix)
#define KP_RGB_STRIP DT_PHANDLE(KP_RGB_NODE, strip)
#define KP_LED_COUNT DT_PROP(KP_RGB_STRIP, chain_length)
#endif

#define KP_RGB_HUE_MAX 360
#define KP_RGB_SAT_MAX 100
#define KP_RGB_BRT_MAX 100
struct kp_rgb_hsb {
  uint16_t h; /* 0..360 */
  uint8_t s;  /* 0..100 */
  uint8_t b;  /* 0..100 */
};

struct led_rgb kp_rgb_hsb_to_rgb(struct kp_rgb_hsb color);
struct kp_rgb_hsb kp_rgb_hex_to_hsb(uint32_t hex);
uint32_t kp_rgb_hsb_to_hex(struct kp_rgb_hsb color);

/* Brightness helpers. Effects render at full scale and then dim the result, so
 * that a single global brightness setting applies uniformly.
 */
#define KP_RGB_SCALE(v, pct) ((uint8_t)((uint32_t)(v) * (pct) / 100u))
struct kp_rgb_hsb kp_rgb_hsb_scale(struct kp_rgb_hsb color, uint8_t pct);
struct led_rgb kp_rgb_rgb_scale(struct led_rgb rgb, uint8_t pct);

/* Blend `over` into `base` by `pct` percent (0 keeps base, 100 replaces it).
 * Used by indicators to composite over whatever the effect rendered. */
struct led_rgb kp_rgb_rgb_mix(struct led_rgb base, struct led_rgb over, uint8_t pct);

#define kp_hex_to_rgb(hex)                                                     \
  ((struct led_rgb){                                                           \
      .r = ((hex) >> 16) & 0xFF,                                               \
      .g = ((hex) >> 8) & 0xFF,                                                \
      .b = (hex) & 0xFF,                                                       \
  })

struct kp_rgb_tuning {
  uint8_t max_brightness;
  uint8_t idle_brightness;
};

struct kp_rgb_coord {
  uint16_t x;
  uint16_t y;
};

/* Resolve a key position to the LED under it: the LED index, or SIZE_MAX when the
 * position has no LED. Effects need this from their on_event callback, where no
 * frame is available.
 *
 * At most one LED is resolved per position (the last `mapping` entry naming that
 * key wins), which matches the shields today; a key with several LEDs underneath
 * would need this API generalised. */
size_t kp_rgb_led_for_position(uint32_t position);

/* An LED's centre in this half's normalised layout units -- the same geometry the
 * frame publishes as `coords` -- or NULL when `led` is out of range. */
const struct kp_rgb_coord *kp_rgb_led_coord(size_t led);

struct kp_rgb_frame {
  const struct kp_rgb_tuning *tune;
  size_t count;
  const struct kp_rgb_coord *coords;
  struct led_rgb *pixels;
  uint32_t elapsed;
  /* Longest edge of this half's LEDs, in layout units (never 0). */
  uint16_t board_length;
  /* The keyboard is not ZMK_ACTIVITY_ACTIVE. */
  bool is_idle;
};

typedef void (*rgb_matrix_effect_render_callback_t)(const struct device *dev,
                                                    struct kp_rgb_frame *frame);
typedef void (*rgb_matrix_effect_event_callback_t)(const struct device *dev,
                                                   const zmk_event_t *eh);

/* Sentinel list meaning "this effect wants no indicators at all"; it is never
 * dereferenced, since the accompanying length is 0. */
extern const struct device *const kp_rgb_no_indicators[];

struct kp_rgb_effect_api {
  const struct behavior_driver_api behavior; /* must be first */
  rgb_matrix_effect_render_callback_t render;
  rgb_matrix_effect_event_callback_t on_event;
  /* Per-effect indicator override, filled in by KP_RGB_EFFECT_DEFINE from the
   * effect node's devicetree:
   *   indicators = <&a &b>;  -> exactly those, in order
   *   no-indicators;         -> kp_rgb_no_indicators (non-NULL, length 0)
   *   neither                -> NULL, inherit the matrix-wide list
   */
  const struct device *const *indicators;
  size_t indicators_len;
};

/* The brightness a renderer should apply this frame. */
static inline uint8_t kp_rgb_brightness_pct(const struct kp_rgb_frame *frame) {
  return frame->is_idle ? frame->tune->idle_brightness : frame->tune->max_brightness;
}

int kp_rgb_effect_convert_central_state_dependent_params(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event);

struct kp_rgb_effect_common_config {
  uint16_t index;       /* user-assigned, unique, stable across rebuilds */
};
struct kp_rgb_effect_common_data {
  uint16_t duration_ms; /* single cycle animation duration */
  uint32_t color_hex;   /* preset default colour (0xRRGGBB) */
};

/* Both common structs must be the first member of the effect's own config/data:
 * the shared command handlers reach them through these casts. */
static inline struct kp_rgb_effect_common_data *
kp_rgb_effect_data(const struct device *dev) {
  return (struct kp_rgb_effect_common_data *)dev->data;
}
static inline const struct kp_rgb_effect_common_config *
kp_rgb_effect_cfg(const struct device *dev) {
  return (const struct kp_rgb_effect_common_config *)dev->config;
}

/* The effect's animation period in milliseconds. The behavior seeds a non-zero
 * duration into every registered effect at boot, so the guard only protects
 * against an effect that was somehow never registered. */
static inline uint32_t kp_rgb_effect_period(const struct device *dev) {
  return MAX(kp_rgb_effect_data(dev)->duration_ms, 1u);
}

/* Expand one entry of an `indicators = <&a &b>;` list. The properties are of type
 * `phandles` (not `phandle-array`, which would demand #indicator-cells), so
 * DT_PROP_BY_IDX is the accessor that yields a node identifier. */
#define KP_RGB_INDICATORS_AT_IDX(idx, node_id) \
  DEVICE_DT_GET(DT_PROP_BY_IDX(node_id, indicators, idx))

/* Declare the override list, but only when the node actually has one, so an
 * unused static array is never left behind. */
#define KP_RGB_EFFECT_INDICATOR_LIST(node_id, cfg_inst)                        \
  COND_CODE_1(                                                                 \
      DT_NODE_HAS_PROP(node_id, indicators),                                   \
      (static const struct device *const cfg_inst##_indicators[] = {LISTIFY(   \
           DT_PROP_LEN(node_id, indicators), KP_RGB_INDICATORS_AT_IDX, (, ),   \
           node_id)};),                                                        \
      ())

#define KP_RGB_EFFECT_INDICATOR_PTR(node_id, cfg_inst)                  \
  COND_CODE_1(DT_PROP(node_id, no_indicators), (kp_rgb_no_indicators),  \
              (COND_CODE_1(DT_NODE_HAS_PROP(node_id, indicators),       \
                           (cfg_inst##_indicators), (NULL))))

#define KP_RGB_EFFECT_DEFINE(node_id, render_fn, event_fn, cfg_inst)           \
  BUILD_ASSERT(sizeof(cfg_inst##_cfg.common) ==                                \
                       sizeof(struct kp_rgb_effect_common_config) &&           \
                   (const void *)&cfg_inst##_cfg ==                            \
                       (const void *)&cfg_inst##_cfg.common,                   \
               "effect config must embed struct kp_rgb_effect_common_config "  \
               "as the first field");                                          \
  BUILD_ASSERT(sizeof(cfg_inst##_data.common) ==                               \
                       sizeof(struct kp_rgb_effect_common_data) &&             \
                   (const void *)&cfg_inst##_data ==                           \
                       (const void *)&cfg_inst##_data.common,                  \
               "effect data must embed struct kp_rgb_effect_common_data "      \
               "as the first field");                                          \
  BUILD_ASSERT(DT_PROP(node_id, index) == DT_NODE_CHILD_IDX(node_id),          \
               "effect index must match the ordering of effects");             \
  BUILD_ASSERT(DT_PROP_LEN_OR(node_id, indicators, 1) > 0,                     \
               "an empty `indicators` list is not expressible; use "           \
               "`no-indicators;` for an effect that wants none");              \
  KP_RGB_EFFECT_INDICATOR_LIST(node_id, cfg_inst)                              \
  static int cfg_inst##_init(const struct device *dev) { return 0; }           \
  IF_ENABLED(                                                                  \
      CONFIG_ZMK_BEHAVIOR_METADATA,                                            \
      (static const struct behavior_parameter_metadata cfg_inst##_metadata = { \
           .sets_len = 0,                                                      \
       };))                                                                    \
  static const struct kp_rgb_effect_api cfg_inst##_api = {                     \
      .behavior = {.locality = BEHAVIOR_LOCALITY_GLOBAL,                       \
                   .binding_convert_central_state_dependent_params =           \
                       kp_rgb_effect_convert_central_state_dependent_params,   \
                   .binding_pressed = NULL,                                    \
                   .binding_released = NULL,                                   \
                   IF_ENABLED(                                                 \
                       CONFIG_ZMK_BEHAVIOR_METADATA,                           \
                       (.parameter_metadata = &cfg_inst##_metadata, ))},       \
      .render = render_fn,                                                     \
      .on_event = event_fn,                                                    \
      .indicators = KP_RGB_EFFECT_INDICATOR_PTR(node_id, cfg_inst),            \
      .indicators_len = DT_PROP_LEN_OR(node_id, indicators, 0),                \
  };                                                                           \
  BEHAVIOR_DT_DEFINE(node_id, cfg_inst##_init, NULL, &cfg_inst##_data,         \
                     &cfg_inst##_cfg, POST_KERNEL,                             \
                     CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &cfg_inst##_api)

/* -------------------------------------------------------------------------
 * Indicators
 *
 * An indicator is a non-behavior device that paints over whatever the active
 * effect rendered, so status (Caps Lock, a layer, battery, ...) can be shown
 * without the effects knowing anything about it. Indicators are listed in
 * paint order by `indicators = <&a &b>;` on the engine node, which individual
 * effects may override.
 *
 * Kinds sample the state they need in `render` -- the engine repaints every
 * tick, so there is no need to subscribe to events.
 * ------------------------------------------------------------------------- */

struct kp_rgb_indicator_api {
  void (*render)(const struct device *dev, struct kp_rgb_frame *frame);
};

/* Kind-agnostic part of an indicator's config; must be its first member. */
struct kp_rgb_indicator_common_config {
  const uint32_t *keys; /* key positions, or NULL */
  size_t keys_len;
  const uint32_t *leds; /* raw chain indices, or NULL */
  size_t leds_len;
  uint32_t color;       /* 24-bit RGB */
  uint8_t brightness;   /* paint strength: 100 replaces, lower values mix */
};

/* Kind-agnostic mutable part; must be the first member. */
struct kp_rgb_indicator_common_data {
  size_t led_count; /* targets resolved from keys/leds; KP_LED_COUNT if neither */
  size_t leds[KP_LED_COUNT];
};

/* Resolve an indicator's `keys`/`leds` devicetree spec into LED indices. With
 * neither given, every LED of this half is targeted. Returns the number written
 * (never more than out_max, and always <= KP_LED_COUNT). */
size_t kp_rgb_resolve_targets(const uint32_t *keys, size_t keys_len,
                              const uint32_t *leds, size_t leds_len, size_t *out,
                              size_t out_max);

/* Paint `color` onto `leds` at `strength` percent: the colour is scaled by the
 * frame's brightness, then mixed over the existing pixels. */
void kp_rgb_indicator_paint(struct kp_rgb_frame *frame, const size_t *leds,
                            size_t led_count, struct led_rgb color, uint8_t strength);

/* Declare the devicetree-derived target arrays for one indicator instance. A
 * missing property yields a one-element array whose length is read as 0, so the
 * declaration is always valid C and the array is always referenced. */
#define KP_RGB_INDICATOR_TARGET_ARRAYS(inst, cfg_inst)                          \
  static const uint32_t cfg_inst##_keys[] =                                    \
      COND_CODE_1(DT_NODE_HAS_PROP(DT_DRV_INST(inst), keys),                    \
                  (DT_PROP(DT_DRV_INST(inst), keys)), ({0}));                   \
  static const uint32_t cfg_inst##_leds[] =                                    \
      COND_CODE_1(DT_NODE_HAS_PROP(DT_DRV_INST(inst), leds),                    \
                  (DT_PROP(DT_DRV_INST(inst), leds)), ({0}))

/* Member-wise initializer for the common part of a kind's config. */
#define KP_RGB_INDICATOR_COMMON(node_id, cfg_inst)                              \
  {.keys = cfg_inst##_keys,                                                     \
   .keys_len = DT_PROP_LEN_OR(node_id, keys, 0),                                \
   .leds = cfg_inst##_leds,                                                     \
   .leds_len = DT_PROP_LEN_OR(node_id, leds, 0),                                \
   .color = DT_PROP_OR(node_id, color, 0xFFFFFF),                              \
   .brightness = DT_PROP_OR(node_id, brightness, 100)}

/* Declare the device. Indicators are plain devices, never behaviors, so they
 * stay out of the behavior registry and cannot be keymap-bound. */
#define KP_RGB_INDICATOR_DEFINE(inst, render_fn, cfg_inst)                      \
  BUILD_ASSERT(sizeof(cfg_inst##_cfg.common) ==                                \
                       sizeof(struct kp_rgb_indicator_common_config) &&         \
                   (const void *)&cfg_inst##_cfg ==                            \
                       (const void *)&cfg_inst##_cfg.common,                   \
               "indicator config must embed "                                  \
               "struct kp_rgb_indicator_common_config as the first field");    \
  BUILD_ASSERT(sizeof(cfg_inst##_data.common) ==                               \
                       sizeof(struct kp_rgb_indicator_common_data) &&           \
                   (const void *)&cfg_inst##_data ==                           \
                       (const void *)&cfg_inst##_data.common,                  \
               "indicator data must embed struct kp_rgb_indicator_common_data " \
               "as the first field");                                          \
  static int cfg_inst##_init(const struct device *dev) {                       \
    const struct kp_rgb_indicator_common_config *cfg = dev->config;            \
    struct kp_rgb_indicator_common_data *data = dev->data;                     \
    data->led_count = kp_rgb_resolve_targets(cfg->keys, cfg->keys_len,         \
                                             cfg->leds, cfg->leds_len,         \
                                             data->leds, KP_LED_COUNT);        \
    return 0;                                                                  \
  }                                                                            \
  static const struct kp_rgb_indicator_api cfg_inst##_api = {                  \
      .render = render_fn,                                                     \
  };                                                                           \
  DEVICE_DT_DEFINE(DT_DRV_INST(inst), cfg_inst##_init, NULL, &cfg_inst##_data, \
                   &cfg_inst##_cfg, POST_KERNEL,                               \
                   CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &cfg_inst##_api)

int zmk_rgb_matrix_toggle(void);
int zmk_rgb_matrix_on(void);
int zmk_rgb_matrix_off(void);
int zmk_rgb_matrix_get_state(bool *on_off);

int zmk_rgb_matrix_select_effect(uint16_t effect);
int zmk_rgb_matrix_cycle_effect(int16_t direction);
