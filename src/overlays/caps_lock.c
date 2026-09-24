/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Built-in "caps lock" overlay: paints its targeted LEDs while the host
 * reports the HID Caps Lock LED as set.
 *
 * The state is tracked by subscribing to zmk_hid_indicators_changed rather than
 * by reading zmk_hid_indicators_get_current_profile(): hid_indicators.c is only
 * built for the central half (zmk/app/CMakeLists.txt gates it on "(NOT
 * CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL"), whereas the event itself
 * is compiled everywhere -- a split peripheral re-raises it from the state the
 * central forwards (split/peripheral.c), provided
 * CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS is on.
 *
 * Caps Lock is global keyboard state, so one cached flag serves every instance;
 * a listener callback has no device argument to key an instance off anyway.
 */

#define DT_DRV_COMPAT keypaw_rgb_overlay_caps_lock

#include <stdbool.h>

#include <zephyr/device.h>

#include <dt-bindings/zmk/hid_indicators.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static bool kp_ovl_caps_lock_on;

static int kp_ovl_caps_lock_listener(const zmk_event_t *eh) {
  const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
  if (ev != NULL) {
    /* Repaint on the edge instead of waiting for the engine tick. */
    bool on = (ev->indicators & HID_INDICATOR_CAPS_LOCK) != 0;
    if (on != kp_ovl_caps_lock_on) {
      kp_ovl_caps_lock_on = on;
      zmk_rgb_matrix_flush();
    }
  }
  return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(kp_ovl_caps_lock, kp_ovl_caps_lock_listener);
ZMK_SUBSCRIPTION(kp_ovl_caps_lock, zmk_hid_indicators_changed);

struct kp_ovl_caps_lock_config {
  struct kp_rgb_overlay_common_config common;
};
struct kp_ovl_caps_lock_data {
  struct kp_rgb_overlay_common_data common;
};

static void kp_ovl_caps_lock_render(const struct device *dev, struct kp_rgb_frame *frame) {
  const struct kp_ovl_caps_lock_config *cfg = dev->config;
  const struct kp_ovl_caps_lock_data *data = dev->data;

  if (!kp_ovl_caps_lock_on) {
    return;
  }

  kp_rgb_overlay_paint(frame, data->common.leds, data->common.led_count,
                     kp_hex_to_rgb(cfg->common.color), cfg->common.brightness);
}

#define KP_OVL_CAPS_LOCK_DEFINE(inst)                                          \
  KP_RGB_OVERLAY_TARGET_ARRAYS(inst, kp_ovl_caps_lock_##inst);                 \
  static const struct kp_ovl_caps_lock_config kp_ovl_caps_lock_##inst##_cfg =  \
      {                                                                        \
          .common = KP_RGB_OVERLAY_COMMON(DT_DRV_INST(inst),                   \
                                          kp_ovl_caps_lock_##inst),            \
  };                                                                           \
  static struct kp_ovl_caps_lock_data kp_ovl_caps_lock_##inst##_data;          \
  KP_RGB_OVERLAY_DEFINE(inst, NULL, kp_ovl_caps_lock_render,                   \
                        kp_ovl_caps_lock_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_OVL_CAPS_LOCK_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
