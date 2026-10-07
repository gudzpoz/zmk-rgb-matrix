/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT keypaw_rgb_condition_ble

#include <stdint.h>

#include <zmk/rgb_matrix.h>

#if IS_ENABLED(CONFIG_ZMK_BLE) &&                                              \
    (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
#define KP_COND_BLE_READABLE 1
#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#else
#define KP_COND_BLE_READABLE 0
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

DEFINE_DT_ENUM(ble_state_t, state, any, connected, disconnected, open);

struct ble_cfg {
  int32_t profile; /* -1 = any */
  ble_state_t state;
};

static bool sample(const struct device *dev, int64_t now_ms,
                   int64_t *next_wakeup_ms) {
  ARG_UNUSED(now_ms);
  ARG_UNUSED(next_wakeup_ms);
#if KP_COND_BLE_READABLE
  const struct ble_cfg *c = dev->config;
  if (c->profile >= 0 && zmk_ble_active_profile_index() != c->profile) {
    return false;
  }
  switch (c->state) {
  case DT_ENUM_CONST(state, connected):
    return zmk_ble_active_profile_is_connected();
  case DT_ENUM_CONST(state, disconnected):
    return !zmk_ble_active_profile_is_connected();
  case DT_ENUM_CONST(state, open):
    return zmk_ble_active_profile_is_open();
  default:
    return true;
  }
#else
  ARG_UNUSED(dev);
  return false;
#endif
}

#if KP_COND_BLE_READABLE
/* Profile select, connect and disconnect all raise this one event. */
#define INVALIDATE(inst)                                                       \
  kp_rgb_condition_invalidate(DEVICE_DT_GET(DT_DRV_INST(inst)));
static int listener(const zmk_event_t *eh) {
  if (as_zmk_ble_active_profile_changed(eh)) {
    DT_INST_FOREACH_STATUS_OKAY(INVALIDATE)
  }
  return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(kp_cond_ble, listener);
ZMK_SUBSCRIPTION(kp_cond_ble, zmk_ble_active_profile_changed);
#endif

#define DEFINE(inst)                                                           \
  static const struct ble_cfg cfg_##inst = {                                   \
      .profile = DT_PROP(DT_DRV_INST(inst), profile),                          \
      .state = CONV_DT_ENUM(inst, state)};                                     \
  static const struct kp_rgb_condition_api api_##inst = {                      \
      .scope = KP_RGB_CONDITION_CENTRAL_ONLY, .sample = sample};               \
  KP_RGB_CONDITION_DEFINE(inst, &api_##inst, &cfg_##inst, NULL)
DT_INST_FOREACH_STATUS_OKAY(DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
