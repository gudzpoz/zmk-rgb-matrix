/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT keypaw_rgb_condition_caps_lock

#include <dt-bindings/zmk/hid_indicators.h>
#include <stdint.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/hid_indicators.h>
#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static bool sample(const struct device *dev, int64_t now_ms,
                   int64_t *next_wakeup_ms) {
  ARG_UNUSED(dev);
  ARG_UNUSED(now_ms);
  ARG_UNUSED(next_wakeup_ms);
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  return (zmk_hid_indicators_get_current_profile() & HID_INDICATOR_CAPS_LOCK) != 0;
#else
  return false;
#endif
}

#define INVALIDATE(inst)                                                       \
  kp_rgb_condition_invalidate(DEVICE_DT_GET(DT_DRV_INST(inst)));
static int listener(const zmk_event_t *eh) {
  const struct zmk_hid_indicators_changed *ev =
      as_zmk_hid_indicators_changed(eh);
  if (ev) {
    DT_INST_FOREACH_STATUS_OKAY(INVALIDATE)
  }
  return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(kp_cond_caps_lock, listener);
ZMK_SUBSCRIPTION(kp_cond_caps_lock, zmk_hid_indicators_changed);

static const struct kp_rgb_condition_api api = {
    .scope = KP_RGB_CONDITION_CENTRAL_ONLY, .sample = sample};
#define DEFINE(inst) KP_RGB_CONDITION_DEFINE(inst, &api, NULL, NULL)
DT_INST_FOREACH_STATUS_OKAY(DEFINE)

#endif
