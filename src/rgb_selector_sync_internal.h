/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "rgb_selector_internal.h"

#define RGB_SELECTOR_COLOR_CMD 0x108u
#define RGB_SELECTOR_PERIOD_CMD 0x109u
#define KP_RGB_SELECTOR_COMMAND_MASK 0xffffu
#define KP_RGB_SELECTOR_MEMBER_COMMAND(command, slot)                          \
  ((uint32_t)(command) | ((uint32_t)(slot) << 16))

struct kp_rgb_selector_snapshot {
  const struct device *control;
  uint16_t slot;
  struct kp_rgb_hsb color;
  uint32_t period_ms;
};

/* Caller holds the matrix mutex. No callbacks, persistence or transport I/O. */
int kp_rgb_selector_snapshot_locked(const struct device *selector,
                                    struct kp_rgb_selector_snapshot *snapshot);
int kp_rgb_selector_member_apply_locked(struct kp_rgb_selector *selector,
                                        uint32_t command, uint32_t argument);
/* Three absolute bindings: member color, member period, then selection.
 * The snapshot and returned binding contain no pointers to mutable parameters.
 */
int kp_rgb_selector_snapshot_binding(
    const struct kp_rgb_selector_snapshot *snapshot, uint8_t step,
    struct zmk_behavior_binding *binding);

/* Central split transport only; invoke after releasing the matrix mutex.
 * Success means local transport acceptance, not acknowledged delivery. */
int kp_rgb_split_send_selector(uint8_t source,
                               const struct kp_rgb_selector_snapshot *snapshot,
                               uint8_t step);
