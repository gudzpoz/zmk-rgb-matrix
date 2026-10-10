/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/device.h>
#include <zmk/event_manager.h>

/* Delivered asynchronously on the RGB worker, outside the matrix mutex, after
 * retained selection changes or first becomes available at startup. Resample
 * zmk_rgb_selector_get_selected(selector); intermediate changes may coalesce.
 * Initial sampling must not depend on receiving an earlier event. A no-op
 * selection or a parameter-only change does not emit this event.
 *
 * Condition providers should invalidate their registered condition when their
 * selector changes. Listeners may use read-only queries and local selector APIs;
 * they must not invoke rendering or child callbacks directly.
 */
struct zmk_rgb_selector_selection_changed {
  const struct device *selector;
};

ZMK_EVENT_DECLARE(zmk_rgb_selector_selection_changed);
