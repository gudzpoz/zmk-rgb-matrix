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
#include <zmk/rgb_persist.h>

struct kp_rgb_tuning {
  uint8_t max_brightness;
  uint8_t idle_brightness;
};

struct kp_rgb_state {
  bool user_on;
  const struct device *active_fx;
};

struct kp_rgb_effect_defaults {
  struct kp_rgb_hsb color;
  uint32_t duration_ms;    /* 0 -> initial_duration_ms */
};

struct kp_rgb_controller {
  const struct device *dev;
  struct kp_rgb_state state;
  struct kp_rgb_tuning tuning;
  uint8_t brightness;
  const struct device *const *effects;
  const struct kp_rgb_effect_defaults *effect_defaults;
  size_t effect_count;
  size_t effect_index;
  bool initial_on;
  int32_t initial_brightness;
  int64_t initial_duration_ms;
  uint16_t initial_effect;
};

extern struct kp_rgb_controller kp_rgb_controller;

struct kp_rgb_scene_runtime {
  struct kp_rgb_callback_state state;
  bool wanted;
  bool reset_pending;
};

void kp_rgb_callbacks_set_active(const struct device *dev,
                                const struct kp_rgb_effect_callbacks *callbacks,
                                struct kp_rgb_callback_state *state,
                                bool active, int64_t now_ms);
void kp_rgb_callbacks_reset(const struct device *dev,
                           const struct kp_rgb_effect_callbacks *callbacks,
                           struct kp_rgb_callback_state *state, int64_t now_ms);
bool kp_rgb_callbacks_on_event(const struct device *dev,
                              const struct kp_rgb_effect_callbacks *callbacks,
                              const struct kp_rgb_callback_state *state,
                              const struct kp_rgb_key_event *event);
bool kp_rgb_callbacks_render(const struct device *dev,
                            const struct kp_rgb_effect_callbacks *callbacks,
                            struct kp_rgb_callback_state *state,
                            const struct kp_rgb_frame *frame);

/* Never hold the matrix lock across flash I/O. */
void kp_rgb_matrix_lock(void);
void kp_rgb_matrix_unlock(void);

void kp_rgb_conditions_request_refresh(uint64_t token);
void kp_rgb_conditions_refreshed(uint64_t token);

/* Caller holds the matrix lock. */
void kp_rgb_reconcile_power_locked(void);
void kp_rgb_request_runtime_reset_locked(void);

int kp_rgb_resolve_active(void);
uint16_t kp_rgb_calc_effect_index(uint16_t current, int16_t delta);
int kp_rgb_select_effect(uint16_t index);

extern const size_t kp_rgb_all_targets[MAX(1, KP_LED_COUNT)];
const size_t *kp_rgb_overlay_targets(const struct device *dev);
size_t kp_rgb_overlay_target_count(const struct device *dev);

/* refresh() runs on the control worker; dispatch() is central-only. */
bool kp_rgb_overlay_gate(const struct device *dev);
bool kp_rgb_overlay_covers_all(const struct device *dev);
bool kp_rgb_overlay_refresh(void);
void kp_rgb_overlay_dispatch(void);
const struct device *const *kp_rgb_overlay_list(void);
size_t kp_rgb_overlay_count(void);
uint16_t kp_rgb_overlay_word_count(void);
bool kp_rgb_overlay_set_word(uint16_t word, uint16_t value);
uint16_t kp_rgb_overlay_get_word(uint16_t word);

bool kp_rgb_condition_require(const struct device *dev);
bool kp_rgb_condition_valid(const struct device *dev);
void kp_rgb_conditions_start(void);
void kp_rgb_overlay_conditions_init(void);

#if IS_ENABLED(CONFIG_KEYPAW_RGB_TRIGGERS)
void kp_rgb_triggers_init(void);
void kp_rgb_triggers_begin(void);
bool kp_rgb_triggers_evaluate(int64_t now_ms);
void kp_rgb_triggers_quarantine(void);
#else
static inline void kp_rgb_triggers_init(void) {}
static inline void kp_rgb_triggers_begin(void) {}
static inline bool kp_rgb_triggers_evaluate(int64_t now_ms) {
  ARG_UNUSED(now_ms);
  return false;
}
static inline void kp_rgb_triggers_quarantine(void) {}
#endif

/* param2 encoding for RGB_OVL_STATE_CMD: one 16-bit state word. */
#define RGB_OVL_STATE_VAL(word, value)                                         \
  (((uint32_t)(word) << 16) | ((uint32_t)(value) & 0xFFFFu))
#define RGB_OVL_STATE_WORD(param) ((uint16_t)((param) >> 16))
#define RGB_OVL_STATE_BITS(param) ((uint16_t)(param))

#if IS_ENABLED(CONFIG_SETTINGS)
#define KP_RGB_MAX_REGISTRY_EFFECTS KP_RGB_PERSIST_MAX_EFFECTS
#else
#define KP_RGB_MAX_REGISTRY_EFFECTS UINT8_MAX
#endif

size_t kp_rgb_effect_count(void);
const struct device *kp_rgb_effect_at(size_t index);
size_t kp_rgb_selected_effect(void);
int kp_rgb_save_state(void);
int kp_rgb_select_effect_locked(uint16_t index);

/* Restore presets and user intent, then reconcile local output permission. */
int kp_rgb_apply_defaults(void);
void kp_rgb_reset_state(void);

int kp_rgb_calc_effect(int16_t direction);
struct kp_rgb_hsb kp_rgb_calc_hue(int8_t direction);
struct kp_rgb_hsb kp_rgb_calc_sat(int8_t direction);
struct kp_rgb_hsb kp_rgb_calc_brt(int8_t direction);
int kp_rgb_change_hue(int8_t direction);
int kp_rgb_change_sat(int8_t direction);
int kp_rgb_change_brt(int8_t direction);
int kp_rgb_set_hsb(struct kp_rgb_hsb color);
int kp_rgb_change_duration(int16_t direction);
int kp_rgb_set_duration(uint32_t duration_ms);
