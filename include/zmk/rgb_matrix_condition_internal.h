/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/sys/atomic.h>

/* Private declaration storage; not a provider API. */
struct kp_rgb_condition_registration {
  const struct device *dev;
  struct kp_rgb_condition_registration *next;
  const struct device *const *deps;
  size_t dep_count;
  size_t rank;

  atomic_t pending;
  int64_t deadline;
  bool live;
  bool resolved;
  bool fault;
  bool sampled;
  bool value;
  bool dirty;
};

void kp_rgb_condition_register(const struct device *dev,
                               struct kp_rgb_condition_registration *entry);
