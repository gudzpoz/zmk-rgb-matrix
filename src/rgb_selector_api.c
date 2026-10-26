/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zmk/rgb_selector.h>

#include "rgb_matrix_internal.h"
#include "rgb_replication_internal.h"

void kp_rgb_selector_publish_locked(struct kp_rgb_selector *selector,
                                    const struct device *previous) {
  if (kp_rgb_selector_selected(selector) != previous) {
    kp_rgb_coordinator_request(KP_RGB_REQUEST_CONTROL | KP_RGB_REQUEST_REPAINT);
    kp_rgb_replication_changed_locked();
  }
}

const struct device *zmk_rgb_selector_get_selected(const struct device *selector) {
  kp_rgb_assert_thread_context();
  kp_rgb_matrix_lock();
  struct kp_rgb_selector *state = kp_rgb_selector_overlay_state_locked(selector);
  const struct device *selected =
      state == NULL ? NULL : kp_rgb_selector_selected(state);
  kp_rgb_matrix_unlock();
  return selected;
}
