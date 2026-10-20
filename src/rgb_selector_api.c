/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zmk/events/rgb_selector_selection_changed.h>
#include <zmk/rgb_selector.h>

#include "rgb_matrix_internal.h"

ZMK_EVENT_IMPL(zmk_rgb_selector_selection_changed);

static struct kp_rgb_selector *notification_head;
static void kp_rgb_selector_notify_work(struct k_work *work);
static K_WORK_DEFINE(notification_work, kp_rgb_selector_notify_work);

void kp_rgb_selector_publish_locked(struct kp_rgb_selector *selector,
                                    const struct device *previous) {
  if (!selector->publish_changes || selector->active_fx == previous)
    return;
  if (!selector->notification_pending) {
    kp_rgb_control_refresh_hold_locked();
    selector->notification_pending = true;
    selector->notification_next = notification_head;
    notification_head = selector;
  } else {
    kp_rgb_control_refresh_locked();
  }
  k_work_submit_to_queue(kp_rgb_work_q(), &notification_work);
}

static void kp_rgb_selector_notify_work(struct k_work *work) {
  ARG_UNUSED(work);
  for (;;) {
    kp_rgb_matrix_lock();
    struct kp_rgb_selector *selector = notification_head;
    if (selector == NULL) {
      kp_rgb_matrix_unlock();
      return;
    }
    notification_head = selector->notification_next;
    selector->notification_next = NULL;
    selector->notification_pending = false;
    const struct device *owner = selector->owner;
    kp_rgb_matrix_unlock();
    raise_zmk_rgb_selector_selection_changed(
        (struct zmk_rgb_selector_selection_changed){.selector = owner});
    kp_rgb_matrix_lock();
    kp_rgb_control_refresh_release_locked();
    kp_rgb_matrix_unlock();
  }
}

const struct device *zmk_rgb_selector_get_selected(const struct device *selector) {
  if (k_is_in_isr())
    return NULL;
  kp_rgb_matrix_lock();
  struct kp_rgb_selector *state = kp_rgb_selector_overlay_state_locked(selector);
  const struct device *selected = state == NULL ? NULL : state->active_fx;
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
