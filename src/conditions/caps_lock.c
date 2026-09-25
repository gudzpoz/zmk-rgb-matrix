/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Built-in "caps lock" condition: active while the host reports the HID Caps
 * Lock LED as set.
 *
 * The state is tracked by subscribing to zmk_hid_indicators_changed rather than
 * by reading zmk_hid_indicators_get_current_profile(): hid_indicators.c is only
 * built for the central half (zmk/app/CMakeLists.txt gates it on "(NOT
 * CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL"), whereas the event
 * itself is compiled everywhere -- a split peripheral re-raises it from the
 * state the central forwards (split/peripheral.c), provided
 * CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS is on.
 *
 * Caps Lock is global keyboard state, so one cached flag serves every node; a
 * listener callback has no device argument to key an instance off anyway.
 */

#define DT_DRV_COMPAT keypaw_rgb_condition_caps_lock

#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/hid_indicators.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static bool kp_cond_caps_lock_on;

static int kp_cond_caps_lock_listener(const zmk_event_t *eh) {
  const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
  if (ev != NULL) {
    /* A locally determined consumer repaints on the edge instead of waiting for
     * the engine tick. */
    bool on = (ev->indicators & HID_INDICATOR_CAPS_LOCK) != 0;
    if (on != kp_cond_caps_lock_on) {
      kp_cond_caps_lock_on = on;
      zmk_rgb_matrix_flush();
    }
  }
  return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(kp_cond_caps_lock, kp_cond_caps_lock_listener);
ZMK_SUBSCRIPTION(kp_cond_caps_lock, zmk_hid_indicators_changed);

static bool kp_cond_caps_lock_active(const struct device *dev) {
  ARG_UNUSED(dev);
  return kp_cond_caps_lock_on;
}

#define KP_COND_CAPS_LOCK_DEFINE(inst)                                         \
  KP_RGB_CONDITION_DEFINE(inst, kp_cond_caps_lock_active, NULL)

DT_INST_FOREACH_STATUS_OKAY(KP_COND_CAPS_LOCK_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
