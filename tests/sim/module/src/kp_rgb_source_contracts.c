/* SPDX-License-Identifier: MIT */
#include "native_contract.h"
#include <zmk/rgb_matrix.h>
#include <zmk/endpoints.h>
#include <zmk/usb.h>
#include <zmk/battery.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include "rgb_matrix_internal.h"

#if !IS_ENABLED(CONFIG_ZMK_BLE) || (IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
ZMK_EVENT_IMPL(zmk_ble_active_profile_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
ZMK_EVENT_IMPL(zmk_endpoint_changed);
#endif
ZMK_EVENT_IMPL(zmk_usb_conn_state_changed);
ZMK_EVENT_IMPL(zmk_battery_state_changed);

static atomic_t preference = ZMK_TRANSPORT_USB, selected = ZMK_TRANSPORT_NONE;
static atomic_t usb, linked, open_profile = 1, profile, soc, preference_reads;
static atomic_t switch_to_usb_on_open;
enum zmk_transport __wrap_zmk_endpoint_get_preferred_transport(void) {
    atomic_inc(&preference_reads);
    return atomic_get(&preference);
}
struct zmk_endpoint_instance __wrap_zmk_endpoint_get_selected(void) {
    return (struct zmk_endpoint_instance){.transport = atomic_get(&selected)};
}
bool __wrap_zmk_endpoint_is_connected(void) {
    return atomic_get(&selected) != ZMK_TRANSPORT_NONE;
}
enum zmk_usb_conn_state zmk_usb_get_conn_state(void) { return atomic_get(&usb); }
bool zmk_usb_is_hid_ready(void) { return atomic_get(&usb) == ZMK_USB_CONN_HID; }
int __wrap_zmk_ble_active_profile_index(void) { return atomic_get(&profile); }
bool __wrap_zmk_ble_active_profile_is_connected(void) { return atomic_get(&linked); }
bool __wrap_zmk_ble_active_profile_is_open(void) {
    if (atomic_cas(&switch_to_usb_on_open, 1, 0)) {
        atomic_set(&preference, ZMK_TRANSPORT_USB);
    }
    return atomic_get(&open_profile);
}
uint8_t zmk_battery_state_of_charge(void) { return atomic_get(&soc); }

#define DEV(name) DEVICE_DT_GET(DT_NODELABEL(name))
#define GATE(name) gate(DEV(gate_##name))
static bool gate(const struct device *dev) {
    kp_rgb_matrix_lock();
    bool result = kp_rgb_overlay_gate(dev);
    kp_rgb_matrix_unlock();
    return result;
}
K_SEM_DEFINE(drained, 0, 1);
static void drain_work_handler(struct k_work *work) { ARG_UNUSED(work); k_sem_give(&drained); }
K_WORK_DEFINE(drain_work, drain_work_handler);
static void drain(void) {
    for (int i = 0; i < 3; i++) {
        KP_CHECK(k_work_submit_to_queue(kp_rgb_work_q(), &drain_work) >= 0);
        KP_CHECK(k_sem_take(&drained, K_SECONDS(1)) == 0);
        k_msleep(1);
    }
}
static void usb_event(void) {
    raise_zmk_usb_conn_state_changed((struct zmk_usb_conn_state_changed){0});
    drain();
}
#if IS_ENABLED(CONFIG_KEYPAW_SIM_SOURCE_USB_HID)
static void ble_event(void) {
    raise_zmk_ble_active_profile_changed((struct zmk_ble_active_profile_changed){0});
    drain();
}
#endif
static void position(bool pressed) {
    raise_zmk_position_state_changed((struct zmk_position_state_changed){
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL, .position = 0,
        .state = pressed, .timestamp = k_uptime_get()});
}
#if IS_ENABLED(CONFIG_KEYPAW_SIM_SOURCE_USB_HID)
static void endpoint_states(void) {
    KP_CHECK(GATE(ble_open) && GATE(ble_disconnected) && GATE(ble_any));
    KP_CHECK(!GATE(ble_connected) && GATE(usb_disconnected));
    KP_CHECK(!GATE(any_connected) && GATE(any_disconnected));
    KP_CHECK(!GATE(ble_profile) && !GATE(ble_outside));
    atomic_set(&profile, 1); ble_event(); KP_CHECK(GATE(ble_profile));
    atomic_set(&usb, ZMK_USB_CONN_POWERED); usb_event();
    KP_CHECK(GATE(usb_powered) && !GATE(usb_disconnected) && GATE(ble_open));
    atomic_set(&usb, ZMK_USB_CONN_HID); atomic_set(&selected, ZMK_TRANSPORT_USB); usb_event();
    KP_CHECK(GATE(usb_connected) && GATE(usb_physical) && !GATE(usb_powered));
    KP_CHECK(!GATE(ble_open) && !GATE(ble_any) && GATE(ble_physical));
    atomic_set(&preference, ZMK_TRANSPORT_BLE); ble_event();
    KP_CHECK(GATE(usb_connected) && GATE(ble_open) && GATE(ble_disconnected));
    atomic_set(&linked, 1); atomic_set(&open_profile, 0);
    atomic_set(&selected, ZMK_TRANSPORT_BLE); ble_event();
    KP_CHECK(GATE(ble_connected) && !GATE(ble_open) && !GATE(ble_disconnected));
    KP_CHECK(!GATE(usb_connected) && GATE(usb_physical) && GATE(any_connected));
    atomic_set(&preference, ZMK_TRANSPORT_NONE); atomic_set(&selected, ZMK_TRANSPORT_NONE);
    raise_zmk_endpoint_changed((struct zmk_endpoint_changed){0}); drain();
    KP_CHECK(!GATE(ble_connected) && !GATE(ble_any) && !GATE(usb_connected));
    KP_CHECK(GATE(usb_physical) && !GATE(any_connected) && GATE(any_disconnected));
    atomic_set(&usb, ZMK_USB_CONN_POWERED); usb_event(); KP_CHECK(GATE(usb_powered));
    atomic_set(&usb, ZMK_USB_CONN_NONE); usb_event(); KP_CHECK(GATE(usb_disconnected));
}
#else
static void endpoint_states(void) {
    KP_CHECK(GATE(usb_disconnected) && !GATE(usb_powered));
    atomic_set(&usb, ZMK_USB_CONN_POWERED); usb_event();
    KP_CHECK(!GATE(usb_disconnected) && GATE(usb_powered));
    atomic_set(&usb, ZMK_USB_CONN_NONE); usb_event();
    KP_CHECK(GATE(usb_disconnected) && !GATE(usb_powered));
}
#endif
static void preference_events(void) {
    atomic_set(&usb, ZMK_USB_CONN_HID); atomic_set(&selected, ZMK_TRANSPORT_USB); usb_event();
    KP_CHECK(!GATE(ble_open));
    position(true); k_msleep(20); drain();
    unsigned before = atomic_get(&preference_reads);
    for (int i = 0; i < 8; i++) position(true);
    k_msleep(20); drain();
    KP_CHECK(atomic_get(&preference_reads) == before + 1);
    atomic_set(&preference, ZMK_TRANSPORT_BLE);
    k_msleep(50); drain(); KP_CHECK(!GATE(ble_open));
    position(false); k_msleep(20); drain(); KP_CHECK(GATE(ble_open));
    /* A transport reevaluation must update the delayed observer's baseline. */
    atomic_set(&preference, ZMK_TRANSPORT_USB); usb_event(); KP_CHECK(!GATE(ble_open));
    atomic_set(&preference, ZMK_TRANSPORT_BLE);
    position(true); k_msleep(20); drain(); KP_CHECK(GATE(ble_open));
    atomic_set(&preference, ZMK_TRANSPORT_USB);
    int64_t start = k_uptime_get();
    while (k_uptime_get() - start < 30) { position(true); k_msleep(1); }
    /* Continuous input must not postpone the initial ten-millisecond sample. */
    KP_CHECK(!GATE(ble_open));
    k_msleep(20); drain();
    /* Preference changes back midway through a dependency-ordered condition pass. */
    atomic_set(&preference, ZMK_TRANSPORT_BLE);
    atomic_set(&switch_to_usb_on_open, 1);
    usb_event(); KP_CHECK(GATE(ble_open));
    position(false); k_msleep(20); drain(); KP_CHECK(!GATE(ble_open));
    before = atomic_get(&preference_reads);
    k_msleep(100); drain(); KP_CHECK(atomic_get(&preference_reads) == before);
}
#if IS_ENABLED(CONFIG_KEYPAW_SIM_SOURCE_SOC)
static void battery_states(void) {
    KP_CHECK(GATE(battery_low) && !GATE(battery_band) && GATE(battery_discharging));
    const uint8_t values[] = {19, 20, 21, 99, 100};
    for (size_t i = 0; i < ARRAY_SIZE(values); i++) {
        atomic_set(&soc, values[i]);
        raise_zmk_battery_state_changed((struct zmk_battery_state_changed){0}); drain();
        KP_CHECK(GATE(battery_low) == (values[i] < 20));
        KP_CHECK(GATE(battery_band) == (values[i] == 20));
        KP_CHECK(!GATE(battery_full));
    }
    atomic_set(&usb, ZMK_USB_CONN_POWERED); usb_event();
    KP_CHECK(GATE(battery_full) && GATE(battery_charging) && !GATE(battery_discharging));
    atomic_set(&soc, 255);
    raise_zmk_battery_state_changed((struct zmk_battery_state_changed){0}); drain();
    KP_CHECK(GATE(battery_full));
    atomic_set(&usb, ZMK_USB_CONN_HID); usb_event(); KP_CHECK(GATE(battery_charging));
    atomic_set(&soc, 99);
    raise_zmk_battery_state_changed((struct zmk_battery_state_changed){0}); drain();
    KP_CHECK(!GATE(battery_full) && GATE(battery_charging));
    atomic_set(&usb, ZMK_USB_CONN_NONE); usb_event();
    KP_CHECK(!GATE(battery_charging) && GATE(battery_discharging));
}
#else
static void battery_states(void) {
    KP_CHECK(GATE(battery_discharging) && !GATE(battery_charging));
    atomic_set(&usb, ZMK_USB_CONN_POWERED); usb_event();
    KP_CHECK(GATE(battery_charging) && !GATE(battery_discharging));
}
#endif
void kp_test_sources_run(void) {
    k_msleep(30); drain();
    if (kp_test_case("peripheral-scope")) {
        KP_CHECK(!kp_rgb_condition_valid(DEV(ble_open)));
        KP_CHECK(!GATE(ble_open) && !GATE(usb_powered));
        battery_states();
        return;
    }
    if (kp_test_case("endpoint-states")) endpoint_states();
    else if (kp_test_case("preference-events")) preference_events();
    else if (kp_test_case("battery-states")) battery_states();
    else KP_CHECK(false);
}
