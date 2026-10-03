/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/sys/atomic.h>

struct kp_rgb_latch_data {
  atomic_t active;
};
