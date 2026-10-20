/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <zephyr/init.h>
#include <zephyr/irq_offload.h>
#include <zephyr/device.h>
#include <string.h>
#include <zephyr/sys/printk.h>
#include <zmk/rgb_selector.h>
#include <drivers/behavior.h>
#include <zmk/split/transport/central.h>
#include "rgb_matrix_internal.h"
#include "rgb_selector_sync_internal.h"

#include "cmdline.h"
#include "soc.h"

static const char *split_case = "existing";
static void split_options(void) {
    static struct args_struct_t args[] = {
        {.option = "split-scenario", .name = "scenario", .type = 's',
         .dest = (void *)&split_case, .descript = "Isolated split regression"},
        ARG_TABLE_ENDMARKER};
    native_add_command_line_opts(args);
}
NATIVE_TASK(split_options, PRE_BOOT_1, 12);
#define DEV(label) DEVICE_DT_GET(DT_NODELABEL(label))
#define CHECK(test) do { if (!(test)) { \
    printk("FAIL scoped split: %s line %d\n", #test, __LINE__); exit(1); \
} } while (0)

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#if !DT_NODE_EXISTS(DT_NODELABEL(selector_a))
#include "kp_rgb_singleton_split_test.inc"
#else
static int split_provider_init(void) {
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == NULL);
    if (!strcmp(split_case, "ready-control")) {
        CHECK(!device_is_ready(DEV(rgbs_a)));
        CHECK(device_init(DEV(rgbs_a)) == 0);
    } else if (!strcmp(split_case, "ready-member")) {
        CHECK(!device_is_ready(DEV(probe_a)) && !device_is_ready(DEV(probe_c)));
        CHECK(device_init(DEV(probe_a)) == 0);
        CHECK(device_init(DEV(probe_c)) == 0);
    }
    return 0;
}
SYS_INIT(split_provider_init, APPLICATION, 98);

struct lane {
    unsigned phase, slot, selected, completed;
    uint32_t color, period, selected_color, selected_period;
    uint32_t member_color[MAX(DT_CHILD_NUM(DT_NODELABEL(selector_a)), DT_CHILD_NUM(DT_NODELABEL(selector_b)))];
    uint32_t member_period[MAX(DT_CHILD_NUM(DT_NODELABEL(selector_a)), DT_CHILD_NUM(DT_NODELABEL(selector_b)))];
};
struct attempt {
    uint8_t source;
    char endpoint[16];
    uint32_t command, argument;
    int64_t time;
    int64_t completion;
    bool accepted;
};
static K_MUTEX_DEFINE(records_lock);
static struct lane remote[2][2];
static struct attempt attempts[512];
static unsigned attempt_count, query_count;
static unsigned power_count[2], brightness_count[2], overlay_count[2];
static uint32_t power[2], brightness[2];
static bool global_race;
static void global_apply(uint32_t command, uint32_t argument) {
    struct zmk_behavior_binding binding = {.behavior_dev = DEV(rgb_matrix)->name,
        .param1 = command, .param2 = argument};
    CHECK(behavior_keymap_binding_pressed(&binding, (struct zmk_behavior_binding_event){0}) == 0);
}
static unsigned connected;
static int64_t reconnect_floor[2];
static bool fail_a;
static uint32_t fail_kind = RGB_SELECTOR_COLOR_CMD;
static unsigned send_duration;
static bool alternate_transport, no_transport;
static int query_error;
static uint32_t mutate_kind;
static unsigned mutate_action;
static unsigned mutation_count;
static unsigned mutate_nth, mutate_seen, injected_index;
static uint32_t injected_period;
static unsigned clock_count[2];
static uint32_t clock_value[2];
static zmk_split_transport_central_status_changed_cb_t status_cb;
static K_SEM_DEFINE(query_request, 0, 1);
static K_SEM_DEFINE(query_done, 0, 1);

/* A separate thread detects a held recursive matrix mutex, unlike a same-thread query. */
static void query_thread(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    while (true) {
        k_sem_take(&query_request, K_FOREVER);
        bool on;
        CHECK(zmk_rgb_matrix_get_user_state(&on) == 0);
        (void)zmk_rgb_selector_get_selected(DEV(selector_a));
        k_mutex_lock(&records_lock, K_FOREVER);
        query_count++;
        k_mutex_unlock(&records_lock);
        k_sem_give(&query_done);
    }
}
K_THREAD_DEFINE(split_query, 1024, query_thread, NULL, NULL, NULL, 5, 0, 0);

static int send_command(uint8_t source, struct zmk_split_transport_central_command cmd) {
    CHECK(source < 2);
    CHECK(cmd.type == ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_INVOKE_BEHAVIOR);
    k_mutex_lock(&records_lock, K_FOREVER);
    bool present = connected & BIT(source);
    k_mutex_unlock(&records_lock);
    if (!present) return -ENOTCONN;
    CHECK(k_uptime_get() >= reconnect_floor[source]);
    CHECK(cmd.data.invoke_behavior.state == 1);
    /* Initial condition publication also broadcasts overlay state with source 0. */
    if (!strncmp(split_case, "late-", 5) && cmd.data.invoke_behavior.param1 == RGB_OVL_STATE_CMD)
        CHECK(cmd.data.invoke_behavior.event_source == 0 || cmd.data.invoke_behavior.event_source == source);
    else
        CHECK(cmd.data.invoke_behavior.event_source == source);
    CHECK(cmd.data.invoke_behavior.position == 0);
    k_sem_give(&query_request);
    CHECK(k_sem_take(&query_done, K_MSEC(100)) == 0);
    bool on;
    CHECK(zmk_rgb_matrix_get_user_state(&on) == 0);

    k_mutex_lock(&records_lock, K_FOREVER);
    CHECK(connected & BIT(source));
    uint32_t command = cmd.data.invoke_behavior.param1;
    uint32_t argument = cmd.data.invoke_behavior.param2;
    const char *endpoint = cmd.data.invoke_behavior.behavior_dev;
    int index = !strcmp(endpoint, DEV(rgbs_a)->name) ? 0 :
                !strcmp(endpoint, DEV(rgbs_b)->name) ? 1 : -1;
    bool accepted = !(source == 0 && index == 0 && fail_a &&
                     (command & KP_RGB_SELECTOR_COMMAND_MASK) == fail_kind);
    bool sweep_inject = source == 0 && index == 0 && mutate_nth && ++mutate_seen == mutate_nth;
    bool phase_inject = source == 0 && index == 0 && mutate_kind &&
        (command & KP_RGB_SELECTOR_COMMAND_MASK) == mutate_kind;
    if ((sweep_inject || phase_inject) && strstr(split_case, "failed")) accepted = false;
    CHECK(attempt_count < ARRAY_SIZE(attempts));
    struct attempt *record = &attempts[attempt_count++];
    *record = (struct attempt){.source = source, .command = command,
        .argument = argument, .time = k_uptime_get(), .accepted = accepted};
    strcpy(record->endpoint, endpoint);
    if (accepted && index >= 0) {
        struct lane *lane = &remote[source][index];
        uint32_t kind = command & KP_RGB_SELECTOR_COMMAND_MASK;
        if (kind == RGB_SELECTOR_COLOR_CMD) {
            CHECK(lane->phase == 0 || lane->phase == 2);
            lane->slot = command >> 16;
            CHECK(lane->slot < ARRAY_SIZE(lane->member_color));
            lane->color = argument;
            lane->member_color[lane->slot] = argument;
            lane->phase = 1;
        } else if (kind == RGB_SELECTOR_PERIOD_CMD) {
            CHECK(lane->phase == 1 && lane->slot == command >> 16);
            lane->period = argument;
            lane->member_period[lane->slot] = argument;
            lane->phase = 2;
        } else {
            CHECK(command == RGB_EFS_CMD);
            CHECK(lane->phase == 2 && argument < ARRAY_SIZE(lane->member_color));
            lane->selected = argument;
            lane->selected_color = lane->member_color[argument];
            lane->selected_period = lane->member_period[argument];
            lane->completed++;
            lane->phase = 0;
        }
    } else if (accepted) {
        CHECK(!strcmp(endpoint, DEV(rgb_matrix)->name));
        if (command == RGB_OFF_CMD || command == RGB_ON_CMD) {
            power[source] = command;
            power_count[source]++;
        } else if (command == RGB_BRT_SET_CMD) {
            brightness_count[source]++;
            brightness[source] = argument;
        } else if (command == RGB_OVL_STATE_CMD) {
            overlay_count[source]++;
        } else {
            CHECK(command == RGB_CLOCK_CMD);
            clock_count[source]++;
            clock_value[source] = argument;
            CHECK((uint32_t)record->time - argument <= source * (send_duration + 1) + 1);
        }
    }
    bool inject = global_race && index < 0 && command == RGB_BRT_SET_CMD;
    if (inject) global_race = false;
    unsigned action = 0;
    if (sweep_inject || (source == 0 && index == 0 && mutate_kind &&
        (command & KP_RGB_SELECTOR_COMMAND_MASK) == mutate_kind)) {
        if (sweep_inject) mutate_nth = 0;
        mutate_kind = 0;
        action = mutate_action;
        mutation_count++;
    }
    if (sweep_inject) injected_index = attempt_count - 1;
    k_mutex_unlock(&records_lock);
    if (sweep_inject && (command & KP_RGB_SELECTOR_COMMAND_MASK) == RGB_SELECTOR_COLOR_CMD) {
        kp_rgb_matrix_lock();
        const struct kp_rgb_selector *owner = kp_rgb_selector_overlay_state_locked(DEV(selector_a));
        injected_period = kp_rgb_effect_period(kp_rgb_selector_at(owner, command >> 16));
        kp_rgb_matrix_unlock();
    }
    if (inject) {
        global_apply(RGB_OFF_CMD, 0);
        global_apply(RGB_BRT_SET_CMD, 19);
        k_mutex_lock(&records_lock, K_FOREVER);
        power[source] = RGB_OFF_CMD;
        brightness[source] = 19;
        k_mutex_unlock(&records_lock);
    }
    if (action == 3) {
        global_apply(RGB_RESET_CMD, 0);
    } else if (action == 4) {
        CHECK(kp_rgb_selector_binding_apply(DEV(selector_a),
            KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 1),
            RGB_COLOR_HSB_VAL(10, 100, 0)) == 0);
    } else if (action) {
        CHECK(kp_rgb_selector_binding_apply(DEV(selector_a),
            action == 2 ? RGB_RESET_CMD : RGB_EFS_CMD, action == 2 ? 0 : 2) == 0);
        CHECK(kp_rgb_selector_binding_apply(DEV(selector_a),
            KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 2),
            RGB_COLOR_HSB_VAL(299, 100, 100)) == 0);
        CHECK(kp_rgb_selector_binding_apply(DEV(selector_a),
            KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, 2), 2999) == 0);
    }
    if (send_duration) k_sleep(K_MSEC(send_duration));
    k_mutex_lock(&records_lock, K_FOREVER);
    record->completion = k_uptime_get();
    k_mutex_unlock(&records_lock);
    return accepted ? 0 : -EIO;
}

static int sources(uint8_t *ids) {
    k_mutex_lock(&records_lock, K_FOREVER);
    if (query_error) { int error = query_error; k_mutex_unlock(&records_lock); return error; }
    int count = 0;
    for (unsigned i = 0; i < 2; i++)
        if (connected & BIT(i)) ids[count++] = i;
    k_mutex_unlock(&records_lock);
    return count;
}
static struct zmk_split_transport_status status(void) {
    k_mutex_lock(&records_lock, K_FOREVER);
    bool present = connected != 0 && !no_transport;
    k_mutex_unlock(&records_lock);
    return (struct zmk_split_transport_status){.available = !alternate_transport && !no_transport, .enabled = true,
        .connections = present ? ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_SOME_CONNECTED :
                                ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED};
}
static int set_callback(zmk_split_transport_central_status_changed_cb_t cb) {
    status_cb = cb;
    return 0;
}
static int set_enabled(bool enabled) { ARG_UNUSED(enabled); return 0; }
static const struct zmk_split_transport_central_api transport_api = {
    .send_command = send_command, .get_available_source_ids = sources,
    .get_status = status, .set_status_callback = set_callback, .set_enabled = set_enabled,
};
ZMK_SPLIT_TRANSPORT_CENTRAL_REGISTER(scoped_mock, &transport_api, 0);
static struct zmk_split_transport_status alternate_status(void) {
    struct zmk_split_transport_status value = status();
    value.available = alternate_transport && !no_transport;
    return value;
}
static const struct zmk_split_transport_central_api alternate_api = {
    .send_command = send_command, .get_available_source_ids = sources,
    .get_status = alternate_status, .set_status_callback = set_callback,
    .set_enabled = set_enabled,
};
ZMK_SPLIT_TRANSPORT_CENTRAL_REGISTER(scoped_alternate, &alternate_api, 0);

static void connect(unsigned mask) {
    k_mutex_lock(&records_lock, K_FOREVER);
    for (unsigned s = 0; s < 2; s++)
        if ((mask & BIT(s)) && !(connected & BIT(s)))
            reconnect_floor[s] = k_uptime_get() + CONFIG_KEYPAW_RGB_SPLIT_SYNC_SETTLE_MS;
    connected = mask;
    k_mutex_unlock(&records_lock);
    CHECK(status_cb != NULL);
    struct zmk_split_transport_status value = status();
    if (alternate_transport) value.connections = ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED;
    CHECK(status_cb(&scoped_mock, value) == (no_transport ? -ENODEV : 0));
}
static void failing(bool fail) {
    k_mutex_lock(&records_lock, K_FOREVER);
    fail_a = fail;
    k_mutex_unlock(&records_lock);
}
static struct lane lane(unsigned source, unsigned index) {
    k_mutex_lock(&records_lock, K_FOREVER);
    struct lane value = remote[source][index];
    k_mutex_unlock(&records_lock);
    return value;
}
static void apply(const struct device *selector, uint32_t command, uint32_t value) {
    CHECK(kp_rgb_selector_binding_apply(selector, command, value) == 0);
}
static void parameters_hsb(const struct device *selector, unsigned slot, uint32_t color, unsigned period) {
    apply(selector, KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, slot), color);
    apply(selector, KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, slot), period);
}
static void matches_hsb(unsigned source, unsigned index, unsigned slot, uint32_t color, unsigned period) {
    struct lane value = lane(source, index);
    CHECK(value.completed > 0 && value.phase == 0);
    CHECK(value.selected == slot && value.selected_color == color);
    CHECK(value.selected_period == period);
}
static void parameters(const struct device *selector, unsigned slot, unsigned hue, unsigned period) {
    parameters_hsb(selector, slot, RGB_COLOR_HSB_VAL(hue, 100, 100), period);
}
static void matches(unsigned source, unsigned index, unsigned slot, unsigned hue, unsigned period) {
    matches_hsb(source, index, slot, RGB_COLOR_HSB_VAL(hue, 100, 100), period);
}

static void check_all_members(unsigned index) {
    const struct device *owner = index ? DEV(selector_b) : DEV(selector_a);
    for (unsigned source = 0; source < 2; source++) {
        struct lane value = lane(source, index);
        struct kp_rgb_selector_snapshot snapshot; size_t cursor = 0;
        kp_rgb_matrix_lock();
        int next; unsigned members = 0;
        while ((next = kp_rgb_selector_next_snapshot_locked(owner, &cursor, &snapshot)) > 0) {
            members++;
            CHECK(value.member_color[snapshot.slot] ==
                  RGB_COLOR_HSB_VAL(snapshot.color.h, snapshot.color.s, snapshot.color.b));
            CHECK(value.member_period[snapshot.slot] == snapshot.period_ms);
        }
        CHECK(next == 0);
        CHECK(members == (index ? DT_CHILD_NUM_STATUS_OKAY(DT_NODELABEL(selector_b)) :
                                  DT_CHILD_NUM_STATUS_OKAY(DT_NODELABEL(selector_a))));
        CHECK(kp_rgb_selector_snapshot_locked(owner, &snapshot) == 0);
        kp_rgb_matrix_unlock();
        CHECK(value.phase == 0 && value.selected == snapshot.slot);
    }
}
static void clock_offsets(void) {
    int32_t offsets[] = {0, 1500, -2000};
    for (size_t i = 0; i < ARRAY_SIZE(offsets); i++) {
        kp_rgb_set_clock_offset(offsets[i]);
        CHECK(kp_rgb_clock_ms() == (uint32_t)k_uptime_get() + (uint32_t)offsets[i]);
    }
    uint32_t central[] = {123456, 10, UINT32_MAX - 15};
    for (size_t i = 0; i < ARRAY_SIZE(central); i++) {
        global_apply(RGB_CLOCK_CMD, central[i]);
        CHECK(kp_rgb_clock_ms() == central[i]);
        uint32_t before = (uint32_t)k_uptime_get();
        k_sleep(K_MSEC(17));
        CHECK(kp_rgb_clock_ms() == central[i] + (uint32_t)k_uptime_get() - before);
    }
    kp_rgb_set_clock_offset(0);
}
static int control_apply(uint32_t command, uint32_t argument) {
    struct zmk_behavior_binding binding = {.behavior_dev = DEV(rgbs_a)->name,
        .param1 = command, .param2 = argument};
    return behavior_keymap_binding_pressed(&binding, (struct zmk_behavior_binding_event){0});
}
static int central_irq_result;
static void central_irq(const void *unused) {
    ARG_UNUSED(unused);
    CHECK(k_is_in_isr());
    central_irq_result = control_apply(RGB_EFS_CMD, 2);
}
static void brightness_split(void) {
    bool reset = !strcmp(split_case, "brightness-reset");
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
    CHECK(kp_rgb_effect_data(DEV(probe_a))->color.b == 50);
    CHECK(kp_rgb_effect_data(DEV(probe_c))->color.b == 25);
    CHECK(kp_rgb_effect_data(DEV(probe_b))->color.b == 50);
    CHECK(kp_rgb_effect_data(DEV(probe_d))->color.b == 25);
    const uint32_t old = RGB_COLOR_HSB_VAL(10, 100, 37);
    parameters_hsb(DEV(selector_a), 1, old, 1100);
    parameters_hsb(DEV(selector_b), 0, RGB_COLOR_HSB_VAL(20, 70, 63), 1200);
    if (reset) {
        parameters_hsb(DEV(selector_a), 2, RGB_COLOR_HSB_VAL(220, 70, 7), 2300);
        parameters_hsb(DEV(selector_b), 1, RGB_COLOR_HSB_VAL(180, 60, 0), 2400);
    }
    mutate_kind = RGB_SELECTOR_COLOR_CMD;
    mutate_action = reset ? 3 : 4;
    failing(!reset);
    connect(3);
    k_sleep(K_MSEC(900));
    CHECK(mutation_count == 1);
    if (!reset) {
        CHECK(lane(0, 0).completed == 0);
        CHECK(lane(1, 0).completed && lane(0, 1).completed && lane(1, 1).completed);
        unsigned failures = 0;
        for (unsigned i = 0; i < attempt_count; i++) {
            const struct attempt *r = &attempts[i];
            if (r->accepted) continue;
            CHECK(r->source == 0 && !strcmp(r->endpoint, DEV(rgbs_a)->name));
            CHECK(r->command == KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 1));
            CHECK(r->argument == old);
            failures++;
        }
        CHECK(failures >= 3);
        failing(false);
    }
    k_sleep(K_MSEC(2500));
    uint32_t a_period = kp_rgb_effect_period(DEV(probe_a));
    uint32_t c_period = kp_rgb_effect_period(DEV(probe_c));
    const uint32_t commands[] = {
        KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 1),
        KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, 1), RGB_EFS_CMD,
        KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 1),
        KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, 1),
        KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 2),
        KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, 2), RGB_EFS_CMD};
    const uint32_t reset_values[] = {old, 1100, 1, RGB_COLOR_HSB_VAL(120, 100, 50),
        a_period, RGB_COLOR_HSB_VAL(240, 100, 25), c_period, 1};
    const uint32_t latest_values[] = {old, 1100, 1, RGB_COLOR_HSB_VAL(10, 100, 0), 1100, 1};
    unsigned step = 0, expected = reset ? 8 : 6;
    for (unsigned i = 0; i < attempt_count && step < expected; i++) {
        const struct attempt *r = &attempts[i];
        if (!r->accepted || r->source || strcmp(r->endpoint, DEV(rgbs_a)->name)) continue;
        uint32_t command = !reset && step == 5 ? RGB_EFS_CMD : commands[step];
        CHECK(r->command == command);
        CHECK(r->argument == (reset ? reset_values[step] : latest_values[step]));
        step++;
    }
    CHECK(step == expected);
    if (reset) {
        check_all_members(0); check_all_members(1);
        for (unsigned s = 0; s < 2; s++) {
            struct lane a = lane(s, 0), b = lane(s, 1);
            CHECK(a.member_color[1] == RGB_COLOR_HSB_VAL(120, 100, 50));
            CHECK(a.member_color[2] == RGB_COLOR_HSB_VAL(240, 100, 25));
            CHECK(b.member_color[0] == RGB_COLOR_HSB_VAL(60, 100, 50));
            CHECK(b.member_color[1] == RGB_COLOR_HSB_VAL(180, 100, 25));
            for (unsigned index = 0; index < 2; index++) {
                const char *endpoint = index ? DEV(rgbs_b)->name : DEV(rgbs_a)->name;
                unsigned count = 0;
                for (unsigned i = 0; i < attempt_count; i++)
                    if (attempts[i].accepted && attempts[i].source == s &&
                        !strcmp(attempts[i].endpoint, endpoint)) count++;
                CHECK(count >= 5);
                unsigned seen = 0;
                for (unsigned i = 0; i < attempt_count; i++) {
                    const struct attempt *r = &attempts[i];
                    if (!r->accepted || r->source != s || strcmp(r->endpoint, endpoint)) continue;
                    if (seen++ < count - 5) continue;
                    unsigned n = seen - (count - 5) - 1;
                    unsigned slot = index ? n / 2 : 1 + n / 2;
                    uint32_t command = n == 4 ? RGB_EFS_CMD : KP_RGB_SELECTOR_MEMBER_COMMAND(
                        n & 1 ? RGB_SELECTOR_PERIOD_CMD : RGB_SELECTOR_COLOR_CMD, slot);
                    CHECK(r->command == command);
                    if (n == 4) CHECK(r->argument == (index ? 0 : 1));
                    else if (!(n & 1)) {
                        uint32_t color = index ? (n == 0 ? RGB_COLOR_HSB_VAL(60, 100, 50) :
                            RGB_COLOR_HSB_VAL(180, 100, 25)) : (n == 0 ? RGB_COLOR_HSB_VAL(120, 100, 50) :
                            RGB_COLOR_HSB_VAL(240, 100, 25));
                        CHECK(r->argument == color);
                    } else {
                        const struct device *member = index ? (n == 1 ? DEV(probe_b) : DEV(probe_d)) :
                            (n == 1 ? DEV(probe_a) : DEV(probe_c));
                        CHECK(r->argument == kp_rgb_effect_period(member));
                    }
                }
            }
        }
    } else {
        for (unsigned s = 0; s < 2; s++) {
            matches_hsb(s, 0, 1, RGB_COLOR_HSB_VAL(10, 100, 0), 1100);
            matches_hsb(s, 1, 0, RGB_COLOR_HSB_VAL(20, 70, 63), 1200);
        }
        connect(2); k_sleep(K_MSEC(150));
        k_mutex_lock(&records_lock, K_FOREVER);
        memset(remote[0], 0, sizeof(remote[0]));
        k_mutex_unlock(&records_lock);
        connect(3); k_sleep(K_MSEC(1200));
        matches_hsb(0, 0, 1, RGB_COLOR_HSB_VAL(10, 100, 0), 1100);
        matches_hsb(0, 1, 0, RGB_COLOR_HSB_VAL(20, 70, 63), 1200);
    }
    printk("PASS split %s\n", split_case); exit(0);
}
static void extended_split(void) {
    if (!strncmp(split_case, "sweep-", 6)) {
        unsigned phase = split_case[strlen(split_case) - 1] - '0';
        CHECK(phase < (DT_NODE_EXISTS(DT_NODELABEL(probe_e)) ? 7 : 5));
        connect(3); k_sleep(K_MSEC(1000));
        k_mutex_lock(&records_lock, K_FOREVER);
        mutate_nth = phase + 1; mutate_seen = 0;
        mutate_action = strstr(split_case, "reset") ? 2 : 1;
        k_mutex_unlock(&records_lock);
        apply(DEV(selector_a), RGB_RESET_CMD, 0);
        k_sleep(K_MSEC(2200));
        CHECK(mutation_count == 1);
        check_all_members(0);
        const struct attempt *frozen = &attempts[injected_index];
        unsigned step = frozen->accepted ? 1 : 0;
        for (unsigned i = injected_index + 1; i < attempt_count && step < 2; i++) {
            const struct attempt *r = &attempts[i];
            if (r->source || strcmp(r->endpoint, DEV(rgbs_a)->name)) continue;
            if (step == 0) {
                CHECK(r->command == frozen->command && r->argument == frozen->argument);
                if (r->accepted) step++;
            } else {
                if ((frozen->command & KP_RGB_SELECTOR_COMMAND_MASK) == RGB_SELECTOR_COLOR_CMD) {
                    CHECK(r->command == KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, frozen->command >> 16));
                    CHECK(r->argument == injected_period);
                }
                step++;
            }
        }
        CHECK(step == 2);
        CHECK(lane(0, 1).completed == 1 && lane(1, 1).completed == 1);
    } else if (!strncmp(split_case, "global-reset-", 13)) {
        mutate_kind = strstr(split_case, "period") ? RGB_SELECTOR_PERIOD_CMD :
                      strstr(split_case, "selection") ? RGB_EFS_CMD : RGB_SELECTOR_COLOR_CMD;
        mutate_action = 3;
        connect(3); k_sleep(K_MSEC(2200));
        CHECK(mutation_count == 1);
        check_all_members(0); check_all_members(1);
        unsigned step = 0;
        const uint32_t commands[] = {KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 1),
            KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, 1), RGB_EFS_CMD};
        const uint32_t values[] = {RGB_COLOR_HSB_VAL(10, 100, 100), 1100, 1};
        for (unsigned i = 0; i < attempt_count && step < 3; i++) {
            const struct attempt *r = &attempts[i];
            if (r->source || strcmp(r->endpoint, DEV(rgbs_a)->name)) continue;
            CHECK(r->command == commands[step] && r->argument == values[step]);
            if (r->accepted) step++;
        }
        CHECK(step == 3);
    } else if (!strcmp(split_case, "dirty-timing")) {
        connect(3); k_sleep(K_MSEC(1200));
        for (unsigned n = 0; n < 4; n++) {
            unsigned before = attempt_count;
            int64_t changed = k_uptime_get();
            apply(DEV(selector_a), RGB_EFS_CMD, 1);
            k_sleep(K_MSEC(500));
            unsigned packets = 0;
            for (unsigned i = before; i < attempt_count; i++) {
                const struct attempt *r = &attempts[i];
                if (strcmp(r->endpoint, DEV(rgbs_a)->name)) continue;
                if (packets == 0) CHECK(r->time >= changed && r->time < changed + CONFIG_KEYPAW_RGB_SPLIT_SYNC_SETTLE_MS);
                packets++;
            }
            CHECK(packets == 6);
        }
        CHECK(lane(0, 1).completed == 1 && lane(1, 1).completed == 1);
    } else if (!strcmp(split_case, "identical") || !strcmp(split_case, "dirty-hook")) {
        connect(3); k_sleep(K_MSEC(1000));
        unsigned before[2] = {lane(0, 0).completed, lane(1, 0).completed};
        unsigned b_before[2] = {lane(0, 1).completed, lane(1, 1).completed};
        if (!strcmp(split_case, "dirty-hook")) {
            kp_rgb_matrix_lock();
            kp_rgb_split_selector_dirty_locked(NULL);
            kp_rgb_split_selector_dirty_locked(DEV(private_white));
            kp_rgb_split_selector_reset_locked(DEV(private_white));
            kp_rgb_matrix_unlock();
        } else {
            const uint32_t commands[] = {KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 1),
                KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, 1), RGB_EFS_CMD};
            const uint32_t arguments[] = {RGB_COLOR_HSB_VAL(10, 100, 100), 1100, 1};
            for (unsigned n = 0; n < ARRAY_SIZE(commands); n++) {
                CHECK(control_apply(commands[n], arguments[n]) == 0);
                k_sleep(K_MSEC(500));
                for (unsigned s = 0; s < 2; s++) {
                    CHECK(lane(s, 0).completed == before[s] + 1);
                    before[s]++;
                    matches(s, 0, 1, 10, 1100);
                }
            }
            CHECK(control_apply(RGB_EFS_CMD, UINT32_MAX) == -EINVAL);
            CHECK(control_apply(0x10a, 0) == -ENOTSUP);
            irq_offload(central_irq, NULL);
            CHECK(central_irq_result == -EWOULDBLOCK);
        }
        k_sleep(K_MSEC(1100));
        for (unsigned s = 0; s < 2; s++) {
            CHECK(lane(s, 0).completed == before[s]);
            CHECK(lane(s, 1).completed == b_before[s]);
        }
    } else if (!strcmp(split_case, "no-transport") || !strcmp(split_case, "negative-query")) {
        connect(3); k_sleep(K_MSEC(1100));
        k_mutex_lock(&records_lock, K_FOREVER);
        if (!strcmp(split_case, "no-transport")) no_transport = true;
        else query_error = -EIO;
        k_mutex_unlock(&records_lock);
        if (no_transport) connect(3);
        k_sleep(K_MSEC(100));
        if (no_transport) {
            STRUCT_SECTION_FOREACH(zmk_split_transport_central, t) {
                CHECK(t->api->get_status != NULL && !t->api->get_status().available);
            }
        }
        unsigned clocks = clock_count[0] + clock_count[1], before = attempt_count;
        k_sleep(K_MSEC(1200));
        CHECK(clock_count[0] + clock_count[1] == clocks && attempt_count == before);
        k_mutex_lock(&records_lock, K_FOREVER);
        no_transport = false; query_error = 0;
        unsigned completed = remote[0][0].completed;
        k_mutex_unlock(&records_lock);
        connect(3); k_sleep(K_MSEC(1100));
        CHECK(lane(0, 0).completed > completed);
        CHECK(clock_count[0] + clock_count[1] > clocks);
    } else if (!strcmp(split_case, "clock")) {
        clock_offsets();
        connect(3); k_sleep(K_MSEC(1400));
        CHECK(clock_count[0] >= 2 && clock_count[1] >= 2);
        connect(0); k_sleep(K_MSEC(100));
        unsigned before = clock_count[0] + clock_count[1];
        k_sleep(K_MSEC(1100)); CHECK(clock_count[0] + clock_count[1] == before);
    } else if (!strcmp(split_case, "local-silence")) {
        connect(3); k_sleep(K_MSEC(1000));
        unsigned before_a = lane(0, 0).completed, before_b = lane(0, 1).completed;
        unsigned before_global = brightness_count[0] + power_count[0];
        struct kp_rgb_hsb color = {.h = 234, .s = 55, .b = 100};
        CHECK(zmk_rgb_matrix_set_color(DEV(probe_a), &color) == 0);
        CHECK(zmk_rgb_matrix_set_period(DEV(probe_a), 1234) == 0);
        CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_a)) == 0);
        CHECK(zmk_rgb_matrix_off() == 0);
        k_sleep(K_MSEC(1200));
        CHECK(lane(0, 0).completed == before_a && lane(0, 1).completed == before_b);
        CHECK(brightness_count[0] + power_count[0] == before_global);
        CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == 234);
        CHECK(kp_rgb_effect_period(DEV(probe_a)) == 1234);
    } else if (!strcmp(split_case, "transport-change")) {
        failing(true); connect(3); k_sleep(K_MSEC(300));
        CHECK(lane(0, 0).completed == 0);
        apply(DEV(selector_a), RGB_EFS_CMD, 2);
        parameters(DEV(selector_a), 2, 220, 2220);
        connect(0); k_sleep(K_MSEC(100));
        k_mutex_lock(&records_lock, K_FOREVER);
        memset(remote, 0, sizeof(remote));
        alternate_transport = true; unsigned before = attempt_count;
        k_mutex_unlock(&records_lock);
        failing(false); connect(3);
        k_sleep(K_MSEC(20)); CHECK(attempt_count == before);
        k_sleep(K_MSEC(1000)); matches(0, 0, 2, 220, 2220); matches(1, 0, 2, 220, 2220);
        for (unsigned i = before; i < attempt_count; i++) if (!strcmp(attempts[i].endpoint, DEV(rgbs_a)->name)) {
            CHECK((attempts[i].command & KP_RGB_SELECTOR_COMMAND_MASK) == RGB_EFS_CMD ||
                  attempts[i].command >> 16 == 2);
        }
        query_error = -EIO; k_sleep(K_MSEC(100));
        query_error = 0; k_sleep(K_MSEC(1000));
        CHECK(lane(0, 0).completed >= 2 && lane(1, 0).completed >= 2);
    } else if (!strcmp(split_case, "fairness")) {
        connect(3);
        for (unsigned i = 0; i < 30; i++) {
            parameters(DEV(selector_a), 1, 100 + i, 2000 + i);
            k_sleep(K_MSEC(20));
        }
        k_sleep(K_MSEC(1000));
        matches(0, 0, 1, 129, 2029); matches(1, 0, 1, 129, 2029);
        CHECK(lane(0, 1).completed == 1 && lane(1, 1).completed == 1);
        unsigned counts[2][2] = {0};
        for (unsigned i = 0; i < attempt_count; i++) {
            struct attempt *a = &attempts[i];
            if (!strcmp(a->endpoint, DEV(rgbs_a)->name)) counts[a->source][0]++;
            if (!strcmp(a->endpoint, DEV(rgbs_b)->name)) counts[a->source][1]++;
        }
        CHECK(counts[0][1] == 3 && counts[1][1] == 3);
        CHECK(counts[0][0] >= 6 && counts[1][0] >= 6);
    } else {
        fail_kind = strstr(split_case, "period") ? RGB_SELECTOR_PERIOD_CMD :
                    strstr(split_case, "selection") ? RGB_EFS_CMD : RGB_SELECTOR_COLOR_CMD;
        bool reset = strstr(split_case, "reset") != NULL;
        bool pacing = strstr(split_case, "pacing") != NULL;
        mutate_kind = fail_kind; mutate_action = reset ? 2 : 1;
        if (pacing) send_duration = 37;
        failing(true); connect(3);
        k_sleep(K_MSEC(pacing ? 6500 : 900));
        CHECK(mutation_count == 1);
        CHECK(lane(0, 0).completed == 0);
        CHECK(lane(0, 1).completed && lane(1, 0).completed && lane(1, 1).completed);
        k_mutex_lock(&records_lock, K_FOREVER);
        unsigned failures = 0; struct attempt *previous = NULL;
        const unsigned delays[] = {50, 100, 200, 400, 800, 1000};
        for (unsigned i = 0; i < attempt_count; i++) {
            struct attempt *record = &attempts[i];
            if (record->source || strcmp(record->endpoint, DEV(rgbs_a)->name)) continue;
            if (previous) {
                unsigned delay = previous->accepted ? 50 : delays[MIN(failures - 1, 5)];
                CHECK(record->time >= previous->completion + delay);
            }
            if (!record->accepted) {
                CHECK((record->command & KP_RGB_SELECTOR_COMMAND_MASK) == fail_kind);
                CHECK(record->argument == (fail_kind == RGB_SELECTOR_COLOR_CMD ?
                      RGB_COLOR_HSB_VAL(10, 100, 100) : fail_kind == RGB_SELECTOR_PERIOD_CMD ? 1100 : 1));
                failures++;
            }
            previous = record;
        }
        CHECK(failures >= (pacing ? 8 : 3));
        k_mutex_unlock(&records_lock);
        failing(false); k_sleep(K_MSEC(3500));
        if (reset) check_all_members(0);
        else { matches(0, 0, 2, 299, 2999); matches(1, 0, 2, 299, 2999); }
        k_mutex_lock(&records_lock, K_FOREVER);
        unsigned step = 0;
        const uint32_t commands[] = {KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 1),
            KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, 1), RGB_EFS_CMD};
        const uint32_t values[] = {RGB_COLOR_HSB_VAL(10, 100, 100), 1100, 1};
        for (unsigned i = 0; i < attempt_count && step < 3; i++) {
            struct attempt *record = &attempts[i];
            if (record->accepted && !record->source && !strcmp(record->endpoint, DEV(rgbs_a)->name)) {
                CHECK(record->command == commands[step] && record->argument == values[step]); step++;
            }
        }
        CHECK(step == 3);
        k_mutex_unlock(&records_lock);
    }
    printk("PASS split %s\n", split_case); exit(0);
}

static void snapshot_validation(void) {
    struct kp_rgb_selector_snapshot snapshot = {0};
    size_t cursor = 0;
    kp_rgb_matrix_lock();
    CHECK(kp_rgb_selector_snapshot_locked(DEV(selector_a), NULL) == -EINVAL);
    CHECK(kp_rgb_selector_snapshot_locked(NULL, &snapshot) == -ENODEV);
    CHECK(kp_rgb_selector_next_snapshot_locked(DEV(selector_a), NULL, &snapshot) == -EINVAL);
    CHECK(kp_rgb_selector_next_snapshot_locked(DEV(selector_a), &cursor, NULL) == -EINVAL);
    CHECK(kp_rgb_selector_next_snapshot_locked(NULL, &cursor, &snapshot) == -ENODEV);
    kp_rgb_matrix_unlock();
}
static void readiness(void) {
    if (!strncmp(split_case, "ready-", 6)) {
        CHECK(device_is_ready(DEV(rgbs_a)) && device_is_ready(DEV(probe_a)));
        CHECK(device_is_ready(DEV(probe_c)));
        CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
        connect(3); k_sleep(K_MSEC(2300));
        for (unsigned s = 0; s < 2; s++) {
            CHECK(lane(s, 0).completed == 1 && lane(s, 1).completed == 1);
            CHECK(clock_count[s] && power_count[s] && brightness_count[s]);
        }
        apply(DEV(selector_a), RGB_RESET_CMD, 0);
        k_sleep(K_MSEC(1000)); check_all_members(0);
        printk("PASS split %s\n", split_case); exit(0);
    }
    bool control = !strcmp(split_case, "late-control");
    bool member = !strcmp(split_case, "late-member");
    const struct device *late = control ? DEV(rgbs_a) : member ? DEV(probe_a) : DEV(rgb_matrix);
    CHECK(!device_is_ready(late));
    if (control || member) {
        CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
        CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
        struct kp_rgb_selector_snapshot snapshot; size_t cursor = 0;
        kp_rgb_matrix_lock();
        CHECK(kp_rgb_selector_snapshot_locked(DEV(selector_a), &snapshot) == -ENODEV);
        CHECK(kp_rgb_selector_next_snapshot_locked(DEV(selector_a), &cursor, &snapshot) == -ENODEV);
        kp_rgb_matrix_unlock();
        struct zmk_behavior_binding binding = {.param1 = RGB_EFS_CMD, .param2 = 1};
        CHECK(kp_rgb_selector_binding_convert(DEV(selector_a), &binding) == -ENODEV);
    }
    connect(3); k_sleep(K_MSEC(1300));
    for (unsigned s = 0; s < 2; s++) {
        CHECK(lane(s, 1).completed == 1);
        if (control || member) {
            CHECK(lane(s, 0).completed == 0 && clock_count[s] >= 2);
            CHECK(power_count[s] && brightness_count[s]);
        } else {
            CHECK(lane(s, 0).completed == 1);
            CHECK(!clock_count[s] && !power_count[s] && !brightness_count[s] && !overlay_count[s]);
        }
    }
    unsigned b_before[2] = {lane(0, 1).completed, lane(1, 1).completed};
    CHECK(device_init(late) == 0 && device_is_ready(late));
    if (member) CHECK(device_init(DEV(probe_c)) == 0 && device_is_ready(DEV(probe_c)));
    k_sleep(K_MSEC(2300));
    for (unsigned s = 0; s < 2; s++) {
        CHECK(lane(s, 1).completed == b_before[s]);
        if (control || member) CHECK(lane(s, 0).completed == 0);
        else CHECK(lane(s, 0).completed == 1);
    }
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) ==
          (control || member ? NULL : DEV(probe_a)));
    printk("PASS split %s\n", split_case); exit(0);
}
static void test_thread(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    for (unsigned i = 0; i < 100 &&
         zmk_rgb_selector_get_selected(DEV(selector_b)) == NULL; i++) k_sleep(K_MSEC(5));
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
    k_sleep(K_MSEC(100));
    snapshot_validation();
    CHECK(zmk_rgb_matrix_off() == 0);
    CHECK(zmk_rgb_matrix_set_inhibited(true) == 0);
    if (!strncmp(split_case, "late-", 5) || !strncmp(split_case, "ready-", 6)) readiness();
    if (!strncmp(split_case, "brightness-", 11)) brightness_split();
    apply(DEV(selector_a), RGB_EFS_CMD, 1);
    parameters(DEV(selector_a), 1, 10, 1100);
    apply(DEV(selector_b), RGB_EFS_CMD, 0);
    parameters(DEV(selector_b), 0, 20, 1200);
    if (strcmp(split_case, "existing")) { extended_split(); }
    failing(true);
    connect(3);
    k_sleep(K_MSEC(800));
    CHECK(lane(0, 0).completed == 0);
    matches(0, 1, 0, 20, 1200);
    matches(1, 0, 1, 10, 1100);
    matches(1, 1, 0, 20, 1200);
    k_mutex_lock(&records_lock, K_FOREVER);
    for (unsigned s = 0; s < 2; s++) {
        CHECK(power_count[s] && power[s] == RGB_OFF_CMD);
        CHECK(brightness_count[s] && overlay_count[s]);
    }
    k_mutex_unlock(&records_lock);
    printk("PASS scoped split off/inhibited reconnect endpoints/qualified ordering\n");

    unsigned previous = lane(1, 0).completed;
    parameters(DEV(selector_a), 1, 30, 1300);
    parameters(DEV(selector_b), 0, 40, 1400);
    k_sleep(K_MSEC(600));
    CHECK(lane(1, 0).completed > previous);
    matches(1, 0, 1, 30, 1300);
    matches(0, 1, 0, 40, 1400);
    matches(1, 1, 0, 40, 1400);
    k_mutex_lock(&records_lock, K_FOREVER);
    unsigned failed = 0;
    int64_t last = -1;
    for (unsigned i = 0; i < attempt_count; i++) {
        struct attempt *record = &attempts[i];
        if (record->accepted) continue;
        CHECK(record->source == 0 && !strcmp(record->endpoint, DEV(rgbs_a)->name));
        CHECK(record->command == KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 1));
        CHECK(record->argument == RGB_COLOR_HSB_VAL(10, 100, 100));
        CHECK(last < 0 || record->time - last >= 50);
        last = record->time;
        failed++;
    }
    CHECK(failed >= 4);
    k_mutex_unlock(&records_lock);
    printk("PASS scoped split same-member parameter repair/independent lanes/frozen retries\n");

    for (unsigned i = 0; i < 6; i++) {
        apply(DEV(selector_a), RGB_EFS_CMD, (i & 1) ? 1 : 2);
        parameters(DEV(selector_a), (i & 1) ? 1 : 2, 50 + i, 1500 + i);
        k_sleep(K_MSEC(10));
    }
    apply(DEV(selector_a), RGB_EFS_CMD, 2);
    parameters(DEV(selector_a), 2, 60, 1600);
    failing(false);
    k_sleep(K_MSEC(1700));
    matches(0, 0, 2, 60, 1600);
    matches(1, 0, 2, 60, 1600);
    /* The first accepted retry must still be the original frozen command. */
    k_mutex_lock(&records_lock, K_FOREVER);
    unsigned frozen_steps = 0;
    const uint32_t frozen_commands[] = {
        KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 1),
        KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, 1), RGB_EFS_CMD};
    const uint32_t frozen_arguments[] = {RGB_COLOR_HSB_VAL(10, 100, 100), 1100, 1};
    for (unsigned i = 0; i < attempt_count && frozen_steps < 3; i++) {
        struct attempt *record = &attempts[i];
        if (record->accepted && record->source == 0 &&
            !strcmp(record->endpoint, DEV(rgbs_a)->name)) {
            CHECK(record->command == frozen_commands[frozen_steps]);
            CHECK(record->argument == frozen_arguments[frozen_steps]);
            frozen_steps++;
        }
    }
    CHECK(frozen_steps == 3);
    k_mutex_unlock(&records_lock);
    printk("PASS scoped split rapid changes/retry convergence\n");

    connect(2);
    k_sleep(K_MSEC(150));
    failing(true);
    k_mutex_lock(&records_lock, K_FOREVER);
    unsigned blocked_start = attempt_count;
    k_mutex_unlock(&records_lock);
    connect(3);
    k_sleep(K_MSEC(200));
    k_mutex_lock(&records_lock, K_FOREVER);
    unsigned blocked_failures = 0;
    for (unsigned i = blocked_start; i < attempt_count; i++) {
        if (!attempts[i].accepted) {
            CHECK(attempts[i].command ==
                KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 2));
            blocked_failures++;
        }
    }
    CHECK(blocked_failures >= 2);
    k_mutex_unlock(&records_lock);
    connect(2);
    k_sleep(K_MSEC(150));
    k_mutex_lock(&records_lock, K_FOREVER);
    memset(remote[0], 0, sizeof(remote[0]));
    unsigned reconnect_start = attempt_count;
    k_mutex_unlock(&records_lock);
    apply(DEV(selector_a), RGB_EFS_CMD, 1);
    parameters(DEV(selector_a), 1, 70, 1700);
    apply(DEV(selector_b), RGB_EFS_CMD, 1);
    parameters(DEV(selector_b), 1, 80, 1800);
    CHECK(zmk_rgb_matrix_on() == 0);
    CHECK(zmk_rgb_matrix_is_inhibited());
    failing(false);
    connect(3);
    k_sleep(K_MSEC(1500));
    matches(0, 0, 1, 70, 1700);
    matches(0, 1, 1, 80, 1800);
    matches(1, 0, 1, 70, 1700);
    matches(1, 1, 1, 80, 1800);
    k_mutex_lock(&records_lock, K_FOREVER);
    CHECK(power[0] == RGB_ON_CMD);
    for (unsigned i = reconnect_start; i < attempt_count; i++) {
        struct attempt *record = &attempts[i];
        if (record->source == 0 && !strcmp(record->endpoint, DEV(rgbs_a)->name) &&
            (record->command & KP_RGB_SELECTOR_COMMAND_MASK) == RGB_SELECTOR_COLOR_CMD) {
            CHECK(record->command == KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 1));
            CHECK(record->argument == RGB_COLOR_HSB_VAL(70, 100, 100));
        }
    }
    k_mutex_unlock(&records_lock);
    printk("PASS scoped split disconnect/reconnect discards in-flight snapshot\n");

    unsigned before = lane(0, 0).completed;
    CHECK(kp_rgb_selector_binding_apply(DEV(selector_a), RGB_EFS_CMD, 0) < 0);
    CHECK(kp_rgb_selector_binding_apply(DEV(selector_a), RGB_EFS_CMD, 99) < 0);
    CHECK(kp_rgb_selector_binding_apply(NULL, RGB_EFS_CMD, 1) == -ENODEV);
    CHECK(kp_rgb_selector_binding_apply(DEV(selector_a),
        KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, 1), 0) == -EINVAL);
    struct kp_rgb_selector_snapshot snapshot;
    k_sleep(K_MSEC(300));
    CHECK(lane(0, 0).completed == before);
    k_mutex_lock(&records_lock, K_FOREVER);
    CHECK(query_count == attempt_count && query_count > 40);
    unsigned total_calls = attempt_count;
    k_mutex_unlock(&records_lock);
    printk("PASS scoped split outside-mutex transport/local errors\n");

    k_mutex_lock(&records_lock, K_FOREVER);
    unsigned reset_start = attempt_count;
    k_mutex_unlock(&records_lock);
    failing(true);
    apply(DEV(selector_a), RGB_EFS_CMD, 2);
    parameters(DEV(selector_a), 2, 211, 3211);
    k_sleep(K_MSEC(200));
    k_mutex_lock(&records_lock, K_FOREVER);
    bool frozen_retry = false;
    for (unsigned i = reset_start; i < attempt_count; i++) {
        frozen_retry |= !attempts[i].accepted && attempts[i].source == 0 &&
            attempts[i].command == KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 2);
    }
    CHECK(frozen_retry);
    k_mutex_unlock(&records_lock);
    apply(DEV(selector_a), RGB_RESET_CMD, 0);
    /* A normal revision must not erase the all-member reset obligation. */
    parameters(DEV(selector_a), 1, 131, 2131);
    failing(false);
    k_sleep(K_MSEC(2000));
    for (unsigned source = 0; source < 2; source++) {
        struct lane value = lane(source, 0);
        size_t cursor = 0;
        kp_rgb_matrix_lock();
        while (kp_rgb_selector_next_snapshot_locked(DEV(selector_a), &cursor, &snapshot) > 0) {
            CHECK(value.member_color[snapshot.slot] ==
                  RGB_COLOR_HSB_VAL(snapshot.color.h, snapshot.color.s, snapshot.color.b));
            CHECK(value.member_period[snapshot.slot] == snapshot.period_ms);
        }
        CHECK(kp_rgb_selector_snapshot_locked(DEV(selector_a), &snapshot) == 0);
        kp_rgb_matrix_unlock();
        CHECK(value.selected == snapshot.slot && value.phase == 0);
    }
    printk("PASS scoped split reset repairs inactive members after frozen retry\n");

    CHECK(zmk_rgb_matrix_on() == 0);
    k_mutex_lock(&records_lock, K_FOREVER);
    global_race = true;
    k_mutex_unlock(&records_lock);
    global_apply(RGB_BRT_SET_CMD, 83);
    k_sleep(K_MSEC(1000));
    k_mutex_lock(&records_lock, K_FOREVER);
    CHECK(!global_race);
    for (unsigned source = 0; source < 2; source++) {
        CHECK(power[source] == RGB_OFF_CMD && brightness[source] == 19);
    }
    k_mutex_unlock(&records_lock);
    printk("PASS scoped split global behavior repairs frozen power/brightness\n");

    global_apply(RGB_RESET_CMD, 0);
    k_sleep(K_MSEC(1500));
    for (unsigned index = 0; index < 2; index++) {
        const struct device *owner = index ? DEV(selector_b) : DEV(selector_a);
        for (unsigned source = 0; source < 2; source++) {
            struct lane value = lane(source, index);
            size_t cursor = 0;
            kp_rgb_matrix_lock();
            while (kp_rgb_selector_next_snapshot_locked(owner, &cursor, &snapshot) > 0) {
                CHECK(value.member_color[snapshot.slot] ==
                      RGB_COLOR_HSB_VAL(snapshot.color.h, snapshot.color.s, snapshot.color.b));
                CHECK(value.member_period[snapshot.slot] == snapshot.period_ms);
            }
            CHECK(kp_rgb_selector_snapshot_locked(owner, &snapshot) == 0);
            kp_rgb_matrix_unlock();
            CHECK(value.selected == snapshot.slot && value.phase == 0);
        }
    }
    printk("PASS scoped split global reset repairs all selectors/members\n");
    printk("PASS scoped split transport acceptance only (%u calls)\n", total_calls);
    exit(0);
}
K_THREAD_DEFINE(split_tests, 2048, test_thread, NULL, NULL, NULL, 8, 0, 0);
#endif
#else
#include <zmk/split/transport/peripheral.h>
static unsigned peripheral_reports;
static int report_event(const struct zmk_split_transport_peripheral_event *event) {
    ARG_UNUSED(event); peripheral_reports++; return 0;
}
static const struct zmk_split_transport_peripheral_api receive_api = {.report_event = report_event};
ZMK_SPLIT_TRANSPORT_PERIPHERAL_REGISTER(receive_mock, &receive_api, 0);
static int receive(const struct device *endpoint, uint32_t command, uint32_t argument) {
    struct zmk_split_transport_central_command cmd = {
        .type = ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_INVOKE_BEHAVIOR};
    CHECK(strlen(endpoint->name) < sizeof(cmd.data.invoke_behavior.behavior_dev));
    strcpy(cmd.data.invoke_behavior.behavior_dev, endpoint->name);
    cmd.data.invoke_behavior.param1 = command;
    cmd.data.invoke_behavior.param2 = argument;
    cmd.data.invoke_behavior.state = 1;
    return zmk_split_transport_peripheral_command_handler(&receive_mock, cmd);
}
static int irq_result;
static void receiving_irq(const void *unused) {
    ARG_UNUSED(unused);
    CHECK(k_is_in_isr());
    irq_result = receive(DEV(rgbs_a), RGB_EFS_CMD, 1);
}
static void receiving(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    for (unsigned i = 0; i < 100 &&
         zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL; i++) k_sleep(K_MSEC(5));
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
    k_sleep(K_MSEC(100));
    struct kp_rgb_hsb a_color = kp_rgb_effect_data(DEV(probe_a))->color;
    struct kp_rgb_hsb b_color = kp_rgb_effect_data(DEV(probe_b))->color;
    uint32_t a_period = kp_rgb_effect_period(DEV(probe_a)), b_period = kp_rgb_effect_period(DEV(probe_b));
    struct kp_rgb_hsb d_color = kp_rgb_effect_data(DEV(probe_d))->color;
    uint32_t d_period = kp_rgb_effect_period(DEV(probe_d));
    CHECK(receive(DEV(rgb_matrix), RGB_BRT_SET_CMD, 37) == 0);
    bool initial_on;
    CHECK(zmk_rgb_matrix_get_user_state(&initial_on) == 0);
    const unsigned levels[] = {37, 0};
    for (size_t i = 0; i < ARRAY_SIZE(levels); i++) {
        CHECK(receive(DEV(rgbs_a), KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 2),
                      RGB_COLOR_HSB_VAL(220, 70, levels[i])) == 0);
        CHECK(receive(DEV(rgbs_a), KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, 2), 2300) == 0);
        CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
        CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
        struct kp_rgb_hsb color = kp_rgb_effect_data(DEV(probe_c))->color;
        CHECK(color.h == 220 && color.s == 70 && color.b == levels[i]);
        CHECK(kp_rgb_effect_period(DEV(probe_c)) == 2300);
        color = kp_rgb_effect_data(DEV(probe_a))->color;
        CHECK(color.h == a_color.h && color.s == a_color.s && color.b == a_color.b);
        color = kp_rgb_effect_data(DEV(probe_b))->color;
        CHECK(color.h == b_color.h && color.s == b_color.s && color.b == b_color.b);
        color = kp_rgb_effect_data(DEV(probe_d))->color;
        CHECK(color.h == d_color.h && color.s == d_color.s && color.b == d_color.b);
        CHECK(kp_rgb_effect_period(DEV(probe_a)) == a_period);
        CHECK(kp_rgb_effect_period(DEV(probe_b)) == b_period && kp_rgb_effect_period(DEV(probe_d)) == d_period);
        bool on;
        CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && on == initial_on);
        CHECK(kp_rgb_global.brightness == 37);
    }
    k_sleep(K_MSEC(200)); CHECK(peripheral_reports == 0);
    CHECK(receive(DEV(rgbs_a), RGB_EFS_CMD, 2) == 0);
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_c));
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
    CHECK(receive(DEV(rgbs_a), KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 0), 0) == -ENOENT);
    const uint32_t colors[] = {RGB_COLOR_HSB_VAL(361, 1, 100), RGB_COLOR_HSB_VAL(1, 101, 100),
                              RGB_COLOR_HSB_VAL(1, 1, 101), RGB_COLOR_HSB_VAL(1, 1, 255), UINT32_MAX};
    for (size_t i = 0; i < ARRAY_SIZE(colors); i++)
        CHECK(receive(DEV(rgbs_a), KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 2), colors[i]) == -EINVAL);
    CHECK(kp_rgb_effect_data(DEV(probe_c))->color.h == 220 &&
          kp_rgb_effect_data(DEV(probe_c))->color.s == 70 && kp_rgb_effect_data(DEV(probe_c))->color.b == 0);
    const uint32_t periods[] = {0, CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS - 1,
                               CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS + 1, UINT32_MAX};
    for (size_t i = 0; i < ARRAY_SIZE(periods); i++)
        CHECK(receive(DEV(rgbs_a), KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, 2), periods[i]) == -EINVAL);
    const uint32_t slots[] = {0, UINT16_MAX, 0x10000, 0x10002, UINT32_MAX};
    for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
        CHECK(receive(DEV(rgbs_a), RGB_EFS_CMD, slots[i]) < 0);
        if (slots[i] <= UINT16_MAX) {
            CHECK(receive(DEV(rgbs_a), KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, slots[i]), 0) < 0);
            CHECK(receive(DEV(rgbs_a), KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, slots[i]), 2300) < 0);
        }
    }
    CHECK(receive(DEV(rgbs_a), 0x10a, 0) == -ENOTSUP);
    CHECK(receive(DEV(rgbs_a), 0xffff, 0) == -ENOTSUP);
    CHECK(kp_rgb_selector_binding_apply(NULL, RGB_EFS_CMD, 1) == -ENODEV);
    struct zmk_behavior_binding no_control = {.behavior_dev = NULL, .param1 = RGB_EFS_CMD, .param2 = 1};
    CHECK(behavior_keymap_binding_pressed(&no_control, (struct zmk_behavior_binding_event){0}) == -EINVAL);
    irq_offload(receiving_irq, NULL);
    CHECK(irq_result == -EWOULDBLOCK);
    CHECK(receive(DEV(rgbs_a), KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 2), RGB_COLOR_HSB_VAL(220, 70, 0)) == 0);
    CHECK(receive(DEV(rgbs_a), KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, 2), 2300) == 0);
    CHECK(receive(DEV(rgbs_a), RGB_EFS_CMD, 2) == 0);
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_c));
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
    CHECK(kp_rgb_effect_data(DEV(probe_c))->color.h == 220 && kp_rgb_effect_data(DEV(probe_c))->color.s == 70 &&
          kp_rgb_effect_data(DEV(probe_c))->color.b == 0 && kp_rgb_effect_period(DEV(probe_c)) == 2300);
    CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == a_color.h &&
          kp_rgb_effect_data(DEV(probe_a))->color.s == a_color.s && kp_rgb_effect_data(DEV(probe_a))->color.b == a_color.b);
    CHECK(kp_rgb_effect_data(DEV(probe_b))->color.h == b_color.h &&
          kp_rgb_effect_data(DEV(probe_b))->color.s == b_color.s && kp_rgb_effect_data(DEV(probe_b))->color.b == b_color.b);
    CHECK(kp_rgb_effect_period(DEV(probe_a)) == a_period && kp_rgb_effect_period(DEV(probe_b)) == b_period);
    CHECK(receive(DEV(rgb_matrix), RGB_BRT_SET_CMD, 37) == 0 && kp_rgb_global.brightness == 37);
    CHECK(receive(DEV(rgb_matrix), RGB_OFF_CMD, 0) == 0);
    bool on; CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && !on);
    const int32_t offsets[] = {0, 1500, -2000};
    for (size_t i = 0; i < ARRAY_SIZE(offsets); i++) {
        kp_rgb_set_clock_offset(offsets[i]);
        CHECK(kp_rgb_clock_ms() == (uint32_t)k_uptime_get() + (uint32_t)offsets[i]);
    }
    const uint32_t central[] = {123456, 10, UINT32_MAX - 15};
    for (size_t i = 0; i < ARRAY_SIZE(central); i++) {
        CHECK(receive(DEV(rgb_matrix), RGB_CLOCK_CMD, central[i]) == 0);
        CHECK(kp_rgb_clock_ms() == central[i]);
        uint32_t before = (uint32_t)k_uptime_get();
        k_sleep(K_MSEC(17));
        CHECK(kp_rgb_clock_ms() == central[i] + (uint32_t)k_uptime_get() - before);
    }
    CHECK(receive(DEV(rgbs_a), RGB_RESET_CMD, 0) == 0);
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
    CHECK(kp_rgb_effect_data(DEV(probe_c))->color.h == 240);
    CHECK(receive(DEV(rgb_matrix), RGB_RESET_CMD, 0) == 0);
    CHECK(kp_rgb_global.brightness == 100);
    k_sleep(K_MSEC(200)); CHECK(peripheral_reports == 0);
    printk("PASS split receiving\n"); exit(0);
}
K_THREAD_DEFINE(split_receiving, 4096, receiving, NULL, NULL, NULL, 8, 0, 0);
#endif
