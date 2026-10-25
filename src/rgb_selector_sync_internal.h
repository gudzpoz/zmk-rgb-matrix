/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "rgb_selector_internal.h"

struct kp_rgb_selector_snapshot {
  const struct device *control;
  uint16_t slot;
  struct kp_rgb_hsb color;
  uint32_t period_ms;
};

/* Caller holds the matrix mutex. No callbacks, persistence or transport I/O. */
int kp_rgb_selector_snapshot_locked(const struct device *selector,
                                    struct kp_rgb_selector_snapshot *snapshot);
/* Advance cursor past the next enabled member; return 1, 0 at end,
 * or a negative error. Caller holds the matrix mutex. */
int kp_rgb_selector_next_snapshot_locked(const struct device *selector,
                                         size_t *cursor,
                                         struct kp_rgb_selector_snapshot *snapshot);
/* Three absolute bindings: member color, member period, then selection.
 * snapshot, snapshot->control and binding must be non-NULL; step must be 0–2.
 * The snapshot and returned binding contain no pointers to mutable parameters.
 */
void kp_rgb_selector_snapshot_binding(
    const struct kp_rgb_selector_snapshot *snapshot, uint8_t step,
    struct zmk_behavior_binding *binding);

/* Central split transport only; invoke after releasing the matrix mutex.
 * snapshot/control must be non-NULL and step must be 0–2, as for the encoder.
 * Success means local transport acceptance, not acknowledged delivery. */
int kp_rgb_split_send_selector(uint8_t source,
                               const struct kp_rgb_selector_snapshot *snapshot,
                               uint8_t step);

#if IS_ENABLED(CONFIG_KEYPAW_RGB_SPLIT_SYNC)
/* Caller holds the matrix mutex. Behavior adapters mark successful commands;
 * local APIs and peripheral reception do not request synchronization. */
void kp_rgb_split_selector_dirty_locked(const struct device *selector);
void kp_rgb_split_selectors_dirty_locked(void);
void kp_rgb_split_selector_reset_locked(const struct device *selector);
void kp_rgb_split_global_dirty_locked(void);
#else
static inline void kp_rgb_split_selector_dirty_locked(const struct device *selector) {
  (void)selector;
}
static inline void kp_rgb_split_selectors_dirty_locked(void) {}
static inline void kp_rgb_split_selector_reset_locked(const struct device *selector) {
  (void)selector;
}
static inline void kp_rgb_split_global_dirty_locked(void) {}
#endif
