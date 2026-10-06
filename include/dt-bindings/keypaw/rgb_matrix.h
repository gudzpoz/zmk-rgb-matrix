/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* Reuse upstream's RGB_*_CMD numbering so &kprgb accepts the same mnemonics as
 * &rgb_ug, and so the four ported effects keep their original indices.
 */
#include <dt-bindings/zmk/rgb.h>

/* Module-local, internal command: param2 carries one 16-bit word of overlay
 * on/off state (word index in the high half, bits in the low half), encoded
 * with RGB_OVL_STATE_VAL/WORD/BITS from src/rgb_matrix_internal.h.
 *
 * BEHAVIOR_LOCALITY_GLOBAL like every &kprgb command, but deliberately not
 * persisted: the handler returns before the kp_rgb_save_state() the other
 * commands end with, so a layer change never schedules a flash write. */
#define RGB_OVL_STATE_CMD 0x101

/* User-bindable: clear every RGB behavior's persisted state on this half and
 * restore the devicetree defaults, so a reflash or a reordered effect registry
 * starts clean. &kprgb is BEHAVIOR_LOCALITY_GLOBAL, so one press on the central
 * reaches each half, and each half deletes only its own keys. */
#define RGB_RESET_CMD 0x102
#define RGB_RESET RGB_RESET_CMD 0

/* Module-local, internal command: param2 is the low 32 bits of the central's
 * monotonic uptime. A peripheral derives its shared animation-clock offset from
 * it (see kp_rgb_clock_ms). Deliberately not persisted: the handler returns
 * before the kp_rgb_save_state() the other commands end with. */
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
