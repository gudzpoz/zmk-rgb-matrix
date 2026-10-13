/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>
#include <zephyr/sys/printk.h>
#include <zmk/rgb_selector.h>
#include <drivers/behavior.h>
#include <zmk/split/transport/central.h>
#include "rgb_matrix_internal.h"
#include "rgb_selector_sync_internal.h"

#define DEV(label) DEVICE_DT_GET(DT_NODELABEL(label))
#define CHECK(test) do { if (!(test)) { \
    printk("FAIL scoped split: %s line %d\n", #test, __LINE__); exit(1); \
} } while (0)

struct lane {
    unsigned phase, slot, selected, completed;
    uint32_t color, period, selected_color, selected_period;
    uint32_t member_color[3], member_period[3];
};
struct attempt {
    uint8_t source;
    char endpoint[16];
    uint32_t command, argument;
    int64_t time;
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
    struct zmk_behavior_binding binding = {.behavior_dev = DEV(kprgb)->name,
        .param1 = command, .param2 = argument};
    CHECK(behavior_keymap_binding_pressed(&binding, (struct zmk_behavior_binding_event){0}) == 0);
}
static unsigned connected;
static bool fail_a;
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
        CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) != NULL);
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
    CHECK(cmd.data.invoke_behavior.state == 1);
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
                     (command & KP_RGB_SELECTOR_COMMAND_MASK) == RGB_SELECTOR_COLOR_CMD);
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
        CHECK(!strcmp(endpoint, DEV(kprgb)->name));
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
        }
    }
    bool inject = global_race && index < 0 && command == RGB_BRT_SET_CMD;
    if (inject) global_race = false;
    k_mutex_unlock(&records_lock);
    if (inject) {
        global_apply(RGB_OFF_CMD, 0);
        global_apply(RGB_BRT_SET_CMD, 19);
        k_mutex_lock(&records_lock, K_FOREVER);
        power[source] = RGB_OFF_CMD;
        brightness[source] = 19;
        k_mutex_unlock(&records_lock);
    }
    return accepted ? 0 : -EIO;
}

static int sources(uint8_t *ids) {
    k_mutex_lock(&records_lock, K_FOREVER);
    int count = 0;
    for (unsigned i = 0; i < 2; i++)
        if (connected & BIT(i)) ids[count++] = i;
    k_mutex_unlock(&records_lock);
    return count;
}
static struct zmk_split_transport_status status(void) {
    k_mutex_lock(&records_lock, K_FOREVER);
    bool present = connected != 0;
    k_mutex_unlock(&records_lock);
    return (struct zmk_split_transport_status){.available = true, .enabled = true,
        .connections = present ? ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_SOME_CONNECTED :
                                ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED};
}
static int set_callback(zmk_split_transport_central_status_changed_cb_t cb) {
    status_cb = cb;
    return 0;
}
static int set_enabled(bool enabled) { CHECK(enabled); return 0; }
static const struct zmk_split_transport_central_api transport_api = {
    .send_command = send_command, .get_available_source_ids = sources,
    .get_status = status, .set_status_callback = set_callback, .set_enabled = set_enabled,
};
ZMK_SPLIT_TRANSPORT_CENTRAL_REGISTER(scoped_mock, &transport_api, 0);

static void connect(unsigned mask) {
    k_mutex_lock(&records_lock, K_FOREVER);
    connected = mask;
    k_mutex_unlock(&records_lock);
    CHECK(status_cb != NULL);
    CHECK(status_cb(&scoped_mock, status()) == 0);
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
static void parameters(const struct device *selector, unsigned slot, unsigned hue, unsigned period) {
    apply(selector, KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, slot),
          RGB_COLOR_HSB_VAL(hue, 100, 100));
    apply(selector, KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, slot), period);
}
static void matches(unsigned source, unsigned index, unsigned slot, unsigned hue, unsigned period) {
    struct lane value = lane(source, index);
    CHECK(value.completed > 0 && value.phase == 0);
    CHECK(value.selected == slot);
    CHECK(value.selected_color == RGB_COLOR_HSB_VAL(hue, 100, 100));
    CHECK(value.selected_period == period);
}

static void test_thread(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    k_sleep(K_MSEC(100));
    CHECK(zmk_rgb_matrix_off() == 0);
    CHECK(zmk_rgb_matrix_set_inhibited(true) == 0);
    apply(DEV(selector_a), RGB_EFS_CMD, 1);
    parameters(DEV(selector_a), 1, 10, 1100);
    apply(DEV(selector_b), RGB_EFS_CMD, 0);
    parameters(DEV(selector_b), 0, 20, 1200);
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
    kp_rgb_matrix_lock();
    CHECK(kp_rgb_selector_snapshot_locked(DEV(selector_a), &snapshot) == 0);
    kp_rgb_matrix_unlock();
    CHECK(kp_rgb_split_send_selector(0, &snapshot, 3) == -EINVAL);
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
}
K_THREAD_DEFINE(split_tests, 2048, test_thread, NULL, NULL, NULL, 8, 0, 0);
