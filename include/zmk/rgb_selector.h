/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/device.h>

/* Thread-only, including effect/overlay callbacks. Returns the retained selected
 * effect, or NULL for a NULL/unready selector or until that selector's
 * post-restoration startup succeeds.
 * Selection is independent of power, inhibition, visibility and local LED count.
 * Supply a controllable keypaw,rgb-overlay device. ISR calls return NULL.
 */
const struct device *zmk_rgb_selector_get_selected(const struct device *selector);

/* Select an enabled, ready effect belonging to this selector. Returns 0 on
 * success, including an already-selected effect; -EINVAL for NULL, foreign or
 * disabled effects, -ENODEV for unavailable selectors/members, and
 * -EWOULDBLOCK in ISR context. Failed calls leave selection unchanged.
 *
 * Thread-only and local-only: changed selection requests a repaint and condition
 * evaluation. Input and painting wait for current condition evaluation. Does not
 * save settings, broadcast, or change power; selection-dependent conditions may
 * change visibility. Do not call from effect/overlay callbacks. Supply a
 * controllable keypaw,rgb-overlay device.
 */
int zmk_rgb_selector_select(const struct device *selector,
                            const struct device *effect);

/* Restore only this selector's DTS selection and effect parameter defaults and
 * request child reset/repaint. Changed selection requests condition evaluation.
 * Returns 0, -ENODEV for an unavailable selector, or
 * -EWOULDBLOCK in ISR context. Thread-only, local-only; does not save settings or
 * broadcast. Supply a controllable keypaw,rgb-overlay device. Do not call from
 * effect/overlay callbacks.
 */
int zmk_rgb_selector_reset(const struct device *selector);
