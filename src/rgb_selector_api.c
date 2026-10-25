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
  if (k_is_in_isr())
    return NULL;
  kp_rgb_matrix_lock();
  struct kp_rgb_selector *state = kp_rgb_selector_overlay_state_locked(selector);
  const struct device *selected =
      state == NULL ? NULL : kp_rgb_selector_selected(state);
  kp_rgb_matrix_unlock();
  return selected;
}

int zmk_rgb_selector_select(const struct device *selector,
                            const struct device *effect) {
  if (k_is_in_isr())
    return -EWOULDBLOCK;
  if (effect == NULL)
    return -EINVAL;
  kp_rgb_matrix_lock();
  struct kp_rgb_selector *state = kp_rgb_selector_overlay_state_locked(selector);
  int err = -ENODEV;
  if (state != NULL) {
    err = -EINVAL;
    for (size_t slot = 0; slot < state->effect_count; slot++) {
      if (kp_rgb_selector_at(state, slot) == effect) {
        err = kp_rgb_selector_overlay_select_locked(selector, slot);
        break;
      }
    }
  }
  kp_rgb_matrix_unlock();
  return err < 0 ? err : 0;
}

int zmk_rgb_selector_reset(const struct device *selector) {
  if (k_is_in_isr())
    return -EWOULDBLOCK;
  kp_rgb_matrix_lock();
  int err = kp_rgb_selector_overlay_reset_locked(selector);
  kp_rgb_matrix_unlock();
  return err;
}
