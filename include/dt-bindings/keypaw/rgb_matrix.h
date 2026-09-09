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
