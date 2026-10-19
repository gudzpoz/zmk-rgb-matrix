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

struct kp_rgb_effect_defaults {
  struct kp_rgb_hsb color;
  uint16_t duration_ms;
};

/* Indexed by DTS child slot; disabled children retain a zeroed entry. */
struct kp_rgb_selector_member {
  const struct device *effect;
  const char *id;
  struct kp_rgb_effect_defaults defaults;
};

struct kp_rgb_selector {
  const struct device *owner;
  const struct device *control;
  const char *id;
  const struct kp_rgb_selector_member *members;
  size_t effect_count;
  size_t effect_index;
  const struct device *active_fx;
  uint16_t initial_effect;
  bool publish_changes;
  bool notification_pending;
  struct kp_rgb_selector *notification_next;
};

/* Identity metadata is available before device/provider readiness. Mutations
 * require the matrix lock. Registry access does not start selectors. */
size_t kp_rgb_selector_registry_count(void);
struct kp_rgb_selector *kp_rgb_selector_registry_at(size_t index);

/* Returns NULL for out-of-range, disabled or currently unready slots. */
const struct device *kp_rgb_selector_at(const struct kp_rgb_selector *selector,
                                        size_t slot);
uint16_t kp_rgb_selector_calc_index(const struct kp_rgb_selector *selector,
                                    uint16_t current, int16_t delta);
int kp_rgb_selector_member_slot(const struct kp_rgb_selector *selector,
                                const struct device *effect);

/* Caller holds the matrix lock. Selection changes queue notifications for
 * publish_changes selectors; callbacks are delivered outside the lock. */
void kp_rgb_selector_publish_locked(struct kp_rgb_selector *selector,
                                    const struct device *previous);
int kp_rgb_selector_resolve_locked(struct kp_rgb_selector *selector);
int kp_rgb_selector_apply_defaults_locked(struct kp_rgb_selector *selector);
/* Returns 1 when selection changed, 0 when unchanged, or a negative error. */
int kp_rgb_selector_select_locked(struct kp_rgb_selector *selector,
                                  uint16_t slot);

/* Called before starting condition control, after restoration and provider
 * readiness. Thread-only; returns a negative error if any selector has no ready
 * member. Successful selector startup is idempotent. */
int kp_rgb_selector_overlays_start(void);

/* Thread context with the matrix lock held; supply a selector overlay device.
 * Changes request composition, not callbacks or I/O. Returns 1 when changed,
 * 0 when unchanged, or a negative error. */
int kp_rgb_selector_overlay_select_locked(const struct device *selector,
                                          uint16_t slot);

/* Caller holds the matrix lock; NULL means this is not a ready selector
 * overlay. */
struct kp_rgb_selector *
kp_rgb_selector_overlay_state_locked(const struct device *selector);
int kp_rgb_selector_overlay_reset_locked(const struct device *selector);
void kp_rgb_selector_overlays_defaults_locked(void);

struct zmk_behavior_binding;
int kp_rgb_selector_convert_locked(struct kp_rgb_selector *selector,
                                   struct zmk_behavior_binding *binding);
int kp_rgb_selector_dispatch_locked(struct kp_rgb_selector *selector,
                                    uint32_t command, uint32_t argument);
int kp_rgb_selector_binding_convert(const struct device *selector,
                                    struct zmk_behavior_binding *binding);
int kp_rgb_selector_binding_apply(const struct device *selector,
                                  uint32_t command, uint32_t argument);

/* Ready-effect parameter APIs deliberately do not require selector membership.
 */
int kp_rgb_set_color_locked(const struct device *effect,
                            const struct kp_rgb_hsb *color);
int kp_rgb_set_period_locked(const struct device *effect, uint32_t period_ms);
