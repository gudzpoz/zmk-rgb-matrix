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

#include <zmk/rgb_color.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>

#include <zephyr/devicetree.h>
#include <zephyr/sys/util_macro.h>

/* The engine's devicetree node, spelled out so an effect TU can size its state
 * from the real LED count: DT_DRV_INST() would resolve to the effect's node. */
#if DT_HAS_COMPAT_STATUS_OKAY(keypaw_rgb_matrix)
#define KP_RGB_NODE DT_INST(0, keypaw_rgb_matrix)
#define KP_RGB_HAS_STRIP DT_NODE_HAS_PROP(KP_RGB_NODE, strip)
#if KP_RGB_HAS_STRIP
#define KP_RGB_STRIP DT_PHANDLE(KP_RGB_NODE, strip)
#define KP_LED_COUNT DT_PROP(KP_RGB_STRIP, chain_length)
#else
#define KP_LED_COUNT 0
#endif
#endif

/* -------------------------------------------------------------------------
 * Devicetree string-enum -> C enum helpers
 *
 *     DEFINE_DT_ENUM(axis_t, axis, none, vertical, horizontal);
 *     .axis = CONV_DT_ENUM(inst, axis),
 *     if (cfg->axis == DT_ENUM_CONST(axis, vertical)) { ... }
 *
 * The value list is the binding YAML's enum, in order. See
 * docs/extending.md#devicetree-string-enum-helper.
 * ------------------------------------------------------------------------- */

#define KP_ENUM_ENTRY(idx, val, prop)                                          \
  CONCAT(DT_DRV_COMPAT, _, prop, _, val) = idx
/* Define an enum with device-tree string enum values. */
#define DEFINE_DT_ENUM(type_name, prop, ...)                                   \
  typedef enum {                                                               \
    FOR_EACH_IDX_FIXED_ARG(KP_ENUM_ENTRY, (, ), prop, __VA_ARGS__),            \
  } type_name

/* Resolve an instance's property to its named constant. */
#define CONV_DT_ENUM(inst, prop)                                               \
  CONCAT(DT_DRV_COMPAT, _, prop, _, DT_STRING_TOKEN(DT_DRV_INST(inst), prop))

/* Resolve a DT enum name to its named constant. */
#define DT_ENUM_CONST(prop, val) CONCAT(DT_DRV_COMPAT, _, prop, _, val)

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

/* A animation frame to be rendered. Units are all in layout units (standard key
 * width = standard key height = 100). */
struct kp_rgb_frame {
  const struct kp_rgb_tuning *tune;
  /* The number of LEDs on this split half / this non-split keyboard, and also
   * the length of `coords` and `pixels` */
  size_t count;
  /* Each LED's center coordinates, in layout units */
  const struct kp_rgb_coord *coords;
  /* Output pixels for each LED */
  struct led_rgb *pixels;
  /* Milliseconds since the previous rendering tick or output resume. Excludes
   * time spent OFF, idle-suppressed, or inhibited. An immediate flush may make
   * this shorter than CONFIG_KEYPAW_RGB_MATRIX_TICK_MS. */
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

/* Use ev->state to distinguish presses and releases, and
 * kp_rgb_led_for_position() to find the LED. Runs under the matrix lock on the
 * low-priority queue. Feedback is admitted only with initialized local LEDs,
 * logical ON, and inhibited=false. Pending feedback is discarded on OFF, idle
 * suppression, or inhibition; a full queue also drops events,
 * so press/release pairs are not guaranteed. An in-flight pass may finish. */
typedef void (*rgb_matrix_effect_event_callback_t)(
    const struct device *dev, const struct zmk_position_state_changed *ev);

/* Sentinel list meaning "this effect wants no overlays at all"; it is never
 * dereferenced, since the accompanying length is 0. */
extern const struct device *const kp_rgb_no_overlays[];

struct kp_rgb_effect_api {
  const struct behavior_driver_api behavior;
  rgb_matrix_effect_render_callback_t render;
  rgb_matrix_effect_event_callback_t on_event;
  /* Per-effect overlay override, filled in by KP_RGB_EFFECT_DEFINE from the
   * effect node's devicetree:
   *   overlays = <&a &b>;  -> exactly those, in order
   *   no-overlays;         -> kp_rgb_no_overlays (non-NULL, length 0)
   *   neither                -> NULL, every registered overlay in
   *                             keypaw,rgb-overlays declaration order
   */
  const struct device *const *overlays;
  size_t overlays_len;
};

BUILD_ASSERT(offsetof(struct kp_rgb_effect_api, behavior) == 0,
             "kp_rgb_effect_api.behavior must be first: the effect api is "
             "reached as a struct behavior_driver_api *");

/* The brightness a renderer should apply this frame. */
static inline uint8_t kp_rgb_brightness_pct(const struct kp_rgb_frame *frame) {
  return frame->is_idle ? frame->tune->idle_brightness
                        : frame->tune->max_brightness;
}

int kp_rgb_effect_convert_central_state_dependent_params(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event);

struct kp_rgb_effect_common_config {
  uint16_t index; /* registry slot; the effect's child position */
};
struct kp_rgb_effect_common_data {
  uint16_t duration_ms;    /* single cycle animation duration */
  struct kp_rgb_hsb color; /* current color */
};

/* The shared command handlers reach the effect's own config/data through these
 * casts. */
static inline struct kp_rgb_effect_common_data *
kp_rgb_effect_data(const struct device *dev) {
  return (struct kp_rgb_effect_common_data *)dev->data;
}
static inline const struct kp_rgb_effect_common_config *
kp_rgb_effect_cfg(const struct device *dev) {
  return (const struct kp_rgb_effect_common_config *)dev->config;
}

/* The period `duration = 0` falls back to, for an effect the owner never seeded
 * (a private effect nested under an overlay). See docs/development.md. */
#define KP_RGB_EFFECT_PERIOD_FALLBACK_MS 1000u

/* The effect's animation period in milliseconds; never 0, so an effect can use
 * it as a modulus without a guard. */
static inline uint32_t kp_rgb_effect_period(const struct device *dev) {
  uint32_t duration = kp_rgb_effect_data(dev)->duration_ms;
  return duration != 0 ? duration : KP_RGB_EFFECT_PERIOD_FALLBACK_MS;
}

/* Expand one entry of an `overlays = <&a &b>;` list. */
#define KP_RGB_OVERLAYS_AT_IDX(idx, node_id)                                   \
  DEVICE_DT_GET(DT_PROP_BY_IDX(node_id, overlays, idx))

/* Repeated phandles produce duplicate enumerators and fail the build. */
#define KP_RGB_OVERLAY_UNIQUE(idx, node_id, cfg_inst)                          \
  CONCAT(cfg_inst, _overlay_, DT_DEP_ORD(DT_PROP_BY_IDX(node_id, overlays, idx)))
#define KP_RGB_EFFECT_OVERLAY_LIST(node_id, cfg_inst)                          \
  COND_CODE_1(                                                                 \
      DT_NODE_HAS_PROP(node_id, overlays),                                     \
      (enum {LISTIFY(DT_PROP_LEN(node_id, overlays), KP_RGB_OVERLAY_UNIQUE,    \
                     (, ), node_id, cfg_inst)};                                \
       static const struct device *const cfg_inst##_overlays[] = {LISTIFY(     \
           DT_PROP_LEN(node_id, overlays), KP_RGB_OVERLAYS_AT_IDX, (, ),       \
           node_id)};),                                                        \
      ())

#define KP_RGB_EFFECT_OVERLAY_PTR(node_id, cfg_inst)                           \
  COND_CODE_1(DT_PROP(node_id, no_overlays), (kp_rgb_no_overlays),             \
              (COND_CODE_1(DT_NODE_HAS_PROP(node_id, overlays),                \
                           (cfg_inst##_overlays), (NULL))))

/* An effect's registry slot: its position among the owning behavior's children.
 * Declaration order is the identity; the shared convert hook reads the baked
 * copy, since it has no `inst` in scope. */
#define KP_RGB_EFFECT_INDEX(inst) DT_NODE_CHILD_IDX(DT_DRV_INST(inst))

/* An effect plays one of two roles, chosen by its parent:
 *
 *   - child of keypaw,behavior-rgb-matrix -> *registry* effect: selectable,
 *     cyclable, persisted, split-addressed, keymap-bindable, seeded with the
 *     behavior's boot defaults.
 *   - child of keypaw,rgb-overlay         -> *private* effect: a compositor's
 *     renderer. No registry slot, no identity, no persistence.
 */
#define KP_RGB_EFFECT_IS_REGISTRY(node_id)                                     \
  DT_NODE_HAS_COMPAT(DT_PARENT(node_id), keypaw_behavior_rgb_matrix)

/* Reject an effect whose parent is neither, and registry-only properties on a
 * private effect. */
#define KP_RGB_EFFECT_PARENT_ASSERT(node_id)                                   \
  COND_CODE_1(                                                                 \
      KP_RGB_EFFECT_IS_REGISTRY(node_id), (),                                  \
      (BUILD_ASSERT(                                                           \
           DT_NODE_HAS_COMPAT(DT_PARENT(node_id), keypaw_rgb_overlay),         \
           "RGB effect must be a child of keypaw,behavior-rgb-matrix or "      \
           "keypaw,rgb-overlay");                                              \
       BUILD_ASSERT(!DT_NODE_HAS_PROP(node_id, overlays) &&                    \
                        !DT_PROP(node_id, no_overlays),                        \
                    "overlays/no-overlays are registry-only; overlays are not "\
                    "composed onto a nested effect");))

#define KP_RGB_EFFECT_CONVERT_HOOK(node_id)                                    \
  COND_CODE_1(KP_RGB_EFFECT_IS_REGISTRY(node_id),                              \
              (kp_rgb_effect_convert_central_state_dependent_params), (NULL))

#define KP_RGB_EFFECT_DEFINE(node_id, render_fn, event_fn, cfg_inst)           \
  KP_RGB_EFFECT_PARENT_ASSERT(node_id)                                         \
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
  BUILD_ASSERT(DT_PROP_LEN_OR(node_id, overlays, 1) > 0,                       \
               "an empty `overlays` list is not expressible; use "             \
               "`no-overlays;` for an effect that wants none");                \
  KP_RGB_EFFECT_OVERLAY_LIST(node_id, cfg_inst)                                \
  static int cfg_inst##_init(const struct device *dev) { return 0; }           \
  IF_ENABLED(                                                                  \
      CONFIG_ZMK_BEHAVIOR_METADATA,                                            \
      (static const struct behavior_parameter_metadata cfg_inst##_metadata = { \
           .sets_len = 0,                                                      \
       };))                                                                    \
  static const struct kp_rgb_effect_api cfg_inst##_api = {                     \
      .behavior = {.locality = BEHAVIOR_LOCALITY_GLOBAL,                       \
                   .binding_convert_central_state_dependent_params =           \
                       KP_RGB_EFFECT_CONVERT_HOOK(node_id),                    \
                   .binding_pressed = NULL,                                    \
                   .binding_released = NULL,                                   \
                   IF_ENABLED(                                                 \
                       CONFIG_ZMK_BEHAVIOR_METADATA,                           \
                       (.parameter_metadata = &cfg_inst##_metadata, ))},       \
      .render = render_fn,                                                     \
      .on_event = event_fn,                                                    \
      .overlays = KP_RGB_EFFECT_OVERLAY_PTR(node_id, cfg_inst),                \
      .overlays_len = DT_PROP_LEN_OR(node_id, overlays, 0),                    \
  };                                                                           \
  BEHAVIOR_DT_DEFINE(node_id, cfg_inst##_init, NULL, &cfg_inst##_data,         \
                     &cfg_inst##_cfg, POST_KERNEL,                             \
                     CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &cfg_inst##_api)

/* Conditions are sampled on the control worker, never by a renderer. A variable
 * source must invalidate after a synchronized update; there is no polling fallback.
 * Deadlines use the supplied monotonic uptime and must be > now_ms or NEVER.
 * Dependencies are immutable, lifetime-long metadata; samples may only read their
 * declared children's cached values, and must not execute actions or LED I/O.
 */
enum kp_rgb_condition_scope {
  KP_RGB_CONDITION_ANY_SIDE = 0,
  KP_RGB_CONDITION_CENTRAL_ONLY,
};
#define KP_RGB_CONDITION_NEVER INT64_MAX

struct kp_rgb_condition_api {
  enum kp_rgb_condition_scope scope;
  /* Returns current activity. Assign to next_wakeup_ms to set a deadline for
   * next sample call.
   */
  bool (*sample)(const struct device *dev, int64_t now_ms,
                 int64_t *next_wakeup_ms);
  const struct device *const *(*dependencies)(const struct device *dev,
                                              size_t *count);
};
/* Start control once defaults, all settings, and all provider sources are ready.
 * Idempotent, thread-context only. Required when CONTROL_AUTO_START is disabled;
 * otherwise called by the late settings commit or APPLICATION startup hook.
 */
void zmk_rgb_matrix_start(void);

/* Any-context, non-blocking notification. NULL/unregistered devices are errors.
 * Notifications coalesce: intermediate source transitions need not be observed.
 * Early notifications are subsumed by mandatory initial sampling.
 */
void kp_rgb_condition_invalidate(const struct device *condition);
/* Control-worker only. Returns a declared child's cache during sampling, or a
 * consumer root's cache after evaluation. Never invokes a provider.
 */
bool kp_rgb_condition_value(const struct device *condition);

#include <zmk/rgb_matrix_condition_internal.h>

/* Config/data may be NULL. The declaration owns registration and cache storage;
 * providers must not access the registration or embed engine state in their data.
 */
#define KP_RGB_CONDITION_DEFINE(inst, api_expr, cfg_expr, data_expr)           \
  static struct kp_rgb_condition_registration kp_rgb_condition_##inst##_entry; \
  static int kp_rgb_condition_##inst##_init(const struct device *dev) {        \
    kp_rgb_condition_register(dev, &kp_rgb_condition_##inst##_entry);          \
    return 0;                                                                  \
  }                                                                            \
  DEVICE_DT_DEFINE(DT_DRV_INST(inst), kp_rgb_condition_##inst##_init, NULL,    \
                   data_expr, cfg_expr, POST_KERNEL,                           \
                   CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, api_expr)

/* Resolve a node's `condition` phandle, or NULL when it has none (meaning
 * "unconditionally active"). Shared by the compositor overlay and the trigger. */
#define KP_RGB_CONDITION_PTR(node_id)                                          \
  COND_CODE_1(DT_NODE_HAS_PROP(node_id, condition),                            \
              (DEVICE_DT_GET(DT_PROP(node_id, condition))), (NULL))

/* -------------------------------------------------------------------------
 * Overlays
 *
 * An overlay is a non-behavior compositor: while its condition is active it
 * renders its nested effect into a temporary layer buffer and
 * blends it onto the targeted LEDs over whatever the active effect painted.
 * `keypaw,rgb-overlay` is the only kind; third parties extend the module with
 * conditions and effects, not overlay kinds. Overlays paint in their
 * `keypaw,rgb-overlays` declaration order (later children composite on top);
 * an individual effect may override that list with its own `overlays`/
 * `no-overlays`.
 *
 * Condition providers notify the control engine after source changes. Renderers
 * consume cached gates and never sample conditions.
 * ------------------------------------------------------------------------- */

struct kp_rgb_overlay_api {
  const struct device *condition; /* NULL = unconditional */
  void (*render)(const struct device *dev, struct kp_rgb_frame *frame);
  /* The effect this overlay composites, so the engine can deliver input events
   * to it (a composited reactive/ripple effect has no other way to see them).
   * NULL for a kind that does not delegate to an effect. The engine delivers
   * only while the overlay is active. Return only the overlay's private effect.
   */
  const struct device *(*event_target)(const struct device *dev);
};

/* Kind-agnostic part of an overlay's config. */
struct kp_rgb_overlay_common_config {
  const uint32_t *keys; /* key positions, or NULL */
  size_t keys_len;
  const uint32_t *leds; /* raw chain indices, or NULL */
  size_t leds_len;
  uint8_t opacity; /* blend strength: 100 replaces, lower values mix */
  /* Paint every LED rather than the resolved targets; at `opacity` 100 this
   * lets the engine skip the effect and the overlays below it. */
  bool all_leds;
};

/* Kind-agnostic mutable part. */
struct kp_rgb_overlay_common_data {
  size_t led_count; /* resolved targets; 0 when keys and leds are both empty */
  size_t *leds;     /* the overlay's own storage */
  uint16_t index;   /* registry ordinal */
  bool local;       /* True when `local`: each half evaluates the condition */
  bool gate;        /* worker-published local gate */
};

/* One state bit per ordinal, in ceil(count / 16) uint16_t words. Sized from the
 * devicetree, so there is no fixed cap on the overlay count. */
#if DT_HAS_COMPAT_STATUS_OKAY(keypaw_rgb_overlays)
#define KP_RGB_OVERLAY_COUNT DT_CHILD_NUM(DT_INST(0, keypaw_rgb_overlays))
#else
#define KP_RGB_OVERLAY_COUNT 0
#endif
#define KP_RGB_OVERLAY_WORDS MAX(1, (KP_RGB_OVERLAY_COUNT + 15) / 16)

/* Add a device to the engine's overlay registry, where `index` places it. */
void kp_rgb_overlay_register(const struct device *dev);

/* The most targets an overlay can resolve. */
#define KP_RGB_OVERLAY_TARGET_CAP(node_id)                                     \
  MAX(1, MIN(KP_LED_COUNT, DT_PROP_LEN_OR(node_id, keys, 0) +                  \
                               DT_PROP_LEN_OR(node_id, leds, 0)))

/* Resolve an overlay's `keys`/`leds` devicetree spec into LED indices.
 * Returns the number written (never more than out_max, and always <=
 * KP_LED_COUNT); 0 when both lists are empty. Duplicate indices are dropped, so
 * each LED is painted once regardless of how the spec names it. */
size_t kp_rgb_resolve_targets(const uint32_t *keys, size_t keys_len,
                              const uint32_t *leds, size_t leds_len,
                              size_t *out, size_t out_max);

/* Paint `color` onto `leds` at `strength` percent: the colour is scaled by the
 * frame's brightness, then mixed over the existing pixels. */
void kp_rgb_overlay_paint(struct kp_rgb_frame *frame, const size_t *leds,
                          size_t led_count, struct led_rgb color,
                          uint8_t strength);

/* `kp_rgb_overlay_paint` generalised from one flat colour to a per-pixel
 * source indexed the same way as frame->pixels: `src[led]` is mixed over
 * `frame->pixels[led]` at `strength` percent for each targeted LED. This is the
 * compositor overlay's blit primitive, used to flatten a sub-effect's layer
 * buffer back onto the frame. */
void kp_rgb_overlay_paint_pixels(struct kp_rgb_frame *frame, const size_t *leds,
                                 size_t led_count, const struct led_rgb *src,
                                 uint8_t strength);

/* Declare the devicetree-derived target arrays for one overlay instance. A
 * missing property yields a one-element array whose length is read as 0, so the
 * declaration is always valid C and the array is always referenced. */
#define KP_RGB_OVERLAY_TARGET_ARRAYS(inst, cfg_inst)                           \
  static const uint32_t cfg_inst##_keys[] =                                    \
      COND_CODE_1(DT_NODE_HAS_PROP(DT_DRV_INST(inst), keys),                   \
                  (DT_PROP(DT_DRV_INST(inst), keys)), ({0}));                  \
  static const uint32_t cfg_inst##_leds[] =                                    \
      COND_CODE_1(DT_NODE_HAS_PROP(DT_DRV_INST(inst), leds),                   \
                  (DT_PROP(DT_DRV_INST(inst), leds)), ({0}))

/* Member-wise initializer for the common part of a kind's config. */
#define KP_RGB_OVERLAY_COMMON(node_id, cfg_inst)                               \
  {.keys = cfg_inst##_keys,                                                    \
   .keys_len = DT_PROP_LEN_OR(node_id, keys, 0),                               \
   .leds = cfg_inst##_leds,                                                    \
   .leds_len = DT_PROP_LEN_OR(node_id, leds, 0),                               \
   .opacity = DT_PROP_OR(node_id, opacity, 100),                               \
   .all_leds = DT_PROP(node_id, all_leds)}

/* Declare the device. Overlays are plain devices, never behaviors, so they
 * stay out of the behavior registry and cannot be keymap-bound. The macro also
 * allocates the instance's target storage, so a kind must declare its device
 * here rather than with DEVICE_DT_DEFINE directly. `event_fn` exposes the
 * effect the kind composites, or NULL; the engine delivers input events to it. */
#define KP_RGB_OVERLAY_DEFINE(inst, render_fn, event_fn, cfg_inst)             \
  BUILD_ASSERT(sizeof(cfg_inst##_cfg.common) ==                                \
                       sizeof(struct kp_rgb_overlay_common_config) &&          \
                   (const void *)&cfg_inst##_cfg ==                            \
                       (const void *)&cfg_inst##_cfg.common,                   \
               "overlay config must embed "                                    \
               "struct kp_rgb_overlay_common_config as the first field");      \
  BUILD_ASSERT(sizeof(cfg_inst##_data.common) ==                               \
                       sizeof(struct kp_rgb_overlay_common_data) &&            \
                   (const void *)&cfg_inst##_data ==                           \
                       (const void *)&cfg_inst##_data.common,                  \
               "overlay data must embed struct kp_rgb_overlay_common_data "    \
               "as the first field");                                          \
  BUILD_ASSERT(                                                                \
      DT_NODE_HAS_COMPAT(DT_PARENT(DT_DRV_INST(inst)), keypaw_rgb_overlays),   \
      "overlay must be a child of a keypaw,rgb-overlays node");                \
  BUILD_ASSERT(!(DT_PROP(DT_DRV_INST(inst), all_leds) &&                       \
                 (DT_PROP_LEN_OR(DT_DRV_INST(inst), keys, 0) > 0 ||            \
                  DT_PROP_LEN_OR(DT_DRV_INST(inst), leds, 0) > 0)),            \
               "`all-leds` replaces `keys`/`leds`; do not combine them");      \
  static size_t                                                                \
      cfg_inst##_targets[KP_RGB_OVERLAY_TARGET_CAP(DT_DRV_INST(inst))];        \
  static int cfg_inst##_init(const struct device *dev) {                       \
    const struct kp_rgb_overlay_common_config *cfg = dev->config;              \
    struct kp_rgb_overlay_common_data *data = dev->data;                       \
    data->leds = cfg_inst##_targets;                                           \
    data->led_count =                                                          \
        cfg->all_leds                                                          \
            ? 0 /* paints every LED; no target list needed */                  \
            : kp_rgb_resolve_targets(cfg->keys, cfg->keys_len, cfg->leds,      \
                                     cfg->leds_len, data->leds,                \
                                     ARRAY_SIZE(cfg_inst##_targets));          \
    data->index = (uint16_t)DT_NODE_CHILD_IDX(DT_DRV_INST(inst));              \
    data->local =                                                              \
        IS_ENABLED(CONFIG_ZMK_SPLIT) && DT_PROP(DT_DRV_INST(inst), local);     \
    kp_rgb_overlay_register(dev);                                              \
    return 0;                                                                  \
  }                                                                            \
  static const struct kp_rgb_overlay_api cfg_inst##_api = {                    \
      .condition = KP_RGB_CONDITION_PTR(DT_DRV_INST(inst)),                    \
      .render = render_fn,                                                     \
      .event_target = event_fn,                                                \
  };                                                                           \
  /* Init resolves `keys` through the engine's key -> LED table, which the     \
   * engine fills from its own POST_KERNEL init at the OBJECTS priority, ahead \
   * of these devices' DEFAULT priority. */                                    \
  DEVICE_DT_DEFINE(DT_DRV_INST(inst), cfg_inst##_init, NULL, &cfg_inst##_data, \
                   &cfg_inst##_cfg, POST_KERNEL,                               \
                   CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &cfg_inst##_api)

/* -------------------------------------------------------------------------
 * The compositor's devicetree contract
 *
 * Every overlay is the same generic kind (`keypaw,rgb-overlay`): it composites
 * exactly one enabled nested effect while its optional condition is active.
 * ------------------------------------------------------------------------- */

#define KP_RGB_OVERLAY_EFFECT_ONE(node_id) DEVICE_DT_GET(node_id)

#define KP_RGB_OVERLAY_EFFECT(node_id)                                         \
  DT_FOREACH_CHILD_STATUS_OKAY(node_id, KP_RGB_OVERLAY_EFFECT_ONE)

#define KP_RGB_OVERLAY_EFFECT_ASSERT(node_id)                                  \
  BUILD_ASSERT(!DT_NODE_HAS_PROP(node_id, effect),                             \
               "overlay effect references are not supported; "                 \
               "use a nested child");                                          \
  BUILD_ASSERT(DT_CHILD_NUM_STATUS_OKAY(node_id) == 1,                         \
               "overlay requires exactly one enabled nested effect");

/* Runtime-only output gate for the local strip; default false. Does not change
 * logical ON/user intent, settings, or split commands. One external policy
 * aggregator must own the setter. Thread context only, including before matrix
 * initialization. Returns 0 once accepted, not once hardware is black; failed
 * black transfers retry at 100..1000 ms. A completed setter prevents subsequent
 * colored submissions until released. Release resumes current logical intent
 * without advancing animation time spent inhibited. Event/deadline control and
 * overlay synchronization remain live even while logically OFF. Pending key
 * feedback is discarded while inhibited. An in-flight render or feedback pass
 * may finish computing. */
int zmk_rgb_matrix_set_inhibited(bool inhibited);
/* Returns the current gate state, not hardware settlement; safe from any context. */
bool zmk_rgb_matrix_is_inhibited(void);

/* Set or toggle local user intent; toggle inverts intent even while idle or
 * inhibited. Idle suppression and inhibition may still prevent output.
 * Thread context only. Return 0 on success or a negative lock error; success
 * does not promise hardware settlement. These calls neither save settings nor
 * send split commands. */
int zmk_rgb_matrix_toggle(void);
int zmk_rgb_matrix_on(void);
int zmk_rgb_matrix_off(void);
/* Returns logical ON after idle suppression; external inhibition does not alter it. */
int zmk_rgb_matrix_get_state(bool *on_off);

int zmk_rgb_matrix_select_effect(uint16_t effect);
int zmk_rgb_matrix_cycle_effect(int16_t direction);

/* Schedule an immediate repaint when output is eligible. Any-context; does not
 * sample conditions or grant output permission. */
void zmk_rgb_matrix_flush(void);
