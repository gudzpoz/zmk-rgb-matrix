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

#include <zephyr/devicetree.h>
#include <zephyr/sys/util_macro.h>

/* RGB devices initialize at POST_KERNEL after strips. Devices with RGB phandle
 * dependencies must use this priority or later; equal priorities follow DTS
 * dependency order. The definition helpers below use it automatically. */
#define KP_RGB_DEVICE_INIT_PRIORITY 99

BUILD_ASSERT(CONFIG_KERNEL_INIT_PRIORITY_OBJECTS < KP_RGB_DEVICE_INIT_PRIORITY,
             "RGB geometry/workqueue initialization must precede RGB devices");

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

/* Layout units use 100 for a standard key's width and height. Do not retain
 * frame pointers or their arrays after the callback. */
struct kp_rgb_frame {
  /* Full local strip size; coords and pixels use physical LED indices. */
  size_t count;
  const struct kp_rgb_coord *coords;
  struct led_rgb *pixels;

  /* Always non-NULL; target_count unique indices below count. Zero is empty.
   * target_count == count permits painting in physical-index order. */
  const size_t *targets;
  size_t target_count;

  /* Overlay-only temporary storage for at least count pixels, independent of
   * pixels, with unspecified contents. NULL in effect callbacks. */
  struct led_rgb *scratch;

  /* This half's own monotonic uptime for this worker pass. Different on each
   * split half; use it for local effects, never for split-synchronized phase. */
  int64_t local_ms;
  /* Split-shared animation clock, wrapping after ~49.7 days. Equal on both
   * halves within link latency, so `clock_ms % period` is the same phase on each
   * half regardless of render cadence or pauses. Use it for continuous effects
   * that must stay in step across the split. */
  uint32_t clock_ms;
  /* Local time since this participant's previous render, saturated at
   * UINT32_MAX. Zero on first paint, resume and after render returned false.
   * For stochastic or event-anchored effects; do not use it for split phase. */
  uint32_t elapsed_ms;

  /* Longest edge of this half's LEDs, in layout units (never 0). */
  uint16_t board_length;
  /* Vertical extent of this half's LEDs (max y - min y), in layout units
   * (never 0). Needed by vertical-basis effects (gradient up/down, cycle
   * up/down, chevron, radial extent). */
  uint16_t board_height;
};

struct kp_rgb_key_event {
  uint32_t position;
  bool pressed;
  /* Original event uptime, not delivery time. */
  int64_t timestamp_ms;
};

/* Callbacks run under the matrix mutex on the low-priority worker. Do not retain
 * frame/event pointers. Read-only queries and requests for a later repaint are
 * allowed; do not invoke bindings or change scene controls (power, inhibition,
 * selection, overlay enablement, or user parameters), even indirectly through
 * synchronous event dispatch. Owners may delegate to independently owned children;
 * do not invoke engine scene rendering recursively.
 *
 * Paint only targets and leave other pixels untouched, before controller
 * brightness. Effect targets start black; overlay targets contain earlier
 * composition. Overlays apply their opacity as blend strength. Simulation and
 * geometry remain full-strip. Return true from render while another timed
 * update is needed; return false only after painting a valid static/terminal
 * image. False does not deactivate the participant. Settled participants must
 * still paint on requested composition, but the engine may skip a settled
 * participant that sits below a still-animating one on later animation frames;
 * invalidate or return true from on_event if its output can change while
 * settled. Return true from on_event when the event requires a repaint. Input
 * is copied, best effort, and routed to participants active at delivery, not
 * capture time. Queue overflow or output suppression may discard either half of
 * a key pair.
 *
 * For engine-managed participants, reset runs before first activation, on
 * explicit RGB reset, and when a paused half resumes; preserve user parameters.
 * set_active runs only on sampled activity changes, before input. Owners choose
 * child callback delivery and must satisfy the child's initialization and timing
 * contracts. Clearing temporary feedback on active=false is recommended, not
 * required; brief hide/show transitions may coalesce. */
typedef bool (*rgb_matrix_effect_render_callback_t)(
    const struct device *dev, const struct kp_rgb_frame *frame);
typedef bool (*rgb_matrix_effect_event_callback_t)(
    const struct device *dev, const struct kp_rgb_key_event *event);

/* render is required; the other callbacks may be NULL. */
struct kp_rgb_effect_callbacks {
  rgb_matrix_effect_render_callback_t render;
  rgb_matrix_effect_event_callback_t on_event;
  void (*set_active)(const struct device *dev, bool active, int64_t now_ms);
  void (*reset)(const struct device *dev, int64_t now_ms);
};

/* Storage managed by the instance helpers; do not modify its fields. */
struct kp_rgb_callback_state {
  int64_t last_render_ms;
  bool initialized;
  bool active;
  bool animating;
};

/* Each instance must own its effect's mutable state independently of other owners.
 * Initialize with KP_RGB_EFFECT_INSTANCE_INIT. The effect must be ready before helper
 * calls. Call only from owner callbacks; scene membership is not required. */
struct kp_rgb_effect_instance {
  const struct device *effect;
  struct kp_rgb_callback_state state;
};

#define KP_RGB_EFFECT_INSTANCE_INIT(effect_dev) { .effect = (effect_dev) }

/* First activation resets before set_active(true). Repeated values emit no callback. */
void kp_rgb_effect_instance_set_active(struct kp_rgb_effect_instance *instance,
                                       bool active, int64_t now_ms);
/* Reset provider state and timing while preserving activity and user parameters. */
void kp_rgb_effect_instance_reset(struct kp_rgb_effect_instance *instance, int64_t now_ms);
/* Inactive instances ignore input and return false. */
bool kp_rgb_effect_instance_on_event(struct kp_rgb_effect_instance *instance,
                                     const struct kp_rgb_key_event *event);
/* Supply black effect targets and NULL scratch. Inactive instances do not paint.
 * Computes an independent elapsed_ms, zero after activation, reset, clock restart
 * or a settled result. Does not clear pixels. Returns the child's animation demand. */
bool kp_rgb_effect_instance_render(struct kp_rgb_effect_instance *instance,
                                   const struct kp_rgb_frame *frame);
/* Pause interval accumulation without resetting the provider or changing activity.
 * On owner renders with elapsed_ms == 0, restart every affected child helper before
 * rendering any child, including helpers whose render callback will be skipped. */
void kp_rgb_effect_instance_restart_clock(struct kp_rgb_effect_instance *instance);

/* Call from owner callbacks for independently owned effects. Supply black targets,
 * NULL scratch and the intended timing. This dispatches render unchanged: it does
 * not clear pixels, manage clocks or check engine activity. */
bool kp_rgb_effect_render(const struct device *dev, const struct kp_rgb_frame *frame);

#define KP_RGB_EFFECT_COMMON_CONFIG(inst, defaults_)                           \
  .common = {.defaults = defaults_}
#define KP_RGB_EFFECT_COMMON_DATA(defaults_) .common = defaults_

struct kp_rgb_effect_common_data {
  uint16_t duration_ms;    /* single cycle animation duration */
  struct kp_rgb_hsb color; /* default or current color */
};
struct kp_rgb_effect_common_config {
  struct kp_rgb_effect_common_data defaults;
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

/* Default period when duration_ms is zero; clamped to configured duration bounds. */
#define KP_RGB_EFFECT_PERIOD_FALLBACK_MS 1000u

/* The effect's animation period in milliseconds; never 0, so an effect can use
 * it as a modulus without a guard. */
static inline uint32_t kp_rgb_effect_period(const struct device *dev) {
  uint32_t duration = kp_rgb_effect_data(dev)->duration_ms;
  return CLAMP(duration != 0 ? duration : KP_RGB_EFFECT_PERIOD_FALLBACK_MS,
               CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS,
               CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
}
/* Build one compile-time default initializer shared by the immutable config and
 * mutable device data. Clamp the full-width property before narrowing it. */
#define KP_RGB_EFFECT_DEFAULTS(inst, default_color_, default_duration_)        \
  {                                                                            \
      .color = KP_RGB_HSB_FROM_HEX(                                            \
          DT_PROP_OR(DT_DRV_INST(inst), color, default_color_)),               \
      .duration_ms = (uint16_t)CLAMP(                                          \
          ((uint32_t)DT_PROP_OR(DT_DRV_INST(inst), duration,                   \
                                default_duration_) != 0U)                      \
              ? (uint32_t)DT_PROP_OR(DT_DRV_INST(inst), duration,              \
                                     default_duration_)                        \
              : KP_RGB_EFFECT_PERIOD_FALLBACK_MS,                              \
          CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS,                            \
          CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS),                           \
  }
#define KP_RGB_EFFECT_SELECTION_ASSERT(node_id)                                \
  BUILD_ASSERT(!DT_NODE_HAS_COMPAT(DT_PARENT(node_id), keypaw_rgb_matrix),     \
               "matrix children must be overlays, not effects");               \
  BUILD_ASSERT(!DT_NODE_HAS_PROP(node_id, overlays) &&                         \
                   !DT_NODE_HAS_PROP(node_id, no_overlays),                    \
               "effect overlays/no-overlays properties have been removed");

/* Registers an ordinary device. callbacks_expr must point to a lifetime-long,
 * immutable struct kp_rgb_effect_callbacks table, exposed directly as dev->api.
 * Select effects through their controllable overlay, not as keymap behaviors.
 *
 * Initialize a per-instance defaults expression with KP_RGB_EFFECT_DEFAULTS and
 * use it in both KP_RGB_EFFECT_COMMON_CONFIG and KP_RGB_EFFECT_COMMON_DATA.
 * The color property overrides the color fallback; duration overrides its fallback,
 * with zero using the common 1000 ms fallback. Duration is clamped to configured
 * bounds before narrowing. Selector reset copies only these common defaults into
 * ready effect data. Lifecycle reset preserves user parameters. */
#define KP_RGB_EFFECT_DEFINE(node_id, callbacks_expr, cfg_inst)                \
  KP_RGB_EFFECT_SELECTION_ASSERT(node_id)                                      \
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
  DEVICE_DT_DEFINE(node_id, NULL, NULL, &cfg_inst##_data, &cfg_inst##_cfg,     \
                   POST_KERNEL, KP_RGB_DEVICE_INIT_PRIORITY, callbacks_expr)

/* Conditions are sampled during coordinator control evaluation, not rendering.
 * A variable source must invalidate after a synchronized update; there is no polling.
 * Timed providers must arrange their own wakeups and invalidate when due.
 */
enum kp_rgb_condition_scope {
  KP_RGB_CONDITION_ANY_SIDE = 0,
  KP_RGB_CONDITION_CENTRAL_ONLY,
};

struct kp_rgb_condition_api {
  enum kp_rgb_condition_scope scope;
  /* Return true when active. Must be side-effect-free and must not synchronously
   * invalidate any condition. */
  bool (*sample)(const struct device *dev);
};

/* Any-context, non-blocking request to resample all eligible live conditions.
 * Requests coalesce: intermediate source transitions need not be observed.
 * Early requests are subsumed by mandatory initial sampling.
 * Pending evaluation defers new scene lifecycle, input and painting. Repeated
 * invalidation may delay them indefinitely; an already-admitted scene may finish.
 */
void kp_rgb_condition_invalidate(void);
/* Control-evaluation only. Returns the cached value without invoking a provider.
 */
bool kp_rgb_condition_value(const struct device *condition);

#include <zmk/rgb_matrix_condition_internal.h>

/* Config/data may be NULL. The declaration owns registration and cache storage;
 * providers must not access the registration or embed engine state in their data.
 */
#define KP_RGB_CONDITION_DEFINE(inst, api_expr, cfg_expr, data_expr)           \
  KP_RGB_CONDITION_REGISTRATION_DEFINE(DT_DRV_INST(inst))                      \
  DEVICE_DT_DEFINE(DT_DRV_INST(inst), NULL, NULL, data_expr, cfg_expr,         \
                   POST_KERNEL, KP_RGB_DEVICE_INIT_PRIORITY, api_expr)

/* Resolve a node's `condition` phandle, or NULL when it has none (meaning
 * "unconditionally active"). Shared by the compositor overlay and the trigger. */
#define KP_RGB_CONDITION_PTR(node_id)                                          \
  COND_CODE_1(DT_NODE_HAS_PROP(node_id, condition),                            \
              (DEVICE_DT_GET(DT_PROP(node_id, condition))), (NULL))

/* Overlays composite in declaration order, consume cached condition gates and
 * own any embedded effects. */

/* Kind-agnostic part of an overlay's config. */
struct kp_rgb_overlay_common_config {
  const struct device *condition; /* NULL = unconditional */
  /* Lifetime-long immutable table; render is required, other callbacks optional. */
  const struct kp_rgb_effect_callbacks *callbacks;
  /* Set true only when opacity 100 replaces every target independently of its
   * incoming color. Filters must set false. */
  bool replaces_target;
  uint8_t opacity; /* blend strength: 100 replaces, lower values mix */
  /* Use every local LED instead of the keys/leds lists. */
  bool all_leds;
  const uint32_t *keys;
  size_t key_count;
  const uint32_t *leds;
  size_t led_count;
};

/* Kind-agnostic mutable part. */
struct kp_rgb_overlay_common_data {
  size_t led_count; /* resolved targets; 0 when keys and leds are both empty */
  size_t *leds;     /* initialized by the engine before overlay devices */
  uint16_t index;   /* enabled-overlay index */
  bool local;       /* True when `local`: each half evaluates the condition */
  bool gate;        /* worker-published local gate */
};

/* These accessors require an overlay device, not an arbitrary device. */
static inline const struct kp_rgb_overlay_common_config *
kp_rgb_overlay_cfg(const struct device *dev) { return dev->config; }
static inline struct kp_rgb_overlay_common_data *
kp_rgb_overlay_data(const struct device *dev) { return dev->data; }
static inline const struct kp_rgb_effect_callbacks *
kp_rgb_overlay_callbacks(const struct device *dev) {
  return kp_rgb_overlay_cfg(dev)->callbacks;
}

#define KP_RGB_IS_CONTROLLED_OVERLAY(node_id) \
  UTIL_AND(DT_NODE_HAS_COMPAT(node_id, keypaw_rgb_overlay), \
           DT_PROP_OR(node_id, controllable, 0))
#define KP_RGB_SELECTOR_ENTRY(node_id, fn) \
  COND_CODE_1(KP_RGB_IS_CONTROLLED_OVERLAY(node_id), (fn(node_id)), ())
#define KP_RGB_FOREACH_SELECTOR(fn) \
  DT_FOREACH_CHILD_STATUS_OKAY_VARGS(KP_RGB_NODE, KP_RGB_SELECTOR_ENTRY, fn)
#define KP_RGB_SELECTOR_COUNT_ONE(node_id) +1
#define KP_RGB_SELECTOR_COUNT (0 KP_RGB_FOREACH_SELECTOR(KP_RGB_SELECTOR_COUNT_ONE))

/* Both halves must use the same enabled overlay sequence for split gate bits. */
#define KP_RGB_OVERLAY_COUNT DT_CHILD_NUM_STATUS_OKAY(KP_RGB_NODE)
#define KP_RGB_OVERLAY_WORDS MAX(1, (KP_RGB_OVERLAY_COUNT + 15) / 16)
#define KP_RGB_OVERLAY_INDEX(node_id)                                          \
  UTIL_CAT(kp_rgb_overlay_index_, DT_DEP_ORD(node_id))
#define KP_RGB_OVERLAY_INDEX_ENTRY(node_id) KP_RGB_OVERLAY_INDEX(node_id),

#if DT_HAS_COMPAT_STATUS_OKAY(keypaw_rgb_matrix)
enum {
  DT_FOREACH_CHILD_STATUS_OKAY(KP_RGB_NODE, KP_RGB_OVERLAY_INDEX_ENTRY)
  kp_rgb_overlay_index_count,
};
#endif

/* Blend src[led] over existing pixels at strength 0..100.
 * Pass frame->targets with frame->target_count, or a subset with its length.
 * Targets must be unique indices from frame->targets; indices are not checked. */
void kp_rgb_overlay_paint_pixels(const struct kp_rgb_frame *frame, const size_t *leds,
                                 size_t led_count, const struct led_rgb *src,
                                 uint8_t strength);

#define KP_RGB_OVERLAY_KEYS(node_id) \
  UTIL_CAT(kp_rgb_overlay_keys_, DT_DEP_ORD(node_id))
#define KP_RGB_OVERLAY_LEDS(node_id) \
  UTIL_CAT(kp_rgb_overlay_leds_, DT_DEP_ORD(node_id))

/* Declare raw lists and target storage before config/data. Resolved LED targets
 * must be unique. The engine resolves them before overlay device initialization. */
#define KP_RGB_OVERLAY_TARGET_ARRAYS(inst, cfg_inst)                           \
  BUILD_ASSERT(DT_SAME_NODE(DT_PARENT(DT_DRV_INST(inst)), KP_RGB_NODE),        \
               "overlay must be a child of the keypaw,rgb-matrix node");       \
  static const uint32_t KP_RGB_OVERLAY_KEYS(                                   \
      DT_DRV_INST(inst))[MAX(1, DT_PROP_LEN_OR(DT_DRV_INST(inst), keys, 0))] = \
      DT_PROP_OR(DT_DRV_INST(inst), keys, {0});                                \
  static const uint32_t KP_RGB_OVERLAY_LEDS(                                   \
      DT_DRV_INST(inst))[MAX(1, DT_PROP_LEN_OR(DT_DRV_INST(inst), leds, 0))] = \
      DT_PROP_OR(DT_DRV_INST(inst), leds, {0});                                \
  static size_t cfg_inst##_targets[MAX(                                        \
      1, DT_PROP_LEN_OR(DT_DRV_INST(inst), keys, 0) +                          \
             DT_PROP_LEN_OR(DT_DRV_INST(inst), leds, 0))]

/* Initialize the common data before device initialization. */
#define KP_RGB_OVERLAY_COMMON_DATA(inst, cfg_inst)                             \
  .common = {                                                                  \
      .leds = cfg_inst##_targets,                                              \
      .index = KP_RGB_OVERLAY_INDEX(DT_DRV_INST(inst)),                        \
      .local =                                                                 \
          IS_ENABLED(CONFIG_ZMK_SPLIT) && DT_PROP(DT_DRV_INST(inst), local),   \
  }

/* Member-wise initializer for the common part of a kind's config. */
#define KP_RGB_OVERLAY_COMMON(node_id, callbacks_expr, replaces)               \
  {.opacity = DT_PROP_OR(node_id, opacity, 100),                               \
   .all_leds = DT_PROP(node_id, all_leds),                                     \
   .keys = KP_RGB_OVERLAY_KEYS(node_id),                                       \
   .key_count = DT_PROP_LEN_OR(node_id, keys, 0),                              \
   .leds = KP_RGB_OVERLAY_LEDS(node_id),                                       \
   .led_count = DT_PROP_LEN_OR(node_id, leds, 0),                              \
   .condition = KP_RGB_CONDITION_PTR(node_id),                                 \
   .callbacks = callbacks_expr,                                                \
   .replaces_target = replaces}

/* Config/data must embed their common headers first. */
#define KP_RGB_OVERLAY_ASSERT(inst, cfg_inst) \
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
  BUILD_ASSERT(KP_RGB_OVERLAY_INDEX(DT_DRV_INST(inst)) <= UINT16_MAX,          \
               "overlay index must fit uint16_t");                             \
  BUILD_ASSERT(DT_PROP_OR(DT_DRV_INST(inst), opacity, 100) >= 0 &&             \
                   DT_PROP_OR(DT_DRV_INST(inst), opacity, 100) <= 100,         \
               "overlay opacity must be 0..100");                              \
  BUILD_ASSERT(!(DT_PROP(DT_DRV_INST(inst), all_leds) &&                       \
                 (DT_PROP_LEN_OR(DT_DRV_INST(inst), keys, 0) > 0 ||            \
                  DT_PROP_LEN_OR(DT_DRV_INST(inst), leds, 0) > 0)),            \
               "`all-leds` replaces `keys`/`leds`; do not combine them");

/* render is required; other callbacks are optional. Ordinary overlays have no API. */
#define KP_RGB_OVERLAY_DEFINE(inst, cfg_inst)                                  \
  KP_RGB_OVERLAY_ASSERT(inst, cfg_inst)                                        \
  DEVICE_DT_DEFINE(DT_DRV_INST(inst), NULL, NULL, &cfg_inst##_data,            \
                   &cfg_inst##_cfg, POST_KERNEL, KP_RGB_DEVICE_INIT_PRIORITY,  \
                   NULL)

/* Include <drivers/behavior.h> before using this declaration. */
#define KP_RGB_OVERLAY_BEHAVIOR_DEFINE(inst, behavior_api_expr, cfg_inst)      \
  KP_RGB_OVERLAY_ASSERT(inst, cfg_inst)                                        \
  BEHAVIOR_DT_DEFINE(DT_DRV_INST(inst), NULL, NULL, &cfg_inst##_data,          \
                     &cfg_inst##_cfg, POST_KERNEL,                             \
                     KP_RGB_DEVICE_INIT_PRIORITY, behavior_api_expr)

/* A private keypaw,rgb-overlay composites exactly one enabled nested effect. */

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
 * colored submissions until released. Release preserves outstanding transfer
 * backoff and resumes current logical intent without advancing animation time
 * spent inhibited. Event-driven control and overlay synchronization remain live
 * even while logically OFF. Pending key feedback is discarded while inhibited.
 * An in-flight render or feedback pass may finish computing. */
int zmk_rgb_matrix_set_inhibited(bool inhibited);
/* Returns the current gate state, not hardware settlement; safe from any context. */
bool zmk_rgb_matrix_is_inhibited(void);

/* Set or toggle local user intent; toggle inverts intent even while idle or
 * inhibited. Activity suppression and inhibition may still prevent output.
 * Thread context only. Return 0 on success or a negative lock error; success
 * does not promise hardware settlement. These calls neither save settings nor
 * send split commands. */
int zmk_rgb_matrix_toggle(void);
int zmk_rgb_matrix_on(void);
int zmk_rgb_matrix_off(void);
/* Returns user intent after activity suppression: false during SLEEP and during
 * IDLE with idle-brightness=0. Inhibition and brightness scaling do not alter it. */
int zmk_rgb_matrix_get_state(bool *on_off);

/* Return retained user intent before activity suppression. Thread context only;
 * NULL returns -EINVAL, ISR calls return -EWOULDBLOCK. */
int zmk_rgb_matrix_get_user_state(bool *user_on);

/* Set parameters of a ready RGB effect device. Thread context only.
 * Supply an RGB effect device; composition membership is not required.
 * Hue accepts 0..360 (360 becomes 0); saturation/brightness accept 0..100.
 * Period accepts configured DURATION_MIN_MS..DURATION_MAX_MS without clamping.
 * NULL color/invalid values return -EINVAL; NULL/not-ready effect devices
 * return -ENODEV; ISR calls return -EWOULDBLOCK. Changed values request repaint
 * without resetting clocks. Success means state acceptance, not strip delivery.
 * Neither call saves settings, sends split commands or changes user intent. */
int zmk_rgb_matrix_set_color(const struct device *effect, const struct kp_rgb_hsb *color);
int zmk_rgb_matrix_set_period(const struct device *effect, uint32_t period_ms);

/* Request a scene pass: reconcile activity and repaint if output is eligible.
 * Notify after a synchronized source update changes pixels or animation demand.
 * Any-context, non-blocking and coalescing. */
void zmk_rgb_matrix_flush(void);
