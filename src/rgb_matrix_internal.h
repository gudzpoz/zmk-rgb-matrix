/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#include <dt-bindings/keypaw/rgb_matrix.h>
#include <zmk/rgb_matrix.h>

/* State is owned by one keypaw,behavior-rgb-matrix device. The physical matrix
 * engine is shared, but these values are deliberately not global. */
struct kp_rgb_state {
  bool on;
  bool user_on;
  const struct device *active_fx;
};

struct kp_rgb_behavior_context {
  const struct device *dev;
  struct kp_rgb_state state;
  struct kp_rgb_tuning tuning;
  const struct device *const *effects;
  const uint8_t *no_cycle;
  size_t effect_count;
  size_t effect_index;
  const uint32_t *leds;
  size_t leds_len;
  bool all_leds;
  const struct device *const *overlays;
  size_t overlays_len;
  bool zone_valid;
#if IS_ENABLED(CONFIG_SETTINGS)
  struct k_work_delayable save_work;
#endif
};

struct kp_rgb_behavior_context *
kp_rgb_behavior_context_from_device(const struct device *dev);
size_t kp_rgb_behavior_count(void);
struct kp_rgb_behavior_context *kp_rgb_behavior_at(size_t index);
bool kp_rgb_behavior_owns_led(const struct kp_rgb_behavior_context *ctx,
                              size_t led);

/* Guards the shared physical output and all behavior contexts. Never hold it
 * across settings_save_one(), which can block on flash I/O. */
void kp_rgb_matrix_lock(void);
void kp_rgb_matrix_unlock(void);

/* Behavior registry and context-local command helpers. */
int kp_rgb_resolve_active(struct kp_rgb_behavior_context *ctx);
uint16_t kp_rgb_calc_effect_index(const struct kp_rgb_behavior_context *ctx,
                                  uint16_t current, int16_t delta);
int kp_rgb_select_effect(struct kp_rgb_behavior_context *ctx, uint16_t index);
bool kp_rgb_behavior_any_on(void);

/* Overlay registry and the split-pushed on/off state (see rgb_utils.c).
 * refresh()/dispatch() are central-only; on a peripheral they are no-ops. */
bool kp_rgb_overlay_gate(const struct device *dev,
                         const struct kp_rgb_overlay_api *api);
bool kp_rgb_overlay_refresh(void);
void kp_rgb_overlay_dispatch(void);
uint16_t kp_rgb_overlay_word_count(void);
bool kp_rgb_overlay_set_word(uint16_t word, uint16_t value);
uint16_t kp_rgb_overlay_get_word(uint16_t word);

/* param2 encoding for RGB_OVL_STATE_CMD: one 16-bit state word as
 * (word << 16) | bits. C-only, so unlike the command constant in
 * dt-bindings/keypaw/rgb_matrix.h this may use casts -- nothing parses this
 * header with the devicetree compiler. */
#define RGB_OVL_STATE_VAL(word, value)                                         \
  (((uint32_t)(word) << 16) | ((uint32_t)(value) & 0xFFFFu))
#define RGB_OVL_STATE_WORD(param) ((uint16_t)((param) >> 16))
#define RGB_OVL_STATE_BITS(param) ((uint16_t)(param))

#define KP_RGB_PERSIST_MAX_EFFECTS 16

/* Largest effect registry the engine accepts. The persisted blob holds one
 * fixed entry per effect, so with persistence compiled in that is what caps the
 * registry. rgb_settings.c guards the whole blob with IS_ENABLED(CONFIG_SETTINGS),
 * so without persistence the only real limit is the byte the effect index
 * travels in (see the UINT8_MAX assert in behavior_rgb_matrix.c). */
#if IS_ENABLED(CONFIG_SETTINGS)
#define KP_RGB_MAX_REGISTRY_EFFECTS KP_RGB_PERSIST_MAX_EFFECTS
#else
#define KP_RGB_MAX_REGISTRY_EFFECTS UINT8_MAX
#endif

size_t kp_rgb_effect_count(const struct kp_rgb_behavior_context *ctx);
const struct device *kp_rgb_effect_at(const struct kp_rgb_behavior_context *ctx,
                                      size_t index);
size_t kp_rgb_selected_effect(const struct kp_rgb_behavior_context *ctx);

int kp_rgb_save_state(struct kp_rgb_behavior_context *ctx);

int kp_rgb_calc_effect(struct kp_rgb_behavior_context *ctx, int16_t direction);
struct kp_rgb_hsb kp_rgb_calc_hue(const struct kp_rgb_behavior_context *ctx,
                                  int8_t direction);
struct kp_rgb_hsb kp_rgb_calc_sat(const struct kp_rgb_behavior_context *ctx,
                                  int8_t direction);
struct kp_rgb_hsb kp_rgb_calc_brt(const struct kp_rgb_behavior_context *ctx,
                                  int8_t direction);
int kp_rgb_change_hue(struct kp_rgb_behavior_context *ctx, int8_t direction);
int kp_rgb_change_sat(struct kp_rgb_behavior_context *ctx, int8_t direction);
int kp_rgb_change_brt(struct kp_rgb_behavior_context *ctx, int8_t direction);
int kp_rgb_set_hsb(struct kp_rgb_behavior_context *ctx,
                   struct kp_rgb_hsb color);
int kp_rgb_change_duration(struct kp_rgb_behavior_context *ctx,
                           int16_t direction);
int kp_rgb_set_duration(struct kp_rgb_behavior_context *ctx,
                        uint32_t duration_ms);
