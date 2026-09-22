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

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/keymap.h>

#include <zephyr/devicetree.h>
#include <zephyr/sys/util_macro.h>

/* The engine's devicetree node. Effects are separate device instances (their
 * own DT_DRV_COMPAT), so they cannot use DT_DRV_INST() to reach the strip: an
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

/* Compile-time 0xRRGGBB -> struct kp_rgb_hsb initializer, so an effect's preset
 * colour can stay a devicetree constant while the effect state holds HSB (see
 * kp_rgb_effect_common_data). This mirrors the runtime conversion this file
 * used to expose, down to the truncating division, so a given `color` property
 * yields the same preset either way. */
#define KP_RGB_HEX_R(hex) ((int)(((uint32_t)(hex) >> 16) & 0xFFu))
#define KP_RGB_HEX_G(hex) ((int)(((uint32_t)(hex) >> 8) & 0xFFu))
#define KP_RGB_HEX_B(hex) ((int)((uint32_t)(hex) & 0xFFu))

#define KP_RGB_HEX_MAX2(a, b) ((a) > (b) ? (a) : (b))
#define KP_RGB_HEX_MIN2(a, b) ((a) < (b) ? (a) : (b))
#define KP_RGB_HEX_MAX(hex)                                                    \
  KP_RGB_HEX_MAX2(KP_RGB_HEX_MAX2(KP_RGB_HEX_R(hex), KP_RGB_HEX_G(hex)),       \
                  KP_RGB_HEX_B(hex))
#define KP_RGB_HEX_MIN(hex)                                                    \
  KP_RGB_HEX_MIN2(KP_RGB_HEX_MIN2(KP_RGB_HEX_R(hex), KP_RGB_HEX_G(hex)),       \
                  KP_RGB_HEX_B(hex))
#define KP_RGB_HEX_DELTA(hex) (KP_RGB_HEX_MAX(hex) - KP_RGB_HEX_MIN(hex))

/* Adding KP_RGB_HUE_MAX before the modulo keeps the red-dominant term (the only
 * one that can go negative) in range; the green- and blue-dominant terms are
 * already in [60, 180] and [180, 300]. */
#define KP_RGB_HEX_HUE(hex)                                                    \
  (KP_RGB_HEX_DELTA(hex) == 0 ? 0                                              \
   : KP_RGB_HEX_MAX(hex) == KP_RGB_HEX_R(hex)                                  \
       ? ((60 * (KP_RGB_HEX_G(hex) - KP_RGB_HEX_B(hex)) /                      \
           KP_RGB_HEX_DELTA(hex)) +                                            \
          KP_RGB_HUE_MAX) %                                                    \
             KP_RGB_HUE_MAX                                                    \
   : KP_RGB_HEX_MAX(hex) == KP_RGB_HEX_G(hex)                                  \
       ? (120 + 60 * (KP_RGB_HEX_B(hex) - KP_RGB_HEX_R(hex)) /                 \
                    KP_RGB_HEX_DELTA(hex))                                     \
       : (240 + 60 * (KP_RGB_HEX_R(hex) - KP_RGB_HEX_G(hex)) /                 \
                    KP_RGB_HEX_DELTA(hex)))

#define KP_RGB_HEX_SAT(hex)                                                    \
  (KP_RGB_HEX_MAX(hex) == 0                                                    \
       ? 0                                                                     \
       : KP_RGB_HEX_DELTA(hex) * KP_RGB_SAT_MAX / KP_RGB_HEX_MAX(hex))
#define KP_RGB_HEX_BRT(hex) (KP_RGB_HEX_MAX(hex) * KP_RGB_BRT_MAX / 255)

#define KP_RGB_HSB_FROM_HEX(hex)                                               \
  {.h = (uint16_t)KP_RGB_HEX_HUE(hex),                                         \
   .s = (uint8_t)KP_RGB_HEX_SAT(hex),                                          \
   .b = (uint8_t)KP_RGB_HEX_BRT(hex)}

struct led_rgb kp_rgb_hsb_to_rgb(struct kp_rgb_hsb color);

/* -------------------------------------------------------------------------
 * Devicetree string-enum -> C enum helpers
 *
 * Usage (inside an effect .c, after `#define DT_DRV_COMPAT ...`):
 *
 *     DEFINE_DT_ENUM(axis, none, vertical, horizontal);
 *
 * The value list is the enum values in the binding YAML. The DT value is
 * converted into this enum with:
 *
 *     .axis = CONV_DT_ENUM(inst, axis),
 *
 * and compared in render code either against the bare constant or via
 * DT_ENUM_CONST, which keeps the property name for readability:
 *
 *     if (cfg->axis == DT_ENUM_CONST(axis, vertical)) { ... }
 * ------------------------------------------------------------------------- */

#define KP_ENUM_ENTRY(idx, val, prop)                                          \
  CONCAT(DT_DRV_COMPAT, _, prop, _, val) = idx
/* Define an enum with device-tree string enum values. */
#define DEFINE_DT_ENUM(prop, ...)                                              \
  typedef enum {                                                               \
    FOR_EACH_IDX_FIXED_ARG(KP_ENUM_ENTRY, (, ), prop, __VA_ARGS__),            \
  } CONCAT(prop, _t)

/* Resolve an instance's property to its named constant. */
#define CONV_DT_ENUM(inst, prop)                                               \
  CONCAT(DT_DRV_COMPAT, _, prop, _, DT_STRING_TOKEN(DT_DRV_INST(inst), prop))

/* Resolve a DT enum name to its named constant. */
#define DT_ENUM_CONST(prop, val) CONCAT(DT_DRV_COMPAT, _, prop, _, val)

/* Brightness helpers. Effects render at full scale and then dim the result, so
 * that a single global brightness setting applies uniformly.
 */
#define KP_RGB_SCALE(v, pct) ((uint8_t)((uint32_t)(v) * (pct) / 100u))
struct kp_rgb_hsb kp_rgb_hsb_scale(struct kp_rgb_hsb color, uint8_t pct);
struct led_rgb kp_rgb_rgb_scale(struct led_rgb rgb, uint8_t pct);

/* Blend `over` into `base` by `pct` percent (0 keeps base, 100 replaces it).
 * Used by indicators to composite over whatever the effect rendered. */
struct led_rgb kp_rgb_rgb_mix(struct led_rgb base, struct led_rgb over,
                              uint8_t pct);

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

/* Resolve a key position to the LED under it: the LED index, or SIZE_MAX when
 * the position has no LED. Effects need this from their on_event callback,
 * where no frame is available.
 *
 * At most one LED is resolved per position (the last `mapping` entry naming
 * that key wins), which matches the shields today; a key with several LEDs
 * underneath would need this API generalised. */
size_t kp_rgb_led_for_position(uint32_t position);

/* An LED's centre in this half's normalised layout units -- the same geometry
 * the frame publishes as `coords` -- or NULL when `led` is out of range. */
const struct kp_rgb_coord *kp_rgb_led_coord(size_t led);

struct kp_rgb_frame {
  const struct kp_rgb_tuning *tune;
  size_t count;
  const struct kp_rgb_coord *coords;
  struct led_rgb *pixels;
  uint32_t elapsed;
  /* Longest edge of this half's LEDs, in layout units (never 0). */
  uint16_t board_length;
  /* Vertical extent of this half's LEDs (max y - min y), in layout units
   * (never 0). Needed by vertical-basis effects (gradient up/down, cycle
   * up/down, chevron, radial extent). */
  uint16_t board_height;
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
  const struct device *owner; /* parent keypaw,behavior-rgb-matrix */
  /* Per-effect indicator override, filled in by KP_RGB_EFFECT_DEFINE from the
   * effect node's devicetree:
   *   indicators = <&a &b>;  -> exactly those, in order
   *   no-indicators;         -> kp_rgb_no_indicators (non-NULL, length 0)
   *   neither                -> NULL, inherit the owning behavior's list
   */
  const struct device *const *indicators;
  size_t indicators_len;
};

/* The brightness a renderer should apply this frame. */
static inline uint8_t kp_rgb_brightness_pct(const struct kp_rgb_frame *frame) {
  return frame->is_idle ? frame->tune->idle_brightness
                        : frame->tune->max_brightness;
}

int kp_rgb_effect_convert_central_state_dependent_params(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event);

struct kp_rgb_effect_common_config {
  uint16_t index; /* user-assigned, unique, stable across rebuilds */
};
struct kp_rgb_effect_common_data {
  uint16_t duration_ms;    /* single cycle animation duration */
  struct kp_rgb_hsb color; /* current color */
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

/* Expand one entry of an `indicators = <&a &b>;` list. The properties are of
 * type `phandles` (not `phandle-array`, which would demand #indicator-cells),
 * so DT_PROP_BY_IDX is the accessor that yields a node identifier. */
#define KP_RGB_INDICATORS_AT_IDX(idx, node_id)                                 \
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

#define KP_RGB_EFFECT_INDICATOR_PTR(node_id, cfg_inst)                         \
  COND_CODE_1(DT_PROP(node_id, no_indicators), (kp_rgb_no_indicators),         \
              (COND_CODE_1(DT_NODE_HAS_PROP(node_id, indicators),              \
                           (cfg_inst##_indicators), (NULL))))

#define KP_RGB_EFFECT_DEFINE(node_id, render_fn, event_fn, cfg_inst)           \
  BUILD_ASSERT(                                                                \
      DT_NODE_HAS_COMPAT(DT_PARENT(node_id), keypaw_behavior_rgb_matrix),      \
      "RGB effect must be a child of keypaw,behavior-rgb-matrix");             \
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
      .owner = DEVICE_DT_GET(DT_PARENT(node_id)),                              \
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
 * effect rendered. Indicators are listed in paint order by `indicators` on the
 * owning behavior node; individual effects may override that list.
 *
 * Kinds sample the state they need in `render` -- the engine repaints every
 * tick, so there is no need to subscribe to events.
 * ------------------------------------------------------------------------- */

struct kp_rgb_indicator_api {
  bool (*active)(const struct device *dev);
  void (*render)(const struct device *dev, struct kp_rgb_frame *frame);
};

/* Kind-agnostic part of an indicator's config; must be its first member. */
struct kp_rgb_indicator_common_config {
  const uint32_t *keys; /* key positions, or NULL */
  size_t keys_len;
  const uint32_t *leds; /* raw chain indices, or NULL */
  size_t leds_len;
  uint32_t color;     /* 24-bit RGB */
  uint8_t brightness; /* paint strength: 100 replaces, lower values mix */
};

/* Kind-agnostic mutable part; must be the first member. */
struct kp_rgb_indicator_common_data {
  size_t led_count; /* resolved targets; 0 when keys and leds are both empty */
  size_t *leds;     /* the indicator's own storage */
  uint16_t index;   /* registry ordinal; state word index / 16, bit index % 16 */
  bool remote;      /* True when `central-authoritative` */
};

/* One state bit per ordinal, in ceil(count / 16) uint16_t words. Sized from the
 * devicetree, so there is no fixed cap on the indicator count. */
#if DT_HAS_COMPAT_STATUS_OKAY(keypaw_rgb_indicators)
#define KP_RGB_INDICATOR_COUNT DT_CHILD_NUM(DT_INST(0, keypaw_rgb_indicators))
#else
#define KP_RGB_INDICATOR_COUNT 0
#endif
#define KP_RGB_INDICATOR_WORDS MAX(1, (KP_RGB_INDICATOR_COUNT + 15) / 16)

/* Add a device to the engine's indicator registry, where `index` places it.
 * Called by KP_RGB_INDICATOR_DEFINE; a kind never calls this itself. */
void kp_rgb_indicator_register(const struct device *dev);

/* The most targets an indicator can resolve. */
#define KP_RGB_INDICATOR_TARGET_CAP(node_id)                                   \
  MAX(1, MIN(KP_LED_COUNT, DT_PROP_LEN_OR(node_id, keys, 0) +                  \
                               DT_PROP_LEN_OR(node_id, leds, 0)))

/* Resolve an indicator's `keys`/`leds` devicetree spec into LED indices.
 * Returns the number written (never more than out_max, and always <=
 * KP_LED_COUNT); 0 when both lists are empty. */
size_t kp_rgb_resolve_targets(const uint32_t *keys, size_t keys_len,
                              const uint32_t *leds, size_t leds_len,
                              size_t *out, size_t out_max);

/* Paint `color` onto `leds` at `strength` percent: the colour is scaled by the
 * frame's brightness, then mixed over the existing pixels. */
void kp_rgb_indicator_paint(struct kp_rgb_frame *frame, const size_t *leds,
                            size_t led_count, struct led_rgb color,
                            uint8_t strength);

/* Declare the devicetree-derived target arrays for one indicator instance. A
 * missing property yields a one-element array whose length is read as 0, so the
 * declaration is always valid C and the array is always referenced. */
#define KP_RGB_INDICATOR_TARGET_ARRAYS(inst, cfg_inst)                         \
  static const uint32_t cfg_inst##_keys[] =                                    \
      COND_CODE_1(DT_NODE_HAS_PROP(DT_DRV_INST(inst), keys),                   \
                  (DT_PROP(DT_DRV_INST(inst), keys)), ({0}));                  \
  static const uint32_t cfg_inst##_leds[] =                                    \
      COND_CODE_1(DT_NODE_HAS_PROP(DT_DRV_INST(inst), leds),                   \
                  (DT_PROP(DT_DRV_INST(inst), leds)), ({0}))

/* Member-wise initializer for the common part of a kind's config. */
#define KP_RGB_INDICATOR_COMMON(node_id, cfg_inst)                             \
  {.keys = cfg_inst##_keys,                                                    \
   .keys_len = DT_PROP_LEN_OR(node_id, keys, 0),                               \
   .leds = cfg_inst##_leds,                                                    \
   .leds_len = DT_PROP_LEN_OR(node_id, leds, 0),                               \
   .color = DT_PROP_OR(node_id, color, 0xFFFFFF),                              \
   .brightness = DT_PROP_OR(node_id, brightness, 100)}

/* Declare the device. Indicators are plain devices, never behaviors, so they
 * stay out of the behavior registry and cannot be keymap-bound. The macro also
 * allocates the instance's target storage, so a kind must declare its device
 * here rather than with DEVICE_DT_DEFINE directly. */
#define KP_RGB_INDICATOR_DEFINE(inst, active_fn, render_fn, cfg_inst)          \
  BUILD_ASSERT(sizeof(cfg_inst##_cfg.common) ==                                \
                       sizeof(struct kp_rgb_indicator_common_config) &&        \
                   (const void *)&cfg_inst##_cfg ==                            \
                       (const void *)&cfg_inst##_cfg.common,                   \
               "indicator config must embed "                                  \
               "struct kp_rgb_indicator_common_config as the first field");    \
  BUILD_ASSERT(                                                                \
      sizeof(cfg_inst##_data.common) ==                                        \
              sizeof(struct kp_rgb_indicator_common_data) &&                   \
          (const void *)&cfg_inst##_data ==                                    \
              (const void *)&cfg_inst##_data.common,                           \
      "indicator data must embed struct kp_rgb_indicator_common_data "         \
      "as the first field");                                                   \
  BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_PARENT(DT_DRV_INST(inst)),                \
                                  keypaw_rgb_indicators),                      \
               "indicator must be a child of a keypaw,rgb-indicators node");   \
  static size_t                                                                \
      cfg_inst##_targets[KP_RGB_INDICATOR_TARGET_CAP(DT_DRV_INST(inst))];      \
  static int cfg_inst##_init(const struct device *dev) {                       \
    const struct kp_rgb_indicator_common_config *cfg = dev->config;            \
    struct kp_rgb_indicator_common_data *data = dev->data;                     \
    data->leds = cfg_inst##_targets;                                           \
    data->led_count = kp_rgb_resolve_targets(                                  \
        cfg->keys, cfg->keys_len, cfg->leds, cfg->leds_len, data->leds,        \
        ARRAY_SIZE(cfg_inst##_targets));                                       \
    data->index = (uint16_t)DT_NODE_CHILD_IDX(DT_DRV_INST(inst));              \
    data->remote = IS_ENABLED(CONFIG_ZMK_SPLIT) &&                             \
                   DT_PROP(DT_DRV_INST(inst), central_authoritative);          \
    kp_rgb_indicator_register(dev);                                            \
    return 0;                                                                  \
  }                                                                            \
  static const struct kp_rgb_indicator_api cfg_inst##_api = {                  \
      .active = active_fn,                                                     \
      .render = render_fn,                                                     \
  };                                                                           \
  /* Init resolves `keys` through the engine's key -> LED table, which the     \
   * engine fills from its own POST_KERNEL init at the OBJECTS priority, ahead \
   * of these devices' DEFAULT priority. */                                    \
  DEVICE_DT_DEFINE(DT_DRV_INST(inst), cfg_inst##_init, NULL, &cfg_inst##_data, \
                   &cfg_inst##_cfg, POST_KERNEL,                               \
                   CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &cfg_inst##_api)

/* -------------------------------------------------------------------------
 * Triggers
 *
 * A trigger is a non-behavior device holding a condition plus the `&kprgb`
 * bindings to invoke when that condition wins. Triggers are the children of a
 * keypaw,rgb-trigger-table node, and their declaration order is their
 * precedence: the first trigger whose condition is active wins, and its
 * bindings run only when the winner changes. That edge behaviour is what lets a
 * manual RGB_EFF/RGB_EFS survive until the mapping actually changes, and stops
 * relative commands (RGB_HUI, RGB_BRI, ...) from re-firing on unrelated layer
 * events.
 *
 * The table cannot be &kprgb itself (its children are the effect registry) and
 * cannot hold a phandle list of its own children (a node depends on its parent,
 * so that is a devicetree cycle), hence the separate node and declaration
 * order.
 *
 * Evaluation needs the keymap, so it runs on the split central only; the
 * emitted commands still reach both halves because &kprgb is
 * BEHAVIOR_LOCALITY_GLOBAL.
 * ------------------------------------------------------------------------- */

struct kp_rgb_trigger_api {
  bool (*active)(const struct device *dev);
};

/* Kind-agnostic part of a trigger's config; must be its first member. */
struct kp_rgb_trigger_common_config {
  const struct zmk_behavior_binding *bindings;
  size_t bindings_len;
};

/* Declare the binding array for one trigger instance. The property must be
 * named `bindings`: ZMK_KEYMAP_EXTRACT_BINDING hardcodes it, and each element
 * carries the target behavior's cells (two, for &kprgb). */
#define KP_RGB_TRIGGER_BINDING_ARRAY(inst, cfg_inst)                           \
  static const struct zmk_behavior_binding cfg_inst##_bindings[] = {           \
      LISTIFY(DT_PROP_LEN(DT_DRV_INST(inst), bindings),                        \
              ZMK_KEYMAP_EXTRACT_BINDING, (, ), DT_DRV_INST(inst))}

/* Member-wise initializer for the common part of a kind's config. */
#define KP_RGB_TRIGGER_COMMON(node_id, cfg_inst)                               \
  {.bindings = cfg_inst##_bindings,                                            \
   .bindings_len = DT_PROP_LEN(node_id, bindings)}

/* Declare the device. Triggers carry no mutable state, so they pass no data
 * pointer and need no init function. */
#define KP_RGB_TRIGGER_DEFINE(inst, active_fn, cfg_inst)                       \
  BUILD_ASSERT(sizeof(cfg_inst##_cfg.common) ==                                \
                       sizeof(struct kp_rgb_trigger_common_config) &&          \
                   (const void *)&cfg_inst##_cfg ==                            \
                       (const void *)&cfg_inst##_cfg.common,                   \
               "trigger config must embed "                                    \
               "struct kp_rgb_trigger_common_config as the first field");      \
  static const struct kp_rgb_trigger_api cfg_inst##_api = {                    \
      .active = active_fn,                                                     \
  };                                                                           \
  DEVICE_DT_DEFINE(DT_DRV_INST(inst), NULL, NULL, NULL, &cfg_inst##_cfg,       \
                   POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,           \
                   &cfg_inst##_api)

int zmk_rgb_matrix_toggle(const struct device *behavior);
int zmk_rgb_matrix_on(const struct device *behavior);
int zmk_rgb_matrix_off(const struct device *behavior);
int zmk_rgb_matrix_get_state(const struct device *behavior, bool *on_off);

int zmk_rgb_matrix_select_effect(const struct device *behavior,
                                 uint16_t effect);
int zmk_rgb_matrix_cycle_effect(const struct device *behavior,
                                int16_t direction);
