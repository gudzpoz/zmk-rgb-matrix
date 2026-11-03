/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#define DT_DRV_COMPAT keypaw_rgb_condition_endpoint
#include <zmk/rgb_matrix.h>
#include "../rgb_matrix_internal.h"

#define HOST_SIDE (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
DEFINE_DT_ENUM(endpoint_transport_t, transport, ble, usb, any);
DEFINE_DT_ENUM(endpoint_state_t, state, any, connected, disconnected, open, powered);
struct endpoint_cfg {
  int32_t profile;
  endpoint_transport_t transport;
  endpoint_state_t state;
  bool ignore_routing;
};

#if HOST_SIDE
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/position_state_changed.h>
#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#endif
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
#include <zmk/usb.h>
#include <zmk/events/usb_conn_state_changed.h>
#endif

/* Provider sampling and preference work share the RGB queue, including this cache. */
static enum zmk_transport observed_preference;
static bool preference_observed;

static void sample_preference(struct k_work *work) {
  ARG_UNUSED(work);
  enum zmk_transport preference = zmk_endpoint_get_preferred_transport();
  if (!preference_observed || preference != observed_preference) {
    observed_preference = preference;
    preference_observed = true;
    kp_rgb_condition_invalidate();
  }
}
static K_WORK_DELAYABLE_DEFINE(preference_work, sample_preference);

static int listener(const zmk_event_t *eh) {
  if (as_zmk_position_state_changed(eh)) {
    /* Scheduling, unlike rescheduling, preserves the first event's deadline. */
    k_work_schedule_for_queue(kp_rgb_work_q(), &preference_work, K_MSEC(10));
  } else {
    kp_rgb_condition_invalidate();
  }
  return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(kp_cond_endpoint, listener);
ZMK_SUBSCRIPTION(kp_cond_endpoint, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(kp_cond_endpoint, zmk_position_state_changed);
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(kp_cond_endpoint, zmk_ble_active_profile_changed);
#endif
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(kp_cond_endpoint, zmk_usb_conn_state_changed);
#endif
#endif /* HOST_SIDE */

static bool sample(const struct device *dev) {
#if HOST_SIDE
  const struct endpoint_cfg *c = dev->config;
  enum zmk_transport preference = zmk_endpoint_get_preferred_transport();
  /* Preserve a mismatch even if preference changes again during this pass. */
  if (preference_observed && preference != observed_preference) {
    preference_observed = false;
  }
  if (c->transport == DT_ENUM_CONST(transport, any)) {
    bool connected = zmk_endpoint_is_connected();
    return c->state == DT_ENUM_CONST(state, connected) ? connected : !connected;
  }
  if (c->transport == DT_ENUM_CONST(transport, usb)) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    if (c->state == DT_ENUM_CONST(state, powered))
      return zmk_usb_get_conn_state() == ZMK_USB_CONN_POWERED;
    if (c->state == DT_ENUM_CONST(state, disconnected))
      return zmk_usb_get_conn_state() == ZMK_USB_CONN_NONE;
    return c->ignore_routing ? zmk_usb_is_hid_ready() :
           zmk_endpoint_get_selected().transport == ZMK_TRANSPORT_USB;
#else
    return false;
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
  }
#if IS_ENABLED(CONFIG_ZMK_BLE)
  if (!c->ignore_routing) {
    if (preference == ZMK_TRANSPORT_NONE) return false;
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    if (preference == ZMK_TRANSPORT_USB && zmk_usb_is_hid_ready()) return false;
#endif
  }
  if (c->profile >= 0 && zmk_ble_active_profile_index() != c->profile) return false;
  switch (c->state) {
  case DT_ENUM_CONST(state, connected): return zmk_ble_active_profile_is_connected();
  case DT_ENUM_CONST(state, disconnected): return !zmk_ble_active_profile_is_connected();
  case DT_ENUM_CONST(state, open): return zmk_ble_active_profile_is_open();
  default: return true;
  }
#else
  return false;
#endif /* IS_ENABLED(CONFIG_ZMK_BLE) */
#else
  ARG_UNUSED(dev);
  return false;
#endif /* HOST_SIDE */
}

static const struct kp_rgb_condition_api api = {
    .scope = KP_RGB_CONDITION_CENTRAL_ONLY, .sample = sample};
#define ENDPOINT_TRANSPORT(inst, value) DT_INST_ENUM_HAS_VALUE(inst, transport, value)
#define ENDPOINT_STATE(inst, value) DT_INST_ENUM_HAS_VALUE(inst, state, value)
#define DEFINE(inst)                                                           \
  BUILD_ASSERT(((int32_t)DT_INST_PROP(inst, profile)) >= -1,                   \
               "endpoint profile must be >= -1");                              \
  BUILD_ASSERT(ENDPOINT_TRANSPORT(inst, ble) ||                                \
                   ((int32_t)DT_INST_PROP(inst, profile)) == -1,               \
               "endpoint profile requires BLE");                               \
  BUILD_ASSERT(                                                                \
      ENDPOINT_TRANSPORT(inst, ble) ||                                         \
          (!ENDPOINT_STATE(inst, open) && !ENDPOINT_STATE(inst, any)),         \
      "endpoint open/any requires BLE");                                       \
  BUILD_ASSERT(ENDPOINT_TRANSPORT(inst, usb) ||                                \
                   !ENDPOINT_STATE(inst, powered),                             \
               "endpoint powered requires USB");                               \
  BUILD_ASSERT(!DT_INST_PROP(inst, ignore_routing) ||                          \
                   (ENDPOINT_TRANSPORT(inst, ble) ||                           \
                    (ENDPOINT_TRANSPORT(inst, usb) &&                          \
                     ENDPOINT_STATE(inst, connected))),                        \
               "endpoint ignore-routing requires BLE or USB connected");       \
  BUILD_ASSERT(!HOST_SIDE || !ENDPOINT_TRANSPORT(inst, ble) ||                 \
                   IS_ENABLED(CONFIG_ZMK_BLE),                                 \
               "endpoint BLE requires CONFIG_ZMK_BLE on host side");           \
  BUILD_ASSERT(!HOST_SIDE || !ENDPOINT_TRANSPORT(inst, usb) ||                 \
                   IS_ENABLED(CONFIG_USB_DEVICE_STACK),                        \
               "endpoint USB requires CONFIG_USB_DEVICE_STACK on host side");  \
  BUILD_ASSERT(!HOST_SIDE || !ENDPOINT_TRANSPORT(inst, usb) ||                 \
                   !ENDPOINT_STATE(inst, connected) ||                         \
                   IS_ENABLED(CONFIG_ZMK_USB),                                 \
               "endpoint USB connected requires CONFIG_ZMK_USB on host side"); \
  static const struct endpoint_cfg cfg_##inst = {                              \
      .profile = DT_INST_PROP(inst, profile),                                  \
      .transport = CONV_DT_ENUM(inst, transport),                              \
      .state = CONV_DT_ENUM(inst, state),                                      \
      .ignore_routing = DT_INST_PROP(inst, ignore_routing)};                   \
  KP_RGB_CONDITION_DEFINE(inst, &api, &cfg_##inst, NULL)

DT_INST_FOREACH_STATUS_OKAY(DEFINE)
#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
