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
#include <zmk/rgb_color.h>

/* Enabled selector children in declaration order. */
struct kp_rgb_selector_member {
  const struct device *effect;
};

struct kp_rgb_selector {
  const struct device *owner;
  const struct kp_rgb_selector_member *members;
  size_t effect_count;
  size_t effect_index;
  uint16_t initial_effect;
};

/* Available before control startup. Mutations require the matrix lock. */
size_t kp_rgb_selector_registry_count(void);
struct kp_rgb_selector *kp_rgb_selector_registry_at(size_t index);

/* Returns NULL for out-of-range slots. */
const struct device *kp_rgb_selector_at(const struct kp_rgb_selector *selector,
                                        size_t slot);
const struct device *
kp_rgb_selector_selected(const struct kp_rgb_selector *selector);
uint16_t kp_rgb_selector_calc_index(const struct kp_rgb_selector *selector,
                                    uint16_t current, int16_t delta);

/* Caller holds the matrix lock. */
void kp_rgb_selector_publish_locked(struct kp_rgb_selector *selector,
                                    const struct device *previous);
int kp_rgb_selector_resolve_locked(struct kp_rgb_selector *selector,
                                   size_t requested_slot);
void kp_rgb_selector_apply_defaults_locked(struct kp_rgb_selector *selector);
/* Returns 1 when selection changed, 0 when unchanged, or a negative error. */
int kp_rgb_selector_select_locked(struct kp_rgb_selector *selector,
                                  uint16_t slot);

/* Thread context with the matrix lock held; supply a selector overlay device.
 * Changes request composition, not callbacks or I/O. Returns 1 when changed,
 * 0 when unchanged, or a negative error. */
int kp_rgb_selector_overlay_select_locked(const struct device *selector,
                                          uint16_t slot);

/* Caller holds the matrix lock; NULL before control startup or for devices
 * outside the selector registry. */
struct kp_rgb_selector *
kp_rgb_selector_overlay_state_locked(const struct device *selector);
int kp_rgb_selector_overlay_reset_locked(const struct device *selector);
void kp_rgb_selector_overlays_defaults_locked(void);

struct behavior_driver_api;
extern const struct behavior_driver_api kp_rgb_selector_behavior_api;
struct zmk_behavior_binding;
#define RGB_SELECTOR_COLOR_CMD 0x108u
#define RGB_SELECTOR_PERIOD_CMD 0x109u
#define KP_RGB_SELECTOR_COMMAND_MASK 0xffffu
#define KP_RGB_SELECTOR_MEMBER_COMMAND(command, slot) \
  ((uint32_t)(command) | ((uint32_t)(slot) << 16))

int kp_rgb_selector_convert_locked(struct kp_rgb_selector *selector,
                                   struct zmk_behavior_binding *binding);
int kp_rgb_selector_dispatch_locked(struct kp_rgb_selector *selector,
                                    uint32_t command, uint32_t argument);
int kp_rgb_selector_binding_convert(const struct device *selector,
                                    struct zmk_behavior_binding *binding);
int kp_rgb_selector_binding_apply(const struct device *selector,
                                  uint32_t command, uint32_t argument);

/* Parameter APIs do not require selector membership. */
int kp_rgb_set_color_locked(const struct device *effect,
                            const struct kp_rgb_hsb *color);
int kp_rgb_set_period_locked(const struct device *effect, uint32_t period_ms);
