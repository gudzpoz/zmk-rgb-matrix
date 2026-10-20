/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#include <zmk/events/position_state_changed.h>

struct singleton_send {
    uint8_t source;
    uint32_t command, argument;
    int64_t start, end;
    bool accepted, ordinary, complete;
};
static struct singleton_send singleton_sent[512];
static unsigned singleton_count;
static uint32_t singleton_remote_brightness[2], singleton_remote_power[2];
static uint16_t singleton_remote_words[2][2];
static unsigned singleton_clocks[2];
static atomic_t singleton_connected, singleton_alternate, singleton_query_error;
static uint32_t singleton_fail_command;
static int singleton_fail_word = -1, singleton_race_phase = -1;
static unsigned singleton_duration, singleton_progress;
static bool singleton_mutate, singleton_fail_once;
static zmk_split_transport_central_status_changed_cb_t singleton_cb;
K_MUTEX_DEFINE(singleton_records_lock);
K_SEM_DEFINE(singleton_packet_done, 0, 512);
K_SEM_DEFINE(singleton_probe_request, 0, 1);
K_SEM_DEFINE(singleton_probe_done, 0, 1);
K_MUTEX_DEFINE(singleton_probe_lock);
static void singleton_probe(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    for (;;) {
        CHECK(k_sem_take(&singleton_probe_request, K_FOREVER) == 0);
        kp_rgb_matrix_lock(); kp_rgb_matrix_unlock();
        k_sem_give(&singleton_probe_done);
    }
}
K_THREAD_DEFINE(singleton_probe_thread, 1024, singleton_probe, NULL, NULL, NULL, 6, 0, 0);
static void singleton_unlocked(void) {
    k_mutex_lock(&singleton_probe_lock, K_FOREVER);
    k_sem_give(&singleton_probe_request);
    CHECK(k_sem_take(&singleton_probe_done, K_MSEC(100)) == 0);
    k_mutex_unlock(&singleton_probe_lock);
}

struct singleton_frontend_scope { k_tid_t thread; unsigned depth; };
static struct singleton_frontend_scope singleton_frontend[4];
K_MUTEX_DEFINE(singleton_frontend_lock);
extern __typeof__(zmk_behavior_invoke_binding) __real_zmk_behavior_invoke_binding;

int __wrap_zmk_behavior_invoke_binding(const struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event, bool pressed) {
    k_tid_t thread = k_current_get();
    k_mutex_lock(&singleton_frontend_lock, K_FOREVER);
    unsigned slot = ARRAY_SIZE(singleton_frontend);
    for (unsigned i = 0; i < ARRAY_SIZE(singleton_frontend); i++)
        if (singleton_frontend[i].thread == thread) { slot = i; break; }
    if (slot == ARRAY_SIZE(singleton_frontend))
        for (unsigned i = 0; i < ARRAY_SIZE(singleton_frontend); i++)
            if (!singleton_frontend[i].depth) { slot = i; break; }
    CHECK(slot < ARRAY_SIZE(singleton_frontend));
    singleton_frontend[slot].thread = thread;
    singleton_frontend[slot].depth++;
    k_mutex_unlock(&singleton_frontend_lock);
    int result = __real_zmk_behavior_invoke_binding(binding, event, pressed);
    k_mutex_lock(&singleton_frontend_lock, K_FOREVER);
    CHECK(singleton_frontend[slot].thread == thread && singleton_frontend[slot].depth);
    if (!--singleton_frontend[slot].depth) singleton_frontend[slot].thread = NULL;
    k_mutex_unlock(&singleton_frontend_lock);
    return result;
}

static bool singleton_frontend_active(void) {
    bool active = false;
    k_mutex_lock(&singleton_frontend_lock, K_FOREVER);
    for (unsigned i = 0; i < ARRAY_SIZE(singleton_frontend); i++)
        active |= singleton_frontend[i].depth && singleton_frontend[i].thread == k_current_get();
    k_mutex_unlock(&singleton_frontend_lock);
    return active;
}

static unsigned singleton_records(void) {
    k_mutex_lock(&singleton_records_lock, K_FOREVER);
    unsigned n = singleton_count;
    k_mutex_unlock(&singleton_records_lock);
    return n;
}
static struct singleton_send singleton_record(unsigned n) {
    k_mutex_lock(&singleton_records_lock, K_FOREVER);
    CHECK(n < singleton_count);
    struct singleton_send sent = singleton_sent[n];
    k_mutex_unlock(&singleton_records_lock);
    return sent;
}
static void singleton_global(uint32_t command, uint32_t value) {
    struct zmk_behavior_binding binding = {
        .behavior_dev = DEV(rgb_matrix)->name, .param1 = command, .param2 = value};
    CHECK(behavior_keymap_binding_pressed(&binding, (struct zmk_behavior_binding_event){0}) == 0);
}
static void singleton_ordinary(uint32_t command, uint32_t value) {
    struct zmk_behavior_binding binding = {
        .behavior_dev = DEV(rgb_matrix)->name, .param1 = command, .param2 = value};
    struct zmk_behavior_binding_event event = {
        .position = 42, .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
        .timestamp = k_uptime_get()};
    CHECK(zmk_behavior_invoke_binding(&binding, event, true) == 0);
}
static void singleton_latest(void) {
    singleton_ordinary(RGB_OFF_CMD, 0);
    singleton_ordinary(RGB_BRT_SET_CMD, 19);
}
static int singleton_send_command(uint8_t source, struct zmk_split_transport_central_command cmd) {
    if (source >= 2 || !(atomic_get(&singleton_connected) & BIT(source))) return -ENOTCONN;
    CHECK(cmd.type == ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_INVOKE_BEHAVIOR);
    singleton_unlocked();
    CHECK(!strcmp(cmd.data.invoke_behavior.behavior_dev, DEV(rgb_matrix)->name));
    bool ordinary = singleton_frontend_active();
    CHECK(cmd.data.invoke_behavior.state == 1);
    CHECK(ordinary || cmd.data.invoke_behavior.event_source == source);
    if (cmd.data.invoke_behavior.position == 42)
        CHECK(cmd.data.invoke_behavior.event_source == ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL);
    uint32_t command = cmd.data.invoke_behavior.param1, argument = cmd.data.invoke_behavior.param2;
    bool fail = !ordinary && !source && command == singleton_fail_command &&
        (command != RGB_OVL_STATE_CMD || singleton_fail_word < 0 ||
         (argument >> 16) == singleton_fail_word);
    bool mutate = false;
    if (!ordinary && !source) {
        if (command == RGB_CLOCK_CMD) {
            mutate = singleton_mutate && singleton_race_phase == 4;
        } else {
            mutate = singleton_mutate &&
                (singleton_race_phase < 0 ? fail : singleton_progress == singleton_race_phase);
            if (!fail) singleton_progress++;
        }
        if (mutate) singleton_mutate = false;
    }
    if (fail && singleton_fail_once) singleton_fail_command = 0;
    k_mutex_lock(&singleton_records_lock, K_FOREVER);
    CHECK(singleton_count < ARRAY_SIZE(singleton_sent));
    unsigned index = singleton_count++;
    singleton_sent[index] = (struct singleton_send){
        .source = source, .command = command, .argument = argument,
        .start = k_uptime_get(), .accepted = !fail, .ordinary = ordinary};
    k_mutex_unlock(&singleton_records_lock);
    if (mutate) singleton_latest();
    k_mutex_lock(&singleton_records_lock, K_FOREVER);
    if (!fail) {
        if (command == RGB_BRT_SET_CMD) singleton_remote_brightness[source] = argument;
        else if (command == RGB_ON_CMD || command == RGB_OFF_CMD) singleton_remote_power[source] = command;
        else if (command == RGB_CLOCK_CMD) singleton_clocks[source]++;
        else {
            CHECK(command == RGB_OVL_STATE_CMD && (argument >> 16) < 2);
            singleton_remote_words[source][argument >> 16] = argument;
        }
    }
    k_mutex_unlock(&singleton_records_lock);
    if (singleton_duration) k_sleep(K_MSEC(singleton_duration));
    k_mutex_lock(&singleton_records_lock, K_FOREVER);
    singleton_sent[index].end = k_uptime_get();
    singleton_sent[index].complete = true;
    k_mutex_unlock(&singleton_records_lock);
    k_sem_give(&singleton_packet_done);
    return fail ? -EIO : 0;
}
static int singleton_sources(uint8_t *sources) {
    if (atomic_get(&singleton_connected)) singleton_unlocked();
    if (atomic_get(&singleton_query_error)) return -EIO;
    unsigned count = 0;
    for (unsigned i = 0; i < 2; i++)
        if (atomic_get(&singleton_connected) & BIT(i)) sources[count++] = i;
    return count;
}
static struct zmk_split_transport_status singleton_status(void) {
    return (struct zmk_split_transport_status){
        .available = !atomic_get(&singleton_alternate), .enabled = true,
        .connections = atomic_get(&singleton_connected) ?
            ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_SOME_CONNECTED :
            ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED};
}
static struct zmk_split_transport_status singleton_alt_status(void) {
    struct zmk_split_transport_status status = singleton_status();
    status.available = atomic_get(&singleton_alternate);
    return status;
}
static int singleton_callback(zmk_split_transport_central_status_changed_cb_t cb) {
    singleton_cb = cb; return 0;
}
static int singleton_enabled(bool enabled) { ARG_UNUSED(enabled); return 0; }
static const struct zmk_split_transport_central_api singleton_transport_api = {
    .send_command = singleton_send_command, .get_available_source_ids = singleton_sources,
    .get_status = singleton_status, .set_status_callback = singleton_callback,
    .set_enabled = singleton_enabled};
static const struct zmk_split_transport_central_api singleton_alt_api = {
    .send_command = singleton_send_command, .get_available_source_ids = singleton_sources,
    .get_status = singleton_alt_status, .set_status_callback = singleton_callback,
    .set_enabled = singleton_enabled};
ZMK_SPLIT_TRANSPORT_CENTRAL_REGISTER(singleton_mock, &singleton_transport_api, 0);
ZMK_SPLIT_TRANSPORT_CENTRAL_REGISTER(singleton_alt_mock, &singleton_alt_api, 0);
static void singleton_connect(unsigned mask) {
    atomic_set(&singleton_connected, mask);
    struct zmk_split_transport_status value = singleton_status();
    if (atomic_get(&singleton_alternate))
        value.connections = ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED;
    CHECK(singleton_cb(&singleton_mock, value) == 0);
}
static void singleton_masks(void) {
    int64_t until = k_uptime_get() + 1000;
    while (kp_rgb_overlay_get_word(0) != 0x1234 || kp_rgb_overlay_get_word(1) != 0xabcd) {
        CHECK(k_uptime_get() < until); k_sleep(K_MSEC(1));
    }
    CHECK(KP_RGB_OVERLAY_WORDS == 2);
}
static void singleton_remote(uint32_t brightness, uint32_t power) {
    k_mutex_lock(&singleton_records_lock, K_FOREVER);
    for (unsigned i = 0; i < 2; i++) {
        CHECK(singleton_remote_brightness[i] == brightness);
        CHECK(singleton_remote_power[i] == power);
        CHECK(singleton_remote_words[i][0] == 0x1234 && singleton_remote_words[i][1] == 0xabcd);
    }
    k_mutex_unlock(&singleton_records_lock);
    unsigned settled_commands = 0;
    for (unsigned i = 0; i < singleton_records(); i++) {
        struct singleton_send sent = singleton_record(i);
        settled_commands += !sent.ordinary && sent.command != RGB_CLOCK_CMD;
    }
    k_sleep(K_MSEC(150));
    unsigned after = 0;
    for (unsigned i = 0; i < singleton_records(); i++) {
        struct singleton_send sent = singleton_record(i);
        after += !sent.ordinary && sent.command != RGB_CLOCK_CMD;
    }
    CHECK(after == settled_commands);
}
static void singleton_wait_stream(unsigned minimum) {
    int64_t until = k_uptime_get() + 5000;
    for (;;) {
        unsigned complete = 0;
        for (unsigned i = 0; i < singleton_records(); i++) {
            struct singleton_send sent = singleton_record(i);
            complete += !sent.source && !sent.ordinary && sent.command != RGB_CLOCK_CMD &&
                        sent.accepted && sent.complete;
        }
        if (complete >= minimum) return;
        CHECK(k_uptime_get() < until); CHECK(k_sem_take(&singleton_packet_done, K_MSEC(100)) == 0 ||
                                             k_uptime_get() < until);
    }
}
static unsigned singleton_phase(const char *name) {
    if (strstr(name, "complete")) return 4;
    if (strstr(name, "mask-1")) return 3;
    if (strstr(name, "mask-0") || strstr(name, "overlay")) return 2;
    if (strstr(name, "power")) return 1;
    return 0;
}
static uint32_t singleton_phase_command(unsigned phase) {
    return phase == 0 ? RGB_BRT_SET_CMD : phase == 1 ? RGB_ON_CMD : RGB_OVL_STATE_CMD;
}
static void singleton_repair(void) {
    unsigned phase = singleton_phase(split_case);
    singleton_race_phase = phase;
    singleton_mutate = phase < 4;
    if (strstr(split_case, "failed")) {
        CHECK(phase < 4);
        singleton_fail_command = singleton_phase_command(phase);
        singleton_fail_word = phase == 3 ? 1 : 0;
        singleton_fail_once = true;
    }
    singleton_connect(3);
    singleton_wait_stream(4);
    if (phase == 4) {
        k_sleep(K_MSEC(250)); singleton_mutate = true;
    }
    singleton_wait_stream(8);
    k_sleep(K_MSEC(250)); singleton_remote(19, RGB_OFF_CMD);
    unsigned accepted = 0, failed = 0;
    const uint32_t commands[] = {RGB_BRT_SET_CMD, RGB_ON_CMD, RGB_OVL_STATE_CMD, RGB_OVL_STATE_CMD,
                                RGB_BRT_SET_CMD, RGB_OFF_CMD, RGB_OVL_STATE_CMD, RGB_OVL_STATE_CMD};
    const uint32_t arguments[] = {61, 0, RGB_OVL_STATE_VAL(0, 0x1234), RGB_OVL_STATE_VAL(1, 0xabcd),
                                 19, 0, RGB_OVL_STATE_VAL(0, 0x1234), RGB_OVL_STATE_VAL(1, 0xabcd)};
    for (unsigned i = 0; i < singleton_records(); i++) {
        struct singleton_send sent = singleton_record(i);
        if (sent.source || sent.ordinary || sent.command == RGB_CLOCK_CMD) continue;
        CHECK(accepted < ARRAY_SIZE(commands));
        CHECK(sent.command == commands[accepted] && sent.argument == arguments[accepted]);
        if (sent.accepted) accepted++; else failed++;
    }
    CHECK(accepted == 8 && failed == !!strstr(split_case, "failed"));
}
static void singleton_frozen(void) {
    unsigned phase = singleton_phase(split_case);
    singleton_fail_command = singleton_phase_command(phase);
    singleton_fail_word = phase == 3 ? 1 : 0;
    singleton_fail_once = true;
    singleton_connect(3);
    int64_t until = k_uptime_get() + 5000;
    bool failed = false, peer_captured = false;
    while (!failed || !peer_captured) {
        for (unsigned i = 0; i < singleton_records(); i++) {
            struct singleton_send sent = singleton_record(i);
            failed |= !sent.source && !sent.accepted && sent.complete;
            peer_captured |= sent.source == 1 && sent.command == RGB_BRT_SET_CMD &&
                             sent.argument == 61 && sent.accepted && sent.complete;
        }
        CHECK(k_uptime_get() < until); k_sleep(K_MSEC(1));
    }
    kp_rgb_matrix_lock(); kp_rgb_global.brightness = 19; kp_rgb_matrix_unlock();
    CHECK(zmk_rgb_matrix_off() == 0);
    singleton_wait_stream(4); k_sleep(K_MSEC(250)); singleton_remote(61, RGB_ON_CMD);
    unsigned old_end = singleton_records();
    singleton_connect(0); k_sleep(K_MSEC(150)); singleton_connect(3);
    singleton_wait_stream(8); k_sleep(K_MSEC(250)); singleton_remote(19, RGB_OFF_CMD);
    bool refreshed = false;
    for (unsigned i = old_end; i < singleton_records(); i++) {
        struct singleton_send sent = singleton_record(i);
        refreshed |= !sent.source && !sent.ordinary && sent.accepted &&
                     sent.command == RGB_BRT_SET_CMD && sent.argument == 19;
    }
    CHECK(refreshed);
}
static void singleton_pacing(void) {
    singleton_fail_command = singleton_phase_command(singleton_phase(split_case));
    singleton_fail_word = strstr(split_case, "overlay-1") ? 1 : -1;
    singleton_mutate = strstr(split_case, "race") != NULL;
    singleton_connect(3); k_sleep(K_MSEC(6000));
    k_mutex_lock(&singleton_records_lock, K_FOREVER);
    CHECK(singleton_remote_brightness[1] == (strstr(split_case, "race") ? 19 : 61));
    CHECK(singleton_clocks[0] >= 4 && singleton_clocks[1] >= 4);
    k_mutex_unlock(&singleton_records_lock);
    struct singleton_send previous = {0};
    unsigned failures = 0, consecutive = 0;
    const unsigned delays[] = {50, 100, 200, 400, 800, 1000};
    for (unsigned i = 0; i < singleton_records(); i++) {
        struct singleton_send sent = singleton_record(i);
        if (sent.source || sent.ordinary || sent.command == RGB_CLOCK_CMD || !sent.complete) continue;
        if (previous.complete)
            CHECK(sent.start >= previous.end + (previous.accepted ? 50 : delays[MIN(consecutive - 1, 5)]));
        if (!sent.accepted) { failures++; consecutive++; } else consecutive = 0;
        previous = sent;
    }
    CHECK(failures >= 7);
    singleton_fail_command = 0; singleton_wait_stream(strstr(split_case, "race") ? 8 : 4); k_sleep(K_MSEC(350));
    singleton_remote(strstr(split_case, "race") ? 19 : 61,
                     strstr(split_case, "race") ? RGB_OFF_CMD : RGB_ON_CMD);
    singleton_connect(0); k_sleep(K_MSEC(150));
    unsigned before = singleton_records();
    singleton_global(RGB_BRT_SET_CMD, 77);
    atomic_set(&singleton_alternate, 1); singleton_connect(3);
    k_sleep(K_MSEC(20)); CHECK(singleton_records() == before);
    k_sleep(K_MSEC(1500));
    singleton_remote(77, strstr(split_case, "race") ? RGB_OFF_CMD : RGB_ON_CMD);
    singleton_connect(0); k_sleep(K_MSEC(100));
    before = singleton_records(); k_sleep(K_MSEC(1100)); CHECK(singleton_records() == before);
}
static void singleton_negative_query(void) {
    singleton_connect(3); singleton_wait_stream(4); k_sleep(K_MSEC(250));
    atomic_set(&singleton_query_error, 1); k_sleep(K_MSEC(150));
    unsigned before = singleton_records();
    k_sleep(K_MSEC(1200)); CHECK(singleton_records() == before);
    atomic_set(&singleton_query_error, 0); singleton_wait_stream(8);
    k_sleep(K_MSEC(250)); singleton_remote(61, RGB_ON_CMD);
}
static void singleton_unready(void) {
    CHECK(!device_is_ready(DEV(rgb_matrix)));
    kp_rgb_matrix_lock(); kp_rgb_global.brightness = 61; kp_rgb_matrix_unlock();
    singleton_connect(3); k_sleep(K_MSEC(1300)); CHECK(singleton_records() == 0);
    CHECK(device_init(DEV(rgb_matrix)) == 0 && device_is_ready(DEV(rgb_matrix)));
    singleton_masks(); singleton_global(RGB_BRT_SET_CMD, 61);
    singleton_wait_stream(8); k_sleep(K_MSEC(350)); singleton_remote(61, RGB_ON_CMD);
}
static void singleton_split(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c); k_sleep(K_MSEC(100));
    singleton_duration = 37;
    CHECK(zmk_rgb_matrix_on() == 0); CHECK(zmk_rgb_matrix_set_inhibited(true) == 0);
    if (!strcmp(split_case, "unready")) singleton_unready();
    else {
        singleton_masks(); singleton_global(RGB_BRT_SET_CMD, 61);
        if (!strncmp(split_case, "revision-", 9)) singleton_repair();
        else if (!strncmp(split_case, "frozen-", 7)) singleton_frozen();
        else if (!strcmp(split_case, "query-error")) singleton_negative_query();
        else singleton_pacing();
    }
    printk("PASS singleton split %s\n", split_case); exit(0);
}
K_THREAD_DEFINE(singleton_split_thread, 4096, singleton_split, NULL, NULL, NULL, 8, 0, 0);
