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

/* Module-local, internal command. It lives here, next to the upstream include,
 * because that is where the claimed number is maintained against future
 * upstream growth; no devicetree node uses it (a keymap cannot bind it, since
 * its payload is an internal encoding).
 *
 * param2 carries one 16-bit word of indicator on/off state: the high half is
 * the word index, the low half the word's bits (bit N = the indicator at
 * ordinal word * 16 + N). Encode/decode with RGB_IND_STATE_VAL/WORD/BITS, which
 * are C-only and live in the module's src/rgb_matrix_internal.h.
 *
 * Like every &kprgb command it is BEHAVIOR_LOCALITY_GLOBAL, so the central's
 * state reaches every peripheral. Deliberately NOT persisted: the handler
 * applies it and returns before the kp_rgb_save_state() that ends the other
 * commands, so a layer change never schedules a flash write. */
#define RGB_IND_STATE_CMD 0x101

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

/* Expands to a plain integer expression so the devicetree compiler can evaluate
 * it as a cell value (dtc supports |, &, <<). No C types/suffixes here: they
 * are not valid in devicetree. The driver re-derives the flag/mask from the
 * macros above.
 */
#define KP_RGB_NO_KEY(x, y)                                     \
  (0x80000000 | (((x) & 0x3FFF) << 16) | ((y) & 0x3FFF))
