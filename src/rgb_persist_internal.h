/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/device.h>
#include <zmk/rgb_persist.h>

/* Identity metadata is available before post-restoration selector startup. */
size_t kp_rgb_persist_selector_count(void);
const struct kp_rgb_persist_selector_ids *
kp_rgb_persist_selector_at(size_t index, const struct device **owner);
int kp_rgb_persist_ids_validate(void);
const struct device *kp_rgb_persist_member_at(size_t selector, size_t member);
