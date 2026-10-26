/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/device.h>

/* Thread-only, including effect/overlay callbacks. Returns the retained selected
 * effect, or NULL for a non-selector or before controller startup has restored
 * settings. All required RGB providers must be initialized before that boundary.
 * Selection is independent of power, inhibition, visibility and local LED count.
 * Supply a controllable keypaw,rgb-overlay device.
 */
const struct device *zmk_rgb_selector_get_selected(const struct device *selector);
