/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>

/* The state of a keypaw,rgb-condition-latch. A condition's dev->data holds
 * this; the condition's active() reads it and the keypaw,behavior-rgb-overlay-
 * toggle nested under the same node writes it. This header is the only place
 * the two coupled translation units agree on the layout, so it stays private:
 * beside the condition it describes (conditions/) rather than in the public
 * include/ tree. The toggle reaches it with a relative include.
 *
 * `volatile`: the toggle writes it from keymap context and the render tick
 * reads it on the low-priority workqueue. A single bool access cannot tear, so
 * the worst case is one stale ~32 ms frame -- exactly like the overlay state
 * words in rgb_utils.c. */
struct kp_rgb_latch_data {
  volatile bool active;
};
