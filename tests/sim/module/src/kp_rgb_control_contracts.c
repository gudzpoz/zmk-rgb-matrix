/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#define DT_DRV_COMPAT test_rgb_control_probe
#include <drivers/behavior.h>
#include <errno.h>
#include <sys/types.h>
#include <zephyr/irq_offload.h>
#include <zephyr/init.h>
#include <zmk/rgb_matrix.h>
#include <zmk/rgb_selector.h>
#include <zmk/events/rgb_selector_selection_changed.h>
#include "rgb_matrix_internal.h"
#include "rgb_selector_sync_internal.h"
#include "native_contract.h"

BUILD_ASSERT(KP_RGB_DEVICE_INIT_PRIORITY == 99);
#define DEV(label) DEVICE_DT_GET(DT_NODELABEL(label))
DEFINE_DT_ENUM(control_axis_t, axis, none, vertical, horizontal);
struct control_config { struct kp_rgb_effect_common_config common; control_axis_t axis; };
struct control_data { struct kp_rgb_effect_common_data common; unsigned callbacks, resets, activations, deactivations, events; uint32_t elapsed; int64_t event_time; };
static bool render(const struct device *dev, const struct kp_rgb_frame *frame) {
    struct control_data *data = dev->data;
    KP_CHECK(frame->scratch == NULL);
    data->callbacks++;
    data->elapsed = frame->elapsed_ms;
    for (size_t i = 0; i < frame->target_count; i++) {
        size_t led = frame->targets[i];
        KP_CHECK(frame->pixels[led].r == 0 && frame->pixels[led].g == 0 && frame->pixels[led].b == 0);
        frame->pixels[led] = kp_rgb_hsb_to_rgb(data->common.color);
    }
    return ((const struct control_config *)dev->config)->axis == DT_ENUM_CONST(axis, horizontal);
}
static void reset(const struct device *dev, int64_t now) {
    ARG_UNUSED(now);
    struct control_data *data = dev->data;
    data->callbacks++; data->resets++;
}
static void active(const struct device *dev, bool value, int64_t now) {
    ARG_UNUSED(now);
    struct control_data *data = dev->data;
    data->callbacks++;
    if (value) data->activations++; else data->deactivations++;
}
static bool event(const struct device *dev, const struct kp_rgb_key_event *ev) {
    struct control_data *data = dev->data;
    data->callbacks++; data->events++; data->event_time = ev->timestamp_ms;
    return true;
}
static const struct kp_rgb_effect_callbacks callbacks = {
    .render = render, .reset = reset, .set_active = active, .on_event = event,
};
#define CONTROL_DEFINE(inst) \
    static const struct control_config control_##inst##_cfg = { \
        KP_RGB_EFFECT_COMMON_CONFIG(inst), .axis = CONV_DT_ENUM(inst, axis), \
    }; \
    static struct control_data control_##inst##_data = { KP_RGB_EFFECT_COMMON_DATA(inst, 0, 0) }; \
    KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), &callbacks, control_##inst)
DT_INST_FOREACH_STATUS_OKAY(CONTROL_DEFINE)

DEFINE_DT_ENUM(control_mode_t, mode, idle, pulse, sweep);
_Static_assert(DT_ENUM_CONST(mode, idle) == 0 && DT_ENUM_CONST(mode, pulse) == 1 &&
               DT_ENUM_CONST(mode, sweep) == 2, "first provider enum order");
#if DT_HAS_COMPAT_STATUS_OKAY(test_rgb_control_probe)
static const control_mode_t first_mode = CONV_DT_ENUM(0, mode);
#endif
#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT test_rgb_control_enum
DEFINE_DT_ENUM(control_mode, mode, sweep, idle, pulse);
_Static_assert(DT_ENUM_CONST(mode, sweep) == 0 && DT_ENUM_CONST(mode, idle) == 1 &&
               DT_ENUM_CONST(mode, pulse) == 2, "second provider enum order");
_Static_assert(!__builtin_types_compatible_p(control_mode_t, control_mode),
               "providers must have independent enum types");
#if DT_HAS_COMPAT_STATUS_OKAY(test_rgb_control_enum)
static const control_mode second_mode = CONV_DT_ENUM(0, mode);
#endif
#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT test_rgb_control_probe

static bool user_on(void) {
    bool value;
    KP_CHECK(zmk_rgb_matrix_get_user_state(&value) == 0);
    return value;
}

static void matrix_defaults(void) {
    KP_CHECK(KP_RGB_OVERLAY_COUNT == 0);
    KP_CHECK(kp_rgb_selector_registry_count() == 0);
    KP_CHECK(KP_RGB_OVERLAY_WORDS == 1);
    int32_t initial = (int32_t)DT_PROP_OR(KP_RGB_NODE, initial_brightness, 30);
    KP_CHECK(kp_rgb_config.initial_brightness == CLAMP(initial, 0, 100));
    KP_CHECK(kp_rgb_global.brightness == CLAMP(initial, 0, 100));
    KP_CHECK(kp_rgb_global.user_on == DT_PROP(KP_RGB_NODE, initial_on));
    KP_CHECK(kp_rgb_config.initial_on == DT_PROP(KP_RGB_NODE, initial_on));
    KP_CHECK(kp_rgb_config.max_brightness == DT_PROP_OR(KP_RGB_NODE, max_brightness, 40));
    KP_CHECK(kp_rgb_config.idle_brightness == DT_PROP_OR(KP_RGB_NODE, idle_brightness, 0));
    KP_CHECK(device_is_ready(DEV(rgb_matrix)));
    KP_CHECK(strcmp(DEV(rgb_matrix)->name, "rgb") == 0);
    KP_CHECK(zmk_rgb_matrix_off() == 0);
    kp_rgb_matrix_lock(); kp_rgb_global.brightness = 7; kp_rgb_matrix_unlock();
    kp_rgb_apply_defaults();
    KP_CHECK(user_on() == DT_PROP(KP_RGB_NODE, initial_on));
    KP_CHECK(kp_rgb_global.brightness == CLAMP(initial, 0, 100));
}
#if IS_ENABLED(CONFIG_SETTINGS_CUSTOM)
#include <zephyr/settings/settings.h>
static atomic_t storage_writes;
static int control_load(struct settings_store *store, const struct settings_load_arg *arg) {
    ARG_UNUSED(store); ARG_UNUSED(arg); return 0;
}
static int control_save(struct settings_store *store, const char *name, const char *value, size_t len) {
    ARG_UNUSED(store); ARG_UNUSED(name); ARG_UNUSED(value); ARG_UNUSED(len);
    atomic_inc(&storage_writes); return 0;
}
static const struct settings_store_itf control_store_api = {
    .csi_load = control_load, .csi_save = control_save,
};
static struct settings_store control_store = {.cs_itf = &control_store_api};
int settings_backend_init(void) {
    settings_src_register(&control_store);
    settings_dst_register(&control_store);
    return 0;
}
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/transport/central.h>
static atomic_t transport_connected, selector_sends;
static zmk_split_transport_central_status_changed_cb_t transport_callback;
static int control_send(uint8_t source, struct zmk_split_transport_central_command cmd) {
    KP_CHECK(source == 0);
    KP_CHECK(cmd.type == ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_INVOKE_BEHAVIOR);
    if (!atomic_get(&transport_connected)) return -ENOTCONN;
    if (!strcmp(cmd.data.invoke_behavior.behavior_dev, DEV(rgbs_a)->name) ||
        !strcmp(cmd.data.invoke_behavior.behavior_dev, DEV(rgbs_b)->name)) atomic_inc(&selector_sends);
    return 0;
}
static int control_sources(uint8_t *ids) {
    ids[0] = 0; return atomic_get(&transport_connected) ? 1 : 0;
}
static struct zmk_split_transport_status control_status(void) {
    return (struct zmk_split_transport_status){.available = true, .enabled = true,
        .connections = atomic_get(&transport_connected) ?
            ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_SOME_CONNECTED :
            ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED};
}
static int control_status_callback(zmk_split_transport_central_status_changed_cb_t cb) {
    transport_callback = cb; return 0;
}
static int control_enabled(bool enabled) { KP_CHECK(enabled); return 0; }
static const struct zmk_split_transport_central_api control_transport_api = {
    .send_command = control_send, .get_available_source_ids = control_sources,
    .get_status = control_status, .set_status_callback = control_status_callback,
    .set_enabled = control_enabled,
};
ZMK_SPLIT_TRANSPORT_CENTRAL_REGISTER(control_transport, &control_transport_api, 0);
#endif


#if DT_NODE_EXISTS(DT_NODELABEL(selector_a))
static struct zmk_behavior_binding binding(const struct device *dev, uint32_t cmd, uint32_t arg) {
    return (struct zmk_behavior_binding){.behavior_dev = dev->name, .param1 = cmd, .param2 = arg};
}
static int invoke(const struct device *dev, uint32_t cmd, uint32_t arg) {
    struct zmk_behavior_binding b = binding(dev, cmd, arg);
    return zmk_behavior_invoke_binding(&b, (struct zmk_behavior_binding_event){0}, true);
}
static int convert(struct zmk_behavior_binding *b) {
    return behavior_keymap_binding_convert_central_state_dependent_params(b, (struct zmk_behavior_binding_event){0});
}
static unsigned callback_count(void) {
    return ((struct control_data *)DEV(probe_a)->data)->callbacks +
           ((struct control_data *)DEV(probe_b)->data)->callbacks +
           ((struct control_data *)DEV(probe_c)->data)->callbacks +
           ((struct control_data *)DEV(probe_d)->data)->callbacks;
}
static void wait_ready(void) {
    int64_t until = k_uptime_get() + 1000;
    while (!zmk_rgb_selector_get_selected(DEV(selector_a)) && k_uptime_get() < until) k_msleep(1);
    k_sleep(K_MSEC(50));
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
}
static void canonical_metadata_before_start(void) {
    bool found_a = false, found_b = false;
    for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++) {
        const struct kp_rgb_selector *state = kp_rgb_selector_registry_at(i);
        if (state->owner == DEV(selector_a)) {
            KP_CHECK(state->control == DEV(rgbs_a));
            found_a = true;
        } else if (state->owner == DEV(selector_b)) {
            KP_CHECK(state->control == DEV(rgbs_b));
            found_b = true;
        }
    }
    KP_CHECK(found_a && found_b);
    struct zmk_behavior_binding b = binding(DEV(rgbs_a), RGB_EFF_CMD, 0);
    KP_CHECK(kp_rgb_selector_binding_convert(DEV(selector_a), &b) == -ENODEV);
    KP_CHECK(kp_rgb_selector_binding_convert(DEV(ordinary), &b) == -ENODEV);
    KP_CHECK(kp_rgb_selector_binding_convert(NULL, &b) == -ENODEV);
    struct kp_rgb_selector_snapshot snapshot;
    size_t cursor = 0;
    kp_rgb_matrix_lock();
    KP_CHECK(kp_rgb_selector_overlay_state_locked(DEV(selector_a)) == NULL);
    KP_CHECK(kp_rgb_selector_snapshot_locked(DEV(selector_a), &snapshot) == -ENODEV);
    KP_CHECK(kp_rgb_selector_next_snapshot_locked(DEV(selector_a), &cursor, &snapshot) == -ENODEV);
    KP_CHECK(cursor == 0);
    kp_rgb_matrix_unlock();
}
static void early_prepare(void) {
    canonical_metadata_before_start();
    KP_CHECK(device_is_ready(DEV(probe_a)));
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_a)) == -ENODEV);
    KP_CHECK(zmk_rgb_selector_reset(DEV(selector_a)) == -ENODEV);
    KP_CHECK(kp_rgb_effect_period(DEV(probe_a)) == 1000);
    KP_CHECK(kp_rgb_effect_period(DEV(probe_b)) == CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
    KP_CHECK(kp_rgb_effect_period(DEV(control_unowned)) == 1000);
    KP_CHECK(kp_rgb_effect_cfg(DEV(control_unowned))->owner == NULL);
    struct kp_rgb_hsb color = {123, 45, 67};
    KP_CHECK(zmk_rgb_matrix_set_color(DEV(probe_a), &color) == 0);
    KP_CHECK(zmk_rgb_matrix_set_period(DEV(probe_a), 4321) == 0);
}
static void early(void) {
    wait_ready();
    struct kp_rgb_hsb color = {123, 45, 67};
    KP_CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == 123);
    KP_CHECK(kp_rgb_effect_data(DEV(probe_a))->color.s == 45);
    KP_CHECK(kp_rgb_effect_data(DEV(probe_a))->color.b == 67);
    KP_CHECK(kp_rgb_effect_period(DEV(probe_a)) == 4321);
    KP_CHECK(!device_is_ready(DEV(control_late)));
    KP_CHECK(zmk_rgb_matrix_set_color(DEV(control_late), &color) == -ENODEV);
    KP_CHECK(zmk_rgb_matrix_set_period(DEV(control_late), 1234) == -ENODEV);
    KP_CHECK(device_init(DEV(control_late)) == 0);
    KP_CHECK(zmk_rgb_matrix_set_period(DEV(control_late), 2345) == 0);
    KP_CHECK(kp_rgb_effect_period(DEV(control_late)) == 2345);
}
static void duration_defaults(void) {
    KP_CHECK(kp_rgb_effect_period(DEV(probe_a)) == CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS);
    KP_CHECK(kp_rgb_effect_period(DEV(probe_c)) == CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
    KP_CHECK(kp_rgb_effect_period(DEV(probe_b)) == CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
    wait_ready();
    KP_CHECK(zmk_rgb_matrix_set_period(DEV(probe_a), 2345) == 0);
    KP_CHECK(zmk_rgb_selector_reset(DEV(selector_a)) == 0);
    KP_CHECK(kp_rgb_effect_period(DEV(probe_a)) == CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS);
    KP_CHECK(kp_rgb_effect_period(DEV(probe_c)) == CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
}

static void registry(void) {
    mode_t permissions = 0644;
    KP_CHECK(permissions == 0644 && first_mode == test_rgb_control_probe_mode_pulse);
    KP_CHECK(second_mode == test_rgb_control_enum_mode_sweep);
    wait_ready(); kp_rgb_matrix_lock();
    struct kp_rgb_selector *a = kp_rgb_selector_overlay_state_locked(DEV(selector_a));
    struct kp_rgb_selector *b = kp_rgb_selector_overlay_state_locked(DEV(selector_b));
    KP_CHECK(a && b && a != b);
    KP_CHECK(a->control == DEV(rgbs_a) && b->control == DEV(rgbs_b));
    struct kp_rgb_selector_snapshot snapshot;
    KP_CHECK(kp_rgb_selector_snapshot_locked(DEV(selector_a), &snapshot) == 0);
    KP_CHECK(snapshot.control == DEV(rgbs_a));
    size_t cursor = 0;
    KP_CHECK(kp_rgb_selector_next_snapshot_locked(DEV(selector_b), &cursor, &snapshot) == 1);
    KP_CHECK(snapshot.control == DEV(rgbs_b));
    KP_CHECK(a->effect_count == 3 && b->effect_count == 2);
    KP_CHECK(kp_rgb_selector_at(a, 0) == NULL && kp_rgb_selector_at(a, 3) == NULL);
    KP_CHECK(kp_rgb_selector_at(a, 1) == DEV(probe_a));
    KP_CHECK(kp_rgb_selector_at(a, 2) == DEV(probe_c));
    KP_CHECK(kp_rgb_effect_cfg(DEV(probe_a))->owner == DEV(selector_a));
    KP_CHECK(kp_rgb_effect_cfg(DEV(probe_a))->slot == 1);
    KP_CHECK(kp_rgb_selector_member_slot(a, DEV(probe_a)) == 1);
    KP_CHECK(kp_rgb_selector_member_slot(a, DEV(probe_c)) == 2);
    KP_CHECK(kp_rgb_selector_member_slot(a, DEV(probe_b)) == -ENOENT);
    KP_CHECK(kp_rgb_selector_member_slot(a, DEV(private_white)) == -ENOENT);
    KP_CHECK(kp_rgb_selector_member_slot(a, NULL) == -ENOENT);
    KP_CHECK(kp_rgb_selector_calc_index(a, 1, 1) == 2);
    KP_CHECK(kp_rgb_selector_calc_index(a, 2, 1) == 1);
    KP_CHECK(kp_rgb_selector_calc_index(a, 1, -1) == 2);
    KP_CHECK(kp_rgb_selector_calc_index(a, 0, 0) == 1);
    KP_CHECK(kp_rgb_selector_calc_index(a, 1, INT16_MAX) == 2);
    KP_CHECK(kp_rgb_selector_calc_index(a, 2, INT16_MIN) == 2);
    KP_CHECK(kp_rgb_selector_select_locked(a, 0) == -ENOENT);
    KP_CHECK(kp_rgb_selector_select_locked(a, 3) == -EINVAL);
    KP_CHECK(kp_rgb_selector_select_locked(a, 1) == 0);
    KP_CHECK(kp_rgb_selector_select_locked(a, 2) == 1);
    KP_CHECK(b->active_fx == DEV(probe_b));
    KP_CHECK(((const struct control_config *)DEV(probe_a)->config)->axis == DT_ENUM_CONST(axis, none));
    KP_CHECK(((const struct control_config *)DEV(probe_b)->config)->axis == DT_ENUM_CONST(axis, vertical));
    KP_CHECK(((const struct control_config *)DEV(probe_c)->config)->axis == DT_ENUM_CONST(axis, horizontal));
    kp_rgb_matrix_unlock();
}
static void parameters(void) {
    wait_ready(); KP_CHECK(zmk_rgb_matrix_off() == 0); k_sleep(K_MSEC(30));
    const struct device *targets[] = {DEV(private_white), DEV(probe_c), DEV(control_unowned)};
    struct kp_rgb_hsb c = {360, 100, 0};
    KP_CHECK(zmk_rgb_matrix_set_color(NULL, &c) == -ENODEV);
    KP_CHECK(zmk_rgb_matrix_set_period(NULL, 100) == -ENODEV);
    kp_rgb_matrix_lock(); unsigned before = callback_count();
    for (size_t i = 0; i < ARRAY_SIZE(targets); i++) {
        const struct device *dev = targets[i];
        KP_CHECK(zmk_rgb_matrix_set_color(dev, NULL) == -EINVAL);
        KP_CHECK(zmk_rgb_matrix_set_color(dev, &c) == 0);
        KP_CHECK(kp_rgb_effect_data(dev)->color.h == 0 && kp_rgb_effect_data(dev)->color.s == 100 && kp_rgb_effect_data(dev)->color.b == 0);
        KP_CHECK(zmk_rgb_matrix_set_color(dev, &c) == 0);
        const uint8_t brightnesses[] = {0, 100, 43};
        for (size_t j = 0; j < ARRAY_SIZE(brightnesses); j++) {
            struct kp_rgb_hsb valid = {123, 57, brightnesses[j]};
            uint32_t period = kp_rgb_effect_period(dev);
            KP_CHECK(zmk_rgb_matrix_set_color(dev, &valid) == 0);
            KP_CHECK(kp_rgb_effect_data(dev)->color.h == valid.h &&
                     kp_rgb_effect_data(dev)->color.s == valid.s &&
                     kp_rgb_effect_data(dev)->color.b == valid.b);
            KP_CHECK(kp_rgb_effect_period(dev) == period);
            struct kp_rgb_hsb bad[] = {{361,0,0},{0,101,0},{0,255,0},{0,0,101},{0,0,255}};
            for (size_t k = 0; k < ARRAY_SIZE(bad); k++) {
                KP_CHECK(zmk_rgb_matrix_set_color(dev, &bad[k]) == -EINVAL);
                KP_CHECK(kp_rgb_effect_data(dev)->color.h == valid.h &&
                         kp_rgb_effect_data(dev)->color.s == valid.s &&
                         kp_rgb_effect_data(dev)->color.b == valid.b);
                KP_CHECK(kp_rgb_effect_period(dev) == period);
            }
        }
        uint32_t bad_period[] = {0, CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS - 1,
            CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS + 1, 65536 + 100, 0x80000000, UINT32_MAX};
        for (size_t j = 0; j < ARRAY_SIZE(bad_period); j++) KP_CHECK(zmk_rgb_matrix_set_period(dev, bad_period[j]) == -EINVAL);
        KP_CHECK(zmk_rgb_matrix_set_period(dev, CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS) == 0);
        KP_CHECK(zmk_rgb_matrix_set_period(dev, CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS) == 0);
        KP_CHECK(zmk_rgb_matrix_set_period(dev, CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS) == 0);
        KP_CHECK(kp_rgb_effect_data(dev)->color.h == 123 &&
                 kp_rgb_effect_data(dev)->color.s == 57 &&
                 kp_rgb_effect_data(dev)->color.b == 43);
        KP_CHECK(kp_rgb_effect_period(dev) == CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
    }
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
    KP_CHECK(!user_on() && callback_count() == before);
    kp_rgb_matrix_unlock();
}
static void global_defaults(void) {
    wait_ready(); kp_rgb_matrix_lock();
    struct kp_rgb_hsb c = {47,66,100};
    KP_CHECK(zmk_rgb_matrix_set_color(DEV(probe_a), &c) == 0);
    KP_CHECK(zmk_rgb_matrix_set_period(DEV(probe_c), 2345) == 0);
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_c)) == 0);
    KP_CHECK(invoke(DEV(rgb_matrix), RGB_BRT_SET_CMD, 17) == 0);
    KP_CHECK(zmk_rgb_matrix_off() == 0);
    unsigned count = callback_count();
    kp_rgb_apply_defaults();
    KP_CHECK(kp_rgb_global.brightness == CLAMP((int32_t)DT_PROP_OR(KP_RGB_NODE, initial_brightness, 30), 0, 100));
    KP_CHECK(kp_rgb_global.user_on == DT_PROP(KP_RGB_NODE, initial_on));
    KP_CHECK(kp_rgb_config.max_brightness == 100 && kp_rgb_config.idle_brightness == 100);
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_c));
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
    KP_CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == 47);
    KP_CHECK(kp_rgb_effect_period(DEV(probe_c)) == 2345);
    KP_CHECK(callback_count() == count);
    kp_rgb_matrix_unlock();
}

static void no_op(void) {
    wait_ready();
    kp_rgb_matrix_lock();
    unsigned count = callback_count();
    struct kp_rgb_hsb c = kp_rgb_effect_data(DEV(probe_a))->color;
    KP_CHECK(zmk_rgb_matrix_set_color(DEV(probe_a), &c) == 0);
    KP_CHECK(zmk_rgb_matrix_set_period(DEV(probe_a), kp_rgb_effect_period(DEV(probe_a))) == 0);
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_a)) == 0);
    KP_CHECK(zmk_rgb_matrix_set_period(DEV(probe_a), UINT32_MAX) == -EINVAL);
    kp_rgb_matrix_unlock(); k_sleep(K_MSEC(50));
    kp_rgb_matrix_lock(); KP_CHECK(callback_count() == count); kp_rgb_matrix_unlock();
    c.b = c.b == 42 ? 43 : 42;
    KP_CHECK(zmk_rgb_matrix_set_color(DEV(probe_a), &c) == 0);
    k_sleep(K_MSEC(50));
    kp_rgb_matrix_lock(); KP_CHECK(callback_count() > count); kp_rgb_matrix_unlock();
    kp_rgb_matrix_lock();
    KP_CHECK(kp_rgb_effect_data(DEV(probe_a))->color.b == c.b);
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
    count = callback_count();
    KP_CHECK(zmk_rgb_matrix_set_color(DEV(probe_a), &c) == 0);
    kp_rgb_matrix_unlock(); k_sleep(K_MSEC(50));
    kp_rgb_matrix_lock(); KP_CHECK(callback_count() == count); kp_rgb_matrix_unlock();
    c.h = (c.h + 1) % 360;
    KP_CHECK(zmk_rgb_matrix_set_color(DEV(probe_a), &c) == 0);
    k_sleep(K_MSEC(50));
    kp_rgb_matrix_lock(); KP_CHECK(callback_count() > count); kp_rgb_matrix_unlock();
}

static void converted(uint32_t command, uint32_t absolute, uint32_t value) {
    struct zmk_behavior_binding b = binding(DEV(rgbs_a), command, 0);
    struct kp_rgb_effect_common_data before = *kp_rgb_effect_data(DEV(probe_a));
    unsigned count = callback_count();
    KP_CHECK(convert(&b) == 0);
    KP_CHECK(strcmp(b.behavior_dev, DEV(rgbs_a)->name) == 0);
    KP_CHECK(b.param1 == absolute && b.param2 == value);
    KP_CHECK(memcmp(&before, kp_rgb_effect_data(DEV(probe_a)), sizeof(before)) == 0);
    KP_CHECK(callback_count() == count);
    struct kp_rgb_hsb diverged = {111,1,19};
    KP_CHECK(zmk_rgb_matrix_set_color(DEV(probe_a), &diverged) == 0);
    KP_CHECK(zmk_rgb_matrix_set_period(DEV(probe_a), 7777) == 0);
    KP_CHECK(behavior_keymap_binding_pressed(&b, (struct zmk_behavior_binding_event){0}) == 0);
    uint32_t kind = absolute & KP_RGB_SELECTOR_COMMAND_MASK;
    if (kind == RGB_SELECTOR_COLOR_CMD) {
        KP_CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == (value >> 16));
        KP_CHECK(kp_rgb_effect_data(DEV(probe_a))->color.s == ((value >> 8) & 255));
        KP_CHECK(kp_rgb_effect_data(DEV(probe_a))->color.b == (value & 255));
        KP_CHECK(kp_rgb_effect_period(DEV(probe_a)) == 7777);
        KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
    } else {
        KP_CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == diverged.h);
        KP_CHECK(kp_rgb_effect_data(DEV(probe_a))->color.s == diverged.s);
        KP_CHECK(kp_rgb_effect_data(DEV(probe_a))->color.b == diverged.b);
        if (kind == RGB_SELECTOR_PERIOD_CMD) {
            KP_CHECK(kp_rgb_effect_period(DEV(probe_a)) == value);
            KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
        } else {
            KP_CHECK(kp_rgb_effect_period(DEV(probe_a)) == 7777);
            KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_c));
        }
    }
}
static void normalize_commands(void) {
    wait_ready(); kp_rgb_matrix_lock();
    const uint32_t commands[][2] = {
        {RGB_HUI_CMD, 0}, {RGB_HUD_CMD, 0}, {RGB_SAI_CMD, 0}, {RGB_SAD_CMD, 0},
        {RGB_SPI_CMD, 0}, {RGB_SPD_CMD, 0}, {RGB_SPI_CMD, 2400},
        {RGB_COLOR_HSB_CMD, RGB_COLOR_HSB_VAL(17, 43, 61)},
    };
    for (size_t i = 0; i < ARRAY_SIZE(commands); i++) {
        KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_a)) == 0);
        struct kp_rgb_effect_common_data before_a = *kp_rgb_effect_data(DEV(probe_a));
        struct kp_rgb_effect_common_data before_b = *kp_rgb_effect_data(DEV(probe_b));
        struct kp_rgb_effect_common_data before_c = *kp_rgb_effect_data(DEV(probe_c));
        struct zmk_behavior_binding b = binding(DEV(rgbs_a), commands[i][0], commands[i][1]);
        bool period = commands[i][0] == RGB_SPI_CMD || commands[i][0] == RGB_SPD_CMD;
        KP_CHECK(convert(&b) == 0);
        KP_CHECK(b.param1 == KP_RGB_SELECTOR_MEMBER_COMMAND(
            period ? RGB_SELECTOR_PERIOD_CMD : RGB_SELECTOR_COLOR_CMD, 1));
        KP_CHECK(memcmp(&before_a, kp_rgb_effect_data(DEV(probe_a)), sizeof(before_a)) == 0);
        struct zmk_behavior_binding canonical = b;
        KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_c)) == 0);
        KP_CHECK(convert(&b) == 0);
        KP_CHECK(b.param1 == canonical.param1 && b.param2 == canonical.param2 &&
                 strcmp(b.behavior_dev, canonical.behavior_dev) == 0);
        KP_CHECK(behavior_keymap_binding_pressed(&b, (struct zmk_behavior_binding_event){0}) == 0);
        const struct kp_rgb_effect_common_data *after_a = kp_rgb_effect_data(DEV(probe_a));
        if (period) {
            KP_CHECK(after_a->duration_ms == b.param2);
            KP_CHECK(memcmp(&after_a->color, &before_a.color, sizeof(before_a.color)) == 0);
        } else {
            KP_CHECK(after_a->color.h == (b.param2 >> 16));
            KP_CHECK(after_a->color.s == ((b.param2 >> 8) & 255));
            KP_CHECK(after_a->color.b == (b.param2 & 255));
            KP_CHECK(after_a->duration_ms == before_a.duration_ms);
        }
        KP_CHECK(memcmp(&before_b, kp_rgb_effect_data(DEV(probe_b)), sizeof(before_b)) == 0);
        KP_CHECK(memcmp(&before_c, kp_rgb_effect_data(DEV(probe_c)), sizeof(before_c)) == 0);
        KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_c));
        struct kp_rgb_effect_common_data captured_a = *after_a;
        KP_CHECK(invoke(DEV(rgbs_a), RGB_HUI_CMD, 0) == 0);
        KP_CHECK(kp_rgb_effect_data(DEV(probe_c))->color.h ==
                 (before_c.color.h + CONFIG_KEYPAW_RGB_MATRIX_HUE_STEP) % 360);
        KP_CHECK(memcmp(&captured_a, kp_rgb_effect_data(DEV(probe_a)), sizeof(captured_a)) == 0);
    }
    kp_rgb_matrix_unlock();
}
static void conversion(void) {
    wait_ready(); kp_rgb_matrix_lock();
    struct kp_rgb_hsb c = {350,95,37};
    KP_CHECK(zmk_rgb_matrix_set_color(DEV(probe_a), &c) == 0);
    converted(RGB_HUI_CMD, KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 1),
        RGB_COLOR_HSB_VAL((350 + CONFIG_KEYPAW_RGB_MATRIX_HUE_STEP) % 360,95,37));
    KP_CHECK(zmk_rgb_matrix_set_color(DEV(probe_a), &c) == 0);
    converted(RGB_HUD_CMD, KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 1),
        RGB_COLOR_HSB_VAL((350 + 360 - CONFIG_KEYPAW_RGB_MATRIX_HUE_STEP) % 360,95,37));
    KP_CHECK(zmk_rgb_matrix_set_color(DEV(probe_a), &c) == 0);
    converted(RGB_SAI_CMD, KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 1), RGB_COLOR_HSB_VAL(350,100,37));
    c.s = 2; KP_CHECK(zmk_rgb_matrix_set_color(DEV(probe_a), &c) == 0);
    converted(RGB_SAD_CMD, KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 1), RGB_COLOR_HSB_VAL(350,0,37));
    KP_CHECK(zmk_rgb_matrix_set_period(DEV(probe_a), CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS - 1) == 0);
    converted(RGB_SPI_CMD, KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, 1), CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
    KP_CHECK(zmk_rgb_matrix_set_period(DEV(probe_a), CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS + 1) == 0);
    converted(RGB_SPD_CMD, KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, 1), CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS);
    converted(RGB_EFF_CMD, RGB_EFS_CMD, 2);
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_a)) == 0);
    converted(RGB_EFR_CMD, RGB_EFS_CMD, 2);
    KP_CHECK(device_is_ready(DEV(probe_a)) && device_is_ready(DEV(private_white)));
    KP_CHECK(zmk_behavior_get_binding(DEV(probe_a)->name) == NULL);
    KP_CHECK(zmk_behavior_get_binding(DEV(private_white)->name) == NULL);
    KP_CHECK(zmk_behavior_get_binding(DEV(rgbs_a)->name) == DEV(rgbs_a));
    KP_CHECK(kp_rgb_selector_overlay_state_locked(DEV(selector_a))->control == DEV(rgbs_a));
    KP_CHECK(kp_rgb_selector_overlay_state_locked(DEV(selector_b))->control == DEV(rgbs_b));
    enum behavior_locality locality;
    KP_CHECK(behavior_get_locality(DEV(rgbs_a), &locality) == 0 && locality == BEHAVIOR_LOCALITY_GLOBAL);
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    struct behavior_parameter_metadata metadata;
    KP_CHECK(behavior_get_parameter_metadata(DEV(rgbs_a), &metadata) == 0 && metadata.sets_len == 4);
    KP_CHECK(metadata.sets[1].param2_values[0].range.max == UINT16_MAX);
    for (size_t i = 0; i < metadata.sets_len; i++) for (size_t j = 0; j < metadata.sets[i].param1_values_len; j++) {
        uint32_t cmd = metadata.sets[i].param1_values[j].value;
        KP_CHECK(cmd != RGB_TOG_CMD && cmd != RGB_BRI_CMD && cmd != RGB_OFF_CMD);
    }
#endif
    kp_rgb_matrix_unlock();
}
static void validation(void) {
    wait_ready(); kp_rgb_matrix_lock();
    const uint32_t noarg[] = {RGB_HUI_CMD,RGB_HUD_CMD,RGB_SAI_CMD,RGB_SAD_CMD,RGB_EFF_CMD,RGB_EFR_CMD,RGB_SPD_CMD,RGB_RESET_CMD};
    for (size_t i = 0; i < ARRAY_SIZE(noarg); i++) {
        KP_CHECK(invoke(DEV(rgbs_a), noarg[i], 1) == -EINVAL);
        KP_CHECK(invoke(DEV(rgbs_a), noarg[i], UINT32_MAX) == -EINVAL);
    }
    const uint32_t slots[] = {3,UINT16_MAX,65536,UINT32_MAX};
    for (size_t i = 0; i < ARRAY_SIZE(slots); i++) KP_CHECK(invoke(DEV(rgbs_a), RGB_EFS_CMD, slots[i]) == -EINVAL);
    KP_CHECK(invoke(DEV(rgbs_a), RGB_EFS_CMD, 0) == -ENOENT);
    const uint32_t colors[] = {RGB_COLOR_HSB_VAL(361,0,0), RGB_COLOR_HSB_VAL(0,101,0), RGB_COLOR_HSB_VAL(0,0,101),RGB_COLOR_HSB_VAL(0,0,255),RGB_COLOR_HSB_VAL(0,255,0),UINT32_MAX,0x80000000};
    struct kp_rgb_effect_common_data unchanged = *kp_rgb_effect_data(DEV(probe_a));
    for (size_t i = 0; i < ARRAY_SIZE(colors); i++) {
        KP_CHECK(invoke(DEV(rgbs_a), RGB_COLOR_HSB_CMD, colors[i]) == -EINVAL);
        KP_CHECK(memcmp(&unchanged, kp_rgb_effect_data(DEV(probe_a)), sizeof(unchanged)) == 0);
        KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
    }
    const uint32_t periods[] = {CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS-1,CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS+1,65536+100,0x80000000,UINT32_MAX};
    for (size_t i = 0; i < ARRAY_SIZE(periods); i++) KP_CHECK(invoke(DEV(rgbs_a), RGB_SPI_CMD, periods[i]) == -EINVAL);
    KP_CHECK(invoke(DEV(rgbs_a), RGB_BRI_CMD, 0) == -ENOTSUP);
    KP_CHECK(invoke(DEV(rgbs_a), UINT32_MAX, 0) == -ENOTSUP);
    KP_CHECK(invoke(DEV(rgbs_a), RGB_COLOR_HSB_CMD, RGB_COLOR_HSB_VAL(360,100,0)) == 0);
    KP_CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == 0 && kp_rgb_effect_data(DEV(probe_a))->color.b == 0);
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), NULL) == -EINVAL);
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_b)) == -EINVAL);
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(private_white)) == -EINVAL);
    KP_CHECK(zmk_rgb_selector_get_selected(NULL) == NULL);
    KP_CHECK(zmk_rgb_selector_select(NULL, DEV(probe_a)) == -ENODEV);
    KP_CHECK(zmk_rgb_selector_reset(NULL) == -ENODEV);
    KP_CHECK(kp_rgb_selector_binding_apply(NULL, RGB_RESET_CMD, 0) == -ENODEV);
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(ordinary)) == NULL);
    KP_CHECK(zmk_rgb_selector_select(DEV(ordinary), DEV(probe_a)) == -ENODEV);
    KP_CHECK(zmk_rgb_selector_reset(DEV(ordinary)) == -ENODEV);
    kp_rgb_matrix_unlock();
}
static void canonical_validation(void) {
    wait_ready(); kp_rgb_matrix_lock();
    const uint32_t slots[] = {0, 3, UINT16_MAX};
    const uint32_t bad_colors[] = {RGB_COLOR_HSB_VAL(361,0,0), RGB_COLOR_HSB_VAL(0,101,0),
        RGB_COLOR_HSB_VAL(0,0,101), UINT32_MAX, 0x80000000};
    const uint32_t bad_periods[] = {0, CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS - 1,
        CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS + 1, 65536 + 100, 0x80000000, UINT32_MAX};
    struct kp_rgb_effect_common_data before_a = *kp_rgb_effect_data(DEV(probe_a));
    struct kp_rgb_effect_common_data before_c = *kp_rgb_effect_data(DEV(probe_c));
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_a)) == 0);
    for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
        int expected = slots[i] == 0 ? -ENOENT : -EINVAL;
        uint32_t cmd = KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, slots[i]);
        struct zmk_behavior_binding b = binding(DEV(rgbs_a), cmd, RGB_COLOR_HSB_VAL(22, 33, 44));
        KP_CHECK(convert(&b) == expected);
        KP_CHECK(behavior_keymap_binding_pressed(&b, (struct zmk_behavior_binding_event){0}) == expected);
        cmd = KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, slots[i]);
        b = binding(DEV(rgbs_a), cmd, CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS);
        KP_CHECK(convert(&b) == expected);
        KP_CHECK(behavior_keymap_binding_pressed(&b, (struct zmk_behavior_binding_event){0}) == expected);
    }
    for (size_t i = 0; i < ARRAY_SIZE(bad_colors); i++) {
        struct zmk_behavior_binding b = binding(DEV(rgbs_a),
            KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 1), bad_colors[i]);
        KP_CHECK(convert(&b) == -EINVAL);
        KP_CHECK(behavior_keymap_binding_pressed(&b, (struct zmk_behavior_binding_event){0}) == -EINVAL);
    }
    for (size_t i = 0; i < ARRAY_SIZE(bad_periods); i++) {
        struct zmk_behavior_binding b = binding(DEV(rgbs_a),
            KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, 1), bad_periods[i]);
        KP_CHECK(convert(&b) == -EINVAL);
        KP_CHECK(behavior_keymap_binding_pressed(&b, (struct zmk_behavior_binding_event){0}) == -EINVAL);
    }
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
    KP_CHECK(memcmp(&before_a, kp_rgb_effect_data(DEV(probe_a)), sizeof(before_a)) == 0);
    KP_CHECK(memcmp(&before_c, kp_rgb_effect_data(DEV(probe_c)), sizeof(before_c)) == 0);
    KP_CHECK(invoke(DEV(rgbs_a), RGB_RESET_CMD, 1) == -EINVAL);
    KP_CHECK(invoke(DEV(rgbs_a), RGB_EFS_CMD, UINT32_MAX) == -EINVAL);
    KP_CHECK(kp_rgb_effect_period(DEV(probe_a)) == before_a.duration_ms);
    kp_rgb_matrix_unlock();
}
static void global_commands(void) {
    wait_ready(); kp_rgb_matrix_lock();
    KP_CHECK(strcmp(DEV(rgb_matrix)->name, "rgb") == 0);
    enum behavior_locality locality;
    KP_CHECK(behavior_get_locality(DEV(rgb_matrix), &locality) == 0);
    KP_CHECK(locality == BEHAVIOR_LOCALITY_GLOBAL);
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    struct behavior_parameter_metadata metadata;
    KP_CHECK(behavior_get_parameter_metadata(DEV(rgb_matrix), &metadata) == 0);
    const uint32_t advertised[] = {RGB_TOG_CMD, RGB_ON_CMD, RGB_OFF_CMD,
                                  RGB_BRI_CMD, RGB_BRD_CMD, RGB_RESET_CMD};
    unsigned seen = 0;
    for (size_t i = 0; i < metadata.sets_len; i++) {
        for (size_t j = 0; j < metadata.sets[i].param1_values_len; j++) {
            uint32_t command = metadata.sets[i].param1_values[j].value;
            bool found = false;
            for (size_t k = 0; k < ARRAY_SIZE(advertised); k++) {
                if (command == advertised[k]) { found = true; seen |= BIT(k); }
            }
            KP_CHECK(found);
        }
    }
    KP_CHECK(seen == BIT(ARRAY_SIZE(advertised)) - 1);
#endif
    const uint32_t commands[] = {
        RGB_HUI_CMD, RGB_HUD_CMD, RGB_SAI_CMD, RGB_SAD_CMD, RGB_SPI_CMD, RGB_SPD_CMD,
        RGB_EFS_CMD, RGB_EFF_CMD, RGB_EFR_CMD, RGB_COLOR_HSB_CMD,
        KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 1),
        KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, 1),
    };
    unsigned callbacks_before = callback_count();
    struct kp_rgb_hsb color_before = kp_rgb_effect_data(DEV(probe_a))->color;
    uint32_t period_before = kp_rgb_effect_period(DEV(probe_a));
    for (size_t i = 0; i < ARRAY_SIZE(commands); i++) {
        struct zmk_behavior_binding b = binding(DEV(rgb_matrix), commands[i], 0);
        KP_CHECK(convert(&b) == -ENOTSUP);
        KP_CHECK(strcmp(b.behavior_dev, DEV(rgb_matrix)->name) == 0 && b.param1 == commands[i] && b.param2 == 0);
        KP_CHECK(invoke(DEV(rgb_matrix), commands[i], 0) == -ENOTSUP);
    }
    KP_CHECK(callback_count() == callbacks_before);
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
    KP_CHECK(memcmp(&kp_rgb_effect_data(DEV(probe_a))->color, &color_before, sizeof(color_before)) == 0);
    KP_CHECK(kp_rgb_effect_period(DEV(probe_a)) == period_before);
    kp_rgb_global.brightness = 17;
    struct zmk_behavior_binding b = binding(DEV(rgb_matrix), RGB_BRI_CMD, 0);
    KP_CHECK(convert(&b) == 0 && b.param1 == RGB_BRT_SET_CMD && b.param2 == 17 + CONFIG_KEYPAW_RGB_MATRIX_BRT_STEP);
    KP_CHECK(kp_rgb_global.brightness == 17);
    KP_CHECK(behavior_keymap_binding_pressed(&b, (struct zmk_behavior_binding_event){0}) == 0);
    KP_CHECK(kp_rgb_global.brightness == b.param2);
    KP_CHECK(invoke(DEV(rgb_matrix), RGB_BRT_SET_CMD, UINT32_MAX) == -EINVAL);
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
    kp_rgb_matrix_unlock();
}
static void scoped_reset(void) {
    wait_ready(); KP_CHECK(zmk_rgb_matrix_off() == 0); k_sleep(K_MSEC(30));
    kp_rgb_matrix_lock();
    unsigned count = callback_count();
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_c)) == 0);
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_b), DEV(probe_d)) == 0);
    KP_CHECK(zmk_rgb_matrix_set_period(DEV(probe_c), 2000) == 0);
    KP_CHECK(zmk_rgb_matrix_set_period(DEV(probe_d), 3000) == 0);
    KP_CHECK(zmk_rgb_matrix_set_period(DEV(private_white), 3456) == 0);
    KP_CHECK(zmk_rgb_selector_reset(DEV(selector_a)) == 0);
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
    KP_CHECK(kp_rgb_effect_period(DEV(probe_c)) == 1000);
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_d));
    KP_CHECK(kp_rgb_effect_period(DEV(probe_d)) == 3000);
    KP_CHECK(kp_rgb_effect_period(DEV(private_white)) == 3456);
    KP_CHECK(callback_count() == count);
    KP_CHECK(invoke(DEV(rgb_matrix), RGB_RESET_CMD, 1) == -EINVAL);
    KP_CHECK(invoke(DEV(rgb_matrix), RGB_RESET_CMD, 0) == 0);
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
    KP_CHECK(kp_rgb_effect_period(DEV(probe_d)) == CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
    KP_CHECK(kp_rgb_effect_period(DEV(private_white)) == 3456);
    KP_CHECK(callback_count() == count);
    kp_rgb_matrix_unlock();
}
static atomic_t events_a, events_b;
static void check_reset_parameters(void) {
    const struct kp_rgb_effect_common_data *a = kp_rgb_effect_data(DEV(probe_a));
    const struct kp_rgb_effect_common_data *c = kp_rgb_effect_data(DEV(probe_c));
    KP_CHECK(a->color.h == 120 && a->color.s == 100 && a->color.b == 100);
    KP_CHECK(c->color.h == 240 && c->color.s == 100 && c->color.b == 100);
    KP_CHECK(a->duration_ms == 1000 && c->duration_ms == 1000);
}
static void prestart_reset_prepare(void) {
    struct kp_rgb_selector *a = NULL;
    for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++) {
        struct kp_rgb_selector *candidate = kp_rgb_selector_registry_at(i);
        if (candidate->owner == DEV(selector_a)) a = candidate;
    }
    KP_CHECK(a != NULL && a->effect_count == 3 && a->initial_effect == 1);
    kp_rgb_matrix_lock();
    unsigned callbacks = callback_count();
    unsigned publications = atomic_get(&events_a);
    KP_CHECK(kp_rgb_selector_select_locked(a, 2) == 1);
    struct kp_rgb_hsb changed = {31, 41, 51};
    kp_rgb_effect_data(DEV(probe_a))->color = changed;
    kp_rgb_effect_data(DEV(probe_c))->color = changed;
    kp_rgb_effect_data(DEV(probe_a))->duration_ms = 4321;
    kp_rgb_effect_data(DEV(probe_c))->duration_ms = 4321;
    kp_rgb_selector_overlays_defaults_locked();
    KP_CHECK(a->effect_index == a->initial_effect && a->active_fx == NULL);
    check_reset_parameters();
    KP_CHECK(!a->publish_changes && zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
    KP_CHECK(!a->notification_pending && callback_count() == callbacks);
    KP_CHECK(atomic_get(&events_a) == publications);
    kp_rgb_matrix_unlock();
}
static void reset_fallback(void) {
    wait_ready();
    KP_CHECK(zmk_rgb_matrix_off() == 0);
    uint16_t start = kp_test_case("control-reset-fallback-noop") ? 0 : 2;
    kp_rgb_matrix_lock();
    struct kp_rgb_selector *a = kp_rgb_selector_overlay_state_locked(DEV(selector_a));
    KP_CHECK(a != NULL && a->initial_effect == 1);
    KP_CHECK(kp_rgb_selector_select_locked(a, start) >= 0);
    kp_rgb_matrix_unlock();
    k_sleep(K_MSEC(50));
    kp_rgb_matrix_lock();
    KP_CHECK(!a->notification_pending);
    unsigned callbacks = callback_count();
    unsigned publications = atomic_get(&events_a);
    struct kp_rgb_hsb changed = {31, 41, 51};
    kp_rgb_effect_data(DEV(probe_a))->color = changed;
    kp_rgb_effect_data(DEV(probe_c))->color = changed;
    kp_rgb_effect_data(DEV(probe_a))->duration_ms = 4321;
    kp_rgb_effect_data(DEV(probe_c))->duration_ms = 4321;
    KP_CHECK(zmk_rgb_selector_reset(DEV(selector_a)) == 0);
    KP_CHECK(a->active_fx == DEV(probe_a) && a->effect_index == 0);
    check_reset_parameters();
    KP_CHECK(callback_count() == callbacks && atomic_get(&events_a) == publications);
    KP_CHECK(a->notification_pending == (start == 2));
    kp_rgb_matrix_unlock();
    k_sleep(K_MSEC(50));
    KP_CHECK(atomic_get(&events_a) == publications + (start == 2 ? 1 : 0));
}
static void reset_prestart_result(void) {
    wait_ready();
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
    check_reset_parameters();
    KP_CHECK(atomic_get(&events_a) == 1);
}

static bool reenter;
static int listener(const zmk_event_t *eh) {
    const struct zmk_rgb_selector_selection_changed *ev = as_zmk_rgb_selector_selection_changed(eh);
    KP_CHECK(ev != NULL && !k_is_in_isr());
    if (ev->selector == DEV(selector_a)) {
        KP_CHECK(zmk_rgb_selector_get_selected(ev->selector) != NULL);
        atomic_inc(&events_a);
        if (reenter) {
            reenter = false;
            KP_CHECK(zmk_rgb_selector_select(ev->selector, DEV(probe_a)) == 0);
        }
    } else if (ev->selector == DEV(selector_b)) atomic_inc(&events_b);
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(control_contract_listener, listener);
ZMK_SUBSCRIPTION(control_contract_listener, zmk_rgb_selector_selection_changed);
static void publication(void) {
    wait_ready(); KP_CHECK(atomic_get(&events_a) == 1 && atomic_get(&events_b) == 1);
    kp_rgb_matrix_lock();
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_c)) == 0);
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_a)) == 0);
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_c)) == 0);
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_b), DEV(probe_d)) == 0);
    KP_CHECK(atomic_get(&events_a) == 1 && atomic_get(&events_b) == 1);
    kp_rgb_matrix_unlock(); k_sleep(K_MSEC(50));
    KP_CHECK(atomic_get(&events_a) == 2 && atomic_get(&events_b) == 2);
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_c));
    kp_rgb_matrix_lock();
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_a)) == 0);
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_c)) == 0);
    kp_rgb_matrix_unlock(); k_sleep(K_MSEC(50));
    KP_CHECK(atomic_get(&events_a) == 3);
    kp_rgb_matrix_lock();
    struct kp_rgb_selector *a = kp_rgb_selector_overlay_state_locked(DEV(selector_a));
    a->publish_changes = false;
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_a)) == 0);
    KP_CHECK(!a->notification_pending);
    kp_rgb_matrix_unlock(); k_sleep(K_MSEC(50));
    KP_CHECK(atomic_get(&events_a) == 3);
    kp_rgb_matrix_lock(); a->publish_changes = true; reenter = true;
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_c)) == 0);
    kp_rgb_matrix_unlock(); k_sleep(K_MSEC(50));
    KP_CHECK(atomic_get(&events_a) == 5 && !reenter);
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
}
static void owner_callbacks(void) {
    wait_ready(); KP_CHECK(zmk_rgb_matrix_off() == 0); k_sleep(K_MSEC(50));
    kp_rgb_matrix_lock();
    const struct kp_rgb_effect_callbacks *a = ((const struct kp_rgb_overlay_api *)DEV(selector_a)->api)->callbacks;
    const struct kp_rgb_effect_callbacks *b = ((const struct kp_rgb_overlay_api *)DEV(selector_b)->api)->callbacks;
    struct control_data *a0 = DEV(probe_a)->data, *a1 = DEV(probe_c)->data, *b0 = DEV(probe_b)->data;
    struct led_rgb pixels[KP_LED_COUNT], scratch[KP_LED_COUNT];
    memset(pixels, 0, sizeof(pixels)); memset(scratch, 0xcc, sizeof(scratch));
    const size_t targets_a[] = {0,1}, targets_b[] = {1,2};
    struct kp_rgb_frame frame = {.count = KP_LED_COUNT, .pixels = pixels, .scratch = scratch,
        .targets = targets_a, .target_count = 2, .local_ms = 1000, .elapsed_ms = 0};
    a->set_active(DEV(selector_a), true, 1000);
    unsigned old_deactivations = a0->deactivations, new_resets = a1->resets;
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_c)) == 0);
    KP_CHECK(a0->deactivations == old_deactivations && a1->resets == new_resets);
    struct kp_rgb_key_event ev = {.position = 3, .pressed = true, .timestamp_ms = INT64_C(0x100000009)};
    KP_CHECK(a->on_event(DEV(selector_a), &ev));
    KP_CHECK(a0->deactivations == old_deactivations + 1 && a1->resets == new_resets + 1);
    KP_CHECK(a1->events == 1 && a1->event_time == ev.timestamp_ms);
    KP_CHECK(a->render(DEV(selector_a), &frame)); KP_CHECK(a1->elapsed == 0);
    frame.local_ms = 1050; frame.elapsed_ms = 50;
    KP_CHECK(a->render(DEV(selector_a), &frame)); KP_CHECK(a1->elapsed == 50);
    frame.local_ms = 1100; frame.elapsed_ms = 0;
    KP_CHECK(a->render(DEV(selector_a), &frame)); KP_CHECK(a1->elapsed == 0);
    unsigned skipped_resets = a0->resets;
    a->reset(DEV(selector_a), 1100);
    KP_CHECK(a1->resets == new_resets + 2 && a0->resets == skipped_resets);
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_a)) == 0);
    KP_CHECK(a->on_event(DEV(selector_a), &ev));
    KP_CHECK(a0->resets == skipped_resets + 1);
    memset(pixels, 0, sizeof(pixels)); frame.local_ms = 1200;
    KP_CHECK(!a->render(DEV(selector_a), &frame)); KP_CHECK(a0->elapsed == 0);
    KP_CHECK(pixels[0].g == 127 && pixels[1].g == 127 && pixels[2].g == 0);
    frame.targets = targets_b; b->set_active(DEV(selector_b), true, 1200);
    KP_CHECK(!b->render(DEV(selector_b), &frame)); KP_CHECK(b0->elapsed == 0);
    KP_CHECK(pixels[0].g == 127 && pixels[1].g == 191 && pixels[2].g == 127);
    KP_CHECK(pixels[0].r == 0 && pixels[1].r == 127 && pixels[2].r == 127);
    KP_CHECK(pixels[3].r == 0 && pixels[3].g == 0 && pixels[3].b == 0);
    a->set_active(DEV(selector_a), false, 1300);
    skipped_resets = a0->resets;
    a->reset(DEV(selector_a), 1400); KP_CHECK(a0->resets == skipped_resets);
    a->set_active(DEV(selector_a), true, 1500); KP_CHECK(a0->resets == skipped_resets + 1);
    a->set_active(DEV(selector_a), false, 1600); b->set_active(DEV(selector_b), false, 1600);
    kp_rgb_matrix_unlock();
}

static void local_only(void) {
#if IS_ENABLED(CONFIG_SETTINGS_CUSTOM) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    wait_ready();
    KP_CHECK(transport_callback != NULL);
    atomic_set(&transport_connected, 1);
    KP_CHECK(transport_callback(&control_transport, control_status()) == 0);
    k_sleep(K_MSEC(1500));
    KP_CHECK(atomic_get(&selector_sends) > 0);
    atomic_val_t sends = atomic_get(&selector_sends), writes = atomic_get(&storage_writes);
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_c)) == 0);
    KP_CHECK(zmk_rgb_selector_reset(DEV(selector_a)) == 0);
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_c)) == 0);
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_a)) == 0);
    struct kp_rgb_hsb c = {17,63,100};
    KP_CHECK(zmk_rgb_matrix_set_color(DEV(probe_c), &c) == 0);
    KP_CHECK(zmk_rgb_matrix_set_period(DEV(probe_c), 3456) == 0);
    struct zmk_behavior_binding b = binding(DEV(rgbs_a), RGB_HUI_CMD, 0);
    KP_CHECK(convert(&b) == 0);
    k_sleep(K_MSEC(1500));
    KP_CHECK(atomic_get(&selector_sends) == sends);
    KP_CHECK(atomic_get(&storage_writes) == writes);
    KP_CHECK(invoke(DEV(rgbs_a), RGB_EFS_CMD, 2) == 0);
    k_sleep(K_MSEC(1500));
    KP_CHECK(atomic_get(&selector_sends) > sends);
    KP_CHECK(atomic_get(&storage_writes) > writes);
#else
    KP_CHECK(false);
#endif
}

static void isr(const void *arg) {
    ARG_UNUSED(arg); KP_CHECK(k_is_in_isr());
    struct kp_rgb_hsb c = {0};
    struct zmk_behavior_binding b = binding(DEV(rgbs_a), RGB_HUI_CMD, 0);
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_c)) == -EWOULDBLOCK);
    KP_CHECK(zmk_rgb_selector_select(NULL, NULL) == -EWOULDBLOCK);
    KP_CHECK(zmk_rgb_selector_reset(DEV(selector_a)) == -EWOULDBLOCK);
    KP_CHECK(zmk_rgb_selector_reset(NULL) == -EWOULDBLOCK);
    KP_CHECK(zmk_rgb_matrix_set_color(DEV(private_white), &c) == -EWOULDBLOCK);
    KP_CHECK(zmk_rgb_matrix_set_period(DEV(private_white), 100) == -EWOULDBLOCK);
    KP_CHECK(kp_rgb_selector_binding_apply(NULL, RGB_RESET_CMD, 0) == -EWOULDBLOCK);
    KP_CHECK(kp_rgb_selector_binding_convert(NULL, &b) == -EWOULDBLOCK);
    KP_CHECK(convert(&b) == -EWOULDBLOCK);
    KP_CHECK(invoke(DEV(rgbs_a), RGB_HUI_CMD, 0) == -EWOULDBLOCK);
    b = binding(DEV(rgb_matrix), RGB_BRI_CMD, 0); KP_CHECK(convert(&b) == -EWOULDBLOCK);
    KP_CHECK(invoke(DEV(rgb_matrix), RGB_BRI_CMD, 0) == -EWOULDBLOCK);
}
static void deferred_prepare(void) {
    KP_CHECK(!device_is_ready(DEV(rgbs_a)));
    canonical_metadata_before_start();
    struct kp_rgb_hsb c = {12,34,29};
    KP_CHECK(zmk_rgb_matrix_set_color(DEV(probe_a), &c) == 0);
    KP_CHECK(zmk_rgb_matrix_set_period(DEV(probe_a), 4321) == 0);
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_a)) == -ENODEV);
    if (kp_test_case("control-deferred")) KP_CHECK(device_init(DEV(rgbs_a)) == 0);
}
static int control_prepare(void) {
    if (kp_test_case("control-early")) early_prepare();
    if (kp_test_case("control-reset-prestart")) prestart_reset_prepare();
    if (kp_test_case("control-deferred") || kp_test_case("control-unready")) deferred_prepare();
    return 0;
}
SYS_INIT(control_prepare, APPLICATION, 98);
static void deferred(void) {
    if (kp_test_case("control-unready")) {
        KP_CHECK(!device_is_ready(DEV(rgbs_a)));
        KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
        KP_CHECK(device_init(DEV(rgbs_a)) == 0);
        k_msleep(100);
        KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
        KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_a)) == -ENODEV);
    } else wait_ready();
    KP_CHECK(kp_rgb_effect_period(DEV(probe_a)) == 4321);
    KP_CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == 12);
    KP_CHECK(kp_rgb_effect_data(DEV(probe_a))->color.s == 34);
    KP_CHECK(kp_rgb_effect_data(DEV(probe_a))->color.b == 29);
}
#endif

void kp_test_controls_run(void) {
    if (strncmp(kp_test_scenario, "control-matrix-", 15) == 0) { matrix_defaults(); return; }
#if DT_NODE_EXISTS(DT_NODELABEL(selector_a))
    if (kp_test_case("control-early")) early();
    else if (kp_test_case("control-duration-defaults")) duration_defaults();
    else if (kp_test_case("control-registry")) registry();
    else if (kp_test_case("control-parameters")) parameters();
    else if (kp_test_case("control-global-defaults")) global_defaults();
    else if (kp_test_case("control-noop")) no_op();
    else if (kp_test_case("control-peripheral")) { conversion(); irq_offload(isr, NULL); }
    else if (kp_test_case("control-conversion") || kp_test_case("control-conversion-metadata-off") || kp_test_case("control-large-step")) conversion();
    else if (kp_test_case("control-validation")) validation();
    else if (kp_test_case("control-canonical-validation")) canonical_validation();
    else if (kp_test_case("control-normalize-commands")) normalize_commands();
    else if (kp_test_case("control-global-commands")) global_commands();
    else if (kp_test_case("control-reset")) scoped_reset();
    else if (kp_test_case("control-reset-fallback-noop") || kp_test_case("control-reset-fallback-notify")) reset_fallback();
    else if (kp_test_case("control-reset-prestart")) reset_prestart_result();
    else if (kp_test_case("control-publication")) publication();
    else if (kp_test_case("control-deferred") || kp_test_case("control-unready")) deferred();
    else if (kp_test_case("control-owner")) owner_callbacks();
    else if (kp_test_case("control-local-only")) local_only();
    else if (kp_test_case("control-isr")) { wait_ready(); irq_offload(isr, NULL); }
    else KP_CHECK(false);
#else
    KP_CHECK(false);
#endif
}
