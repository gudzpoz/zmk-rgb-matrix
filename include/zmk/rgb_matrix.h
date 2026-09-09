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

struct kp_rgb_effect_api {
  const struct behavior_driver_api behavior; /* must be first */
  rgb_matrix_effect_render_callback_t render;
  rgb_matrix_effect_event_callback_t on_event;
};

/* The brightness an effect should apply this frame. */
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
  };                                                                           \
  BEHAVIOR_DT_DEFINE(node_id, cfg_inst##_init, NULL, &cfg_inst##_data,         \
                     &cfg_inst##_cfg, POST_KERNEL,                             \
                     CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &cfg_inst##_api)

int zmk_rgb_matrix_toggle(void);
int zmk_rgb_matrix_on(void);
int zmk_rgb_matrix_off(void);
int zmk_rgb_matrix_get_state(bool *on_off);

int zmk_rgb_matrix_select_effect(uint16_t effect);
int zmk_rgb_matrix_cycle_effect(int16_t direction);
