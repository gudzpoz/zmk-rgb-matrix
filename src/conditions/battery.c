/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#define DT_DRV_COMPAT keypaw_rgb_condition_battery
#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
#include <zmk/event_manager.h>
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>
#endif
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
#include <zmk/usb.h>
#include <zmk/events/usb_conn_state_changed.h>
#endif

DEFINE_DT_ENUM(battery_state_t, state, any, charging, discharging);
struct battery_cfg {
  int32_t below, above;
  battery_state_t state;
};

static bool sample(const struct device *dev) {
  const struct battery_cfg *c = dev->config;
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
  if (c->below != 101 || c->above != -1) {
    int32_t soc = MIN(zmk_battery_state_of_charge(), 100);
    if (soc <= c->above || soc >= c->below) return false;
  }
#endif
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
  if (c->state == DT_ENUM_CONST(state, charging)) return zmk_usb_is_powered();
  if (c->state == DT_ENUM_CONST(state, discharging)) return !zmk_usb_is_powered();
#endif
  ARG_UNUSED(c);
  return true;
}

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) || IS_ENABLED(CONFIG_USB_DEVICE_STACK)
static int listener(const zmk_event_t *eh) {
  ARG_UNUSED(eh);
  kp_rgb_condition_invalidate();
  return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(kp_cond_battery, listener);
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
ZMK_SUBSCRIPTION(kp_cond_battery, zmk_battery_state_changed);
#endif
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(kp_cond_battery, zmk_usb_conn_state_changed);
#endif
#endif

static const struct kp_rgb_condition_api api = {
    .scope = KP_RGB_CONDITION_ANY_SIDE, .sample = sample};
#define BELOW(inst) ((int32_t)DT_INST_PROP(inst, below))
#define ABOVE(inst) ((int32_t)DT_INST_PROP(inst, above))
#define SOC_TEST(inst) (BELOW(inst) != 101 || ABOVE(inst) != -1)
#define DEFINE(inst)                                                           \
  BUILD_ASSERT(BELOW(inst) >= 1 && BELOW(inst) <= 101,                         \
               "battery below must be in 1..101");                             \
  BUILD_ASSERT(ABOVE(inst) >= -1 && ABOVE(inst) <= 99,                         \
               "battery above must be in -1..99");                             \
  BUILD_ASSERT(ABOVE(inst) + 1 < BELOW(inst),                                  \
               "battery threshold interval must not be empty");                \
  BUILD_ASSERT(SOC_TEST(inst) || !DT_INST_ENUM_HAS_VALUE(inst, state, any),    \
               "battery condition must constrain SOC or power");               \
  BUILD_ASSERT(!SOC_TEST(inst) || IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING),    \
               "battery SOC requires CONFIG_ZMK_BATTERY_REPORTING");           \
  BUILD_ASSERT(DT_INST_ENUM_HAS_VALUE(inst, state, any) ||                     \
                   IS_ENABLED(CONFIG_USB_DEVICE_STACK),                        \
               "battery power requires CONFIG_USB_DEVICE_STACK");              \
  static const struct battery_cfg cfg_##inst = {                               \
      .below = BELOW(inst),                                                    \
      .above = ABOVE(inst),                                                    \
      .state = CONV_DT_ENUM(inst, state)};                                     \
  KP_RGB_CONDITION_DEFINE(inst, &api, &cfg_##inst, NULL)

DT_INST_FOREACH_STATUS_OKAY(DEFINE)
#endif
