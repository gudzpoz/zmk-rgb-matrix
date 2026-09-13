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
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#include <dt-bindings/keypaw/rgb_matrix.h>
#include <zmk/rgb_matrix.h>

/* Engine state shared between rgb_matrix.c and behavior_rgb_matrix.c.
 */
struct kp_rgb_state {
  /* Momentary lit state. The idle auto-off writes this and nothing else. */
  bool on;
  /* The user's intent, persisted across reboots. */
  bool user_on;
  const struct device *active_fx;
};

extern struct kp_rgb_state kp_rgb_state;

/* Owned by behavior_rgb_matrix.c (its node carries the tunables), read by the
 * engine when it builds a frame and by the effects through frame->tune. */
extern struct kp_rgb_tuning kp_rgb_tuning;

/* Guards against the behavior's command handlers racing the render on the
 * low priority work queue. The settings save work snapshots the effect table
 * under this lock; it must never be held across settings_save_one(), which can
 * block on a flash erase while holding the settings lock. */
void kp_rgb_matrix_lock(void);
void kp_rgb_matrix_unlock(void);

/* behavior_rgb_matrix.c: registry and command helpers. */
int kp_rgb_resolve_active(void);
uint16_t kp_rgb_calc_effect_index(uint16_t current, int16_t delta);

/* Bounds the persisted blob, so a stored blob has a size known to both writer
 * and reader. Must be >= KP_RGB_NEFFECTS, which behavior_rgb_matrix.c asserts.
 * Changing it changes sizeof(blob) and therefore discards stored state. */
#define KP_RGB_PERSIST_MAX_EFFECTS 16

/* Registry introspection, for the settings blob. `index` is the effect's
 * devicetree `index` (pinned to its child position), not a chain position. */
size_t kp_rgb_effect_count(void);
const struct device *kp_rgb_effect_at(size_t index);
size_t kp_rgb_selected_effect(void);

/* rgb_settings.c: debounced persist of the user intent, the selected effect and
 * every effect's colour/duration. Safe to call from a behavior handler; it only
 * reschedules a work item and is a no-op when CONFIG_SETTINGS is off. */
int kp_rgb_save_state(void);

int zmk_rgb_matrix_calc_effect(int16_t direction);
struct kp_rgb_hsb zmk_rgb_matrix_calc_hue(int8_t direction);
struct kp_rgb_hsb zmk_rgb_matrix_calc_sat(int8_t direction);
struct kp_rgb_hsb zmk_rgb_matrix_calc_brt(int8_t direction);
int zmk_rgb_matrix_change_hue(int8_t direction);
int zmk_rgb_matrix_change_sat(int8_t direction);
int zmk_rgb_matrix_change_brt(int8_t direction);
int zmk_rgb_matrix_set_hsb(struct kp_rgb_hsb color);
int zmk_rgb_matrix_change_duration(int16_t direction);
int zmk_rgb_matrix_set_duration(uint32_t duration_ms);
