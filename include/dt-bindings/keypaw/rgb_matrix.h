/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <dt-bindings/zmk/rgb.h>

/* Internal matrix transport command, not a keymap command. param1's upper
 * 16 bits select a gate word; param2 contains its 32 bits. Words follow dense
 * enabled-overlay order. Receiving a word does not save settings. */
#define RGB_OVL_STATE_CMD 0x101

/* Restore defaults for the addressed selector, or global state and all selectors
 * through the global control. param2 must be zero. With settings enabled,
 * selection/global defaults are saved asynchronously; parameter defaults are
 * runtime-only. Obsolete settings records are not deleted. */
#define RGB_RESET_CMD 0x102
#define RGB_RESET RGB_RESET_CMD 0

/* Internal matrix transport command, not a keymap command. param2 is the low
 * 32 bits of the central's monotonic uptime in milliseconds. Updates the shared
 * animation-clock offset without saving settings. */
#define RGB_CLOCK_CMD 0x103

/* A "mapping" entry is one of two things:
 *
 *   - a plain integer N  -> the LED sits under key position N in the
 *     zmk,physical-layout named by the rgb-matrix node's "physical-layout"
 *     phandle; its (x, y) centre is derived from that key's attributes.
 *
 *   - KP_RGB_NO_KEY(x, y) -> the LED is not under a key (an edge/underglow
 *     LED) and carries explicit (x, y) coordinates in physical-layout units.
 *
 * The two are distinguished by the top bit: when set, the low 14 bits of each
 * half hold x and y. Coordinates therefore range 0..16383 (plenty for a board
 * measured in 100-unit keys).
 */

#define KP_RGB_NO_KEY_XY_FLAG 0x80000000u
#define KP_RGB_NO_KEY_XY_X_MASK 0x3FFFu
#define KP_RGB_NO_KEY_XY_Y_MASK 0x3FFFu

/* Expands to a plain integer expression dtc can evaluate as a cell value: no C
 * types or suffixes, which are invalid in devicetree. */
#define KP_RGB_NO_KEY(x, y)                                     \
  (0x80000000 | (((x) & 0x3FFF) << 16) | ((y) & 0x3FFF))
