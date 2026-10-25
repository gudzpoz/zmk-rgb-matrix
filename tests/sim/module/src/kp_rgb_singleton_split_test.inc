/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#include <zmk/events/position_state_changed.h>
#include "rgb_replication_internal.h"

struct singleton_send {
    uint8_t source;
    uint32_t command, argument;
    int64_t start, end;
    bool accepted, ordinary, complete;
};
static struct singleton_send singleton_sent[512];
static unsigned singleton_count;
static uint32_t singleton_remote_brightness[2], singleton_remote_power[2];
static uint32_t singleton_remote_mask[2][KP_RGB_OVERLAY_WORDS];
#define SINGLETON_PASS (2 + KP_RGB_OVERLAY_WORDS)
static unsigned singleton_clocks[2];
static uint32_t singleton_expected_mask = UINT32_C(0xabcd1234);
static uint32_t singleton_expected_word(size_t word) {
    if (!word) return singleton_expected_mask;
    if (singleton_expected_mask == UINT32_C(0x12345678)) return UINT32_C(0x12345678) ^ word;
    return word == 1 ? UINT32_C(0x80000001) : 1;
}
static atomic_t singleton_connected, singleton_alternate, singleton_query_error,
    singleton_fail_persistently;
static uint32_t singleton_fail_command;
static unsigned singleton_duration;
static bool singleton_fail_once, singleton_mutate_on_failure;
static int singleton_mutation_phase = -1;
static bool singleton_mutation_once;
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
    kp_rgb_matrix_lock();
    for (size_t word = 0; word < KP_RGB_OVERLAY_WORDS; word++)
        kp_rgb_overlay_set_word(word, UINT32_C(0x12345678) ^ word);
    kp_rgb_matrix_unlock();
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
    uint32_t command = kp_rgb_invocation_command(cmd.data.invoke_behavior.param1);
    uint32_t argument = cmd.data.invoke_behavior.param2;
    bool fail = !ordinary && !source && command == singleton_fail_command &&
                (atomic_get(&singleton_fail_persistently) || singleton_fail_once);
    if (fail && singleton_fail_once) singleton_fail_once = false;
    k_mutex_lock(&singleton_records_lock, K_FOREVER);
    CHECK(singleton_count < ARRAY_SIZE(singleton_sent));
    unsigned index = singleton_count++;
    singleton_sent[index] = (struct singleton_send){
        .source = source, .command = command, .argument = argument,
        .start = k_uptime_get(), .accepted = !fail, .ordinary = ordinary};
    k_mutex_unlock(&singleton_records_lock);
    k_mutex_lock(&singleton_records_lock, K_FOREVER);
    if (!fail) {
        if (command == RGB_BRT_SET_CMD) singleton_remote_brightness[source] = argument;
        else if (command == RGB_ON_CMD || command == RGB_OFF_CMD) singleton_remote_power[source] = command;
        else if (command == RGB_CLOCK_CMD) singleton_clocks[source]++;
        else {
            CHECK((command & 0xffffu) == RGB_OVL_STATE_CMD);
            size_t word = KP_RGB_OVERLAY_COMMAND_WORD(command);
            CHECK(word < KP_RGB_OVERLAY_WORDS);
            singleton_remote_mask[source][word] = argument;
        }
    }
    k_mutex_unlock(&singleton_records_lock);
    bool mutate = !ordinary && !source && singleton_mutation_once &&
        ((singleton_mutation_phase == 0 && command == RGB_BRT_SET_CMD) ||
         (singleton_mutation_phase == 1 && (command == RGB_ON_CMD || command == RGB_OFF_CMD)) ||
         (singleton_mutation_phase == 2 && command == KP_RGB_OVERLAY_WORD_COMMAND(
             !strcmp(split_case, "race-final") ? KP_RGB_OVERLAY_WORDS - 1 :
             !strcmp(split_case, "race-later") ? 1 : 0)));
    if (mutate) {
        singleton_mutation_once = false;
        singleton_latest();
    }
    if (fail && singleton_mutate_on_failure) {
        singleton_mutate_on_failure = false;
        singleton_latest();
    }
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
    CHECK(singleton_cb(&singleton_mock, singleton_status()) == 0);
    CHECK(singleton_cb(&singleton_alt_mock, singleton_alt_status()) == 0);
}
static void singleton_masks(void) {
    int64_t until = k_uptime_get() + 1000;
    for (;;) {
        bool ready = true;
        kp_rgb_matrix_lock();
        for (size_t word = 0; word < KP_RGB_OVERLAY_WORDS; word++)
            ready &= kp_rgb_overlay_get_word(word) == singleton_expected_word(word);
        kp_rgb_matrix_unlock();
        if (ready) break;
        CHECK(k_uptime_get() < until); k_sleep(K_MSEC(1));
    }
    if (KP_RGB_OVERLAY_WORDS > 1) CHECK(KP_RGB_OVERLAY_COUNT == 65);
}
static void singleton_remote(uint32_t brightness, uint32_t power) {
    k_mutex_lock(&singleton_records_lock, K_FOREVER);
    for (unsigned i = 0; i < 2; i++) {
        CHECK(singleton_remote_brightness[i] == brightness);
        CHECK(singleton_remote_power[i] == power);
        for (size_t word = 0; word < KP_RGB_OVERLAY_WORDS; word++)
            CHECK(singleton_remote_mask[i][word] == singleton_expected_word(word));
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
static unsigned singleton_state_count(unsigned source) {
    unsigned count = 0;
    for (unsigned i = 0; i < singleton_records(); i++) {
        struct singleton_send sent = singleton_record(i);
        count += sent.source == source && !sent.ordinary && sent.command != RGB_CLOCK_CMD &&
                 sent.complete;
    }
    return count;
}
static void singleton_wait_source(unsigned source, unsigned minimum) {
    int64_t until = k_uptime_get() + 5000;
    while (singleton_state_count(source) < minimum) {
        CHECK(k_uptime_get() < until);
        CHECK(k_sem_take(&singleton_packet_done, K_MSEC(100)) == 0 || k_uptime_get() < until);
    }
}
static void singleton_wait_idle(void) {
    int64_t deadline = k_uptime_get() + 8000;
    int64_t quiet_since = k_uptime_get();
    unsigned previous = 0;
    while (k_uptime_get() - quiet_since < 1200) {
        unsigned count = 0;
        bool pending = false;
        for (unsigned i = 0; i < singleton_records(); i++) {
            struct singleton_send sent = singleton_record(i);
            if (sent.ordinary || sent.command == RGB_CLOCK_CMD) continue;
            count++;
            pending |= !sent.complete;
        }
        if (count != previous || pending) quiet_since = k_uptime_get();
        previous = count;
        CHECK(k_uptime_get() < deadline);
        k_sleep(K_MSEC(10));
    }
}
static void singleton_current_retry(void) {
    const char *phase = split_case + strlen("current-retry-");
    size_t word = !strcmp(phase, "later") ? 1 : !strcmp(phase, "final") ? KP_RGB_OVERLAY_WORDS - 1 : 0;
    CHECK(word < KP_RGB_OVERLAY_WORDS);
    singleton_fail_command = !strcmp(phase, "brightness") ? RGB_BRT_SET_CMD :
        !strcmp(phase, "power") ? RGB_ON_CMD : KP_RGB_OVERLAY_WORD_COMMAND(word);
    singleton_fail_once = true;
    singleton_mutate_on_failure = true;
    singleton_connect(3);
    singleton_wait_source(0, SINGLETON_PASS + 1);
    singleton_wait_source(1, SINGLETON_PASS);
    if (!strcmp(phase, "later") || !strcmp(phase, "final")) {
        unsigned prior_words = 0;
        bool failed_word = false;
        for (unsigned i = 0; i < singleton_records(); i++) {
            struct singleton_send sent = singleton_record(i);
            if (sent.source || sent.ordinary) continue;
            if (!sent.accepted) {
                CHECK(sent.command == KP_RGB_OVERLAY_WORD_COMMAND(word));
                CHECK(sent.argument == singleton_expected_word(word));
                failed_word = true;
                break;
            }
            if ((sent.command & 0xffffu) == RGB_OVL_STATE_CMD) {
                CHECK(sent.command == KP_RGB_OVERLAY_WORD_COMMAND(prior_words));
                CHECK(sent.argument == singleton_expected_word(prior_words));
                prior_words++;
            }
        }
        CHECK(failed_word && prior_words == word);
    }
    int64_t failed_at = -1;
    bool retry_after_deadline = false, retry_latest = false;
    uint32_t latest_command = !strcmp(phase, "brightness") ? RGB_BRT_SET_CMD :
        !strcmp(phase, "power") ? RGB_OFF_CMD : KP_RGB_OVERLAY_WORD_COMMAND(word);
    uint32_t latest_argument = !strcmp(phase, "brightness") ? 19 :
        !strcmp(phase, "power") ? 0 : (UINT32_C(0x12345678) ^ word);
    for (unsigned i = 0; i < singleton_records(); i++) {
        struct singleton_send sent = singleton_record(i);
        if (!sent.source && !sent.ordinary && sent.command != RGB_CLOCK_CMD) {
            if (!sent.accepted && sent.command == singleton_fail_command) failed_at = sent.end;
            else if (sent.accepted && failed_at >= 0) {
                if (!retry_latest) {
                    CHECK(sent.start >= failed_at + 1000);
                    CHECK(sent.command == latest_command && sent.argument == latest_argument);
                }
                retry_after_deadline |= sent.start >= failed_at + 1000;
                retry_latest |= sent.command == latest_command && sent.argument == latest_argument;
            }
        }
    }
    CHECK(failed_at >= 0 && retry_after_deadline && retry_latest);
    singleton_expected_mask = UINT32_C(0x12345678);
    singleton_wait_idle();
    singleton_remote(19, RGB_OFF_CMD);
    unsigned targets[2] = {singleton_state_count(0) + SINGLETON_PASS, singleton_state_count(1) + SINGLETON_PASS};
    singleton_connect(0); k_sleep(K_MSEC(150)); singleton_connect(3);
    singleton_wait_source(0, targets[0]);
    singleton_wait_source(1, targets[1]);
    k_sleep(K_MSEC(250));
    singleton_remote(19, RGB_OFF_CMD);
}
static void singleton_persistent_failure(void) {
    singleton_fail_command = RGB_BRT_SET_CMD;
    atomic_set(&singleton_fail_persistently, 1);
    singleton_connect(3);
    singleton_wait_source(1, SINGLETON_PASS);
    int64_t deadline = k_uptime_get() + 5000;
    for (;;) {
        unsigned failures = 0;
        int64_t previous_failure = -1;
        for (unsigned i = 0; i < singleton_records(); i++) {
            struct singleton_send sent = singleton_record(i);
            if (sent.source || sent.ordinary || sent.accepted || !sent.complete) continue;
            if (previous_failure >= 0) CHECK(sent.start >= previous_failure + 1000);
            previous_failure = sent.end;
            failures++;
        }
        k_mutex_lock(&singleton_records_lock, K_FOREVER);
        bool clocks_progressed = singleton_clocks[0] >= 2 && singleton_clocks[1] >= 2;
        bool peer_completed = singleton_remote_brightness[1] == 61 &&
            singleton_remote_power[1] == RGB_ON_CMD &&
            singleton_remote_mask[1][0] == singleton_expected_word(0);
        for (size_t word = 1; word < KP_RGB_OVERLAY_WORDS; word++)
            peer_completed &= singleton_remote_mask[1][word] == singleton_expected_word(word);
        k_mutex_unlock(&singleton_records_lock);
        if (clocks_progressed && failures >= 2) {
            CHECK(peer_completed);
            break;
        }
        CHECK(k_uptime_get() < deadline);
        k_sleep(K_MSEC(10));
    }
    atomic_set(&singleton_fail_persistently, 0);
    singleton_wait_idle();
    singleton_remote(61, RGB_ON_CMD);
}
static void singleton_race(void) {
    const char *phase = split_case + strlen("race-");
    singleton_expected_mask = UINT32_C(0x12345678);
    singleton_mutation_phase = !strcmp(phase, "brightness") ? 0 :
        !strcmp(phase, "power") ? 1 : 2;
    singleton_mutation_once = true;
    singleton_connect(3);
    singleton_wait_source(0, 2 * SINGLETON_PASS);
    singleton_wait_source(1, SINGLETON_PASS);
    singleton_wait_idle();
    singleton_remote(19, RGB_OFF_CMD);
}
static void singleton_pacing(void) {
    bool retry = strstr(split_case, "race") == NULL;
    if (retry) {
        singleton_fail_command = strstr(split_case, "overlay") ? KP_RGB_OVERLAY_WORD_COMMAND(0) :
            strstr(split_case, "power") ? RGB_ON_CMD : RGB_BRT_SET_CMD;
        singleton_fail_once = true;
    }
    uint32_t failed_command = singleton_fail_command;
    singleton_connect(3);
    singleton_wait_source(0, SINGLETON_PASS + (retry ? 1 : 0));
    singleton_wait_source(1, SINGLETON_PASS);
    k_sleep(K_MSEC(1200));
    CHECK(singleton_clocks[0] >= 1 && singleton_clocks[1] >= 1);
    int64_t previous_success = -1;
    for (unsigned i = 0; i < singleton_records(); i++) {
        struct singleton_send sent = singleton_record(i);
        if (sent.ordinary || sent.command == RGB_CLOCK_CMD || !sent.complete || !sent.accepted) continue;
        if (previous_success >= 0) CHECK(sent.start >= previous_success + 50);
        previous_success = sent.end;
    }
    if (retry) {
        bool retried = false;
        int64_t failed_at = -1;
        for (unsigned i = 0; i < singleton_records(); i++) {
            struct singleton_send sent = singleton_record(i);
            if (!sent.source && !sent.ordinary && !sent.accepted) failed_at = sent.end;
            if (!sent.source && !sent.ordinary && sent.accepted &&
                sent.command == failed_command && failed_at >= 0)
                retried |= sent.start >= failed_at + 1000;
        }
        CHECK(retried);
    }
    singleton_connect(0); k_sleep(K_MSEC(150));
    unsigned before = singleton_records();
    unsigned targets[2] = {singleton_state_count(0) + SINGLETON_PASS, singleton_state_count(1) + SINGLETON_PASS};
    singleton_global(RGB_BRT_SET_CMD, 77);
    atomic_set(&singleton_alternate, 1); singleton_connect(3);
    k_sleep(K_MSEC(20)); CHECK(singleton_records() == before);
    singleton_wait_source(0, targets[0]); singleton_wait_source(1, targets[1]);
    singleton_wait_idle(); singleton_remote(77, RGB_ON_CMD);
    targets[0] = singleton_state_count(0) + SINGLETON_PASS;
    targets[1] = singleton_state_count(1) + SINGLETON_PASS;
    atomic_set(&singleton_alternate, 0); singleton_connect(3);
    singleton_wait_source(0, targets[0]); singleton_wait_source(1, targets[1]);
    singleton_wait_idle(); singleton_remote(77, RGB_ON_CMD);
}
static void singleton_global_isolation(void) {
    singleton_connect(3);
    singleton_wait_source(0, SINGLETON_PASS); singleton_wait_source(1, SINGLETON_PASS);
    singleton_wait_idle();
    unsigned before = singleton_records();
    singleton_global(RGB_BRT_SET_CMD, 77);
    singleton_global(RGB_OFF_CMD, 0);
    k_sleep(K_MSEC(350));
    for (unsigned i = before; i < singleton_records(); i++) {
        struct singleton_send sent = singleton_record(i);
        CHECK(sent.ordinary || sent.command == RGB_CLOCK_CMD);
    }
    unsigned targets[2] = {singleton_state_count(0) + SINGLETON_PASS, singleton_state_count(1) + SINGLETON_PASS};
    singleton_connect(0); k_sleep(K_MSEC(150)); singleton_connect(3);
    singleton_wait_source(0, targets[0]); singleton_wait_source(1, targets[1]);
    k_sleep(K_MSEC(250)); singleton_remote(77, RGB_OFF_CMD);
}
static void singleton_negative_query(void) {
    singleton_connect(3); singleton_wait_stream(SINGLETON_PASS); k_sleep(K_MSEC(250));
    atomic_set(&singleton_query_error, 1); k_sleep(K_MSEC(150));
    unsigned before = singleton_records();
    k_sleep(K_MSEC(1200)); CHECK(singleton_records() == before);
    atomic_set(&singleton_query_error, 0);
    singleton_connect(0); k_sleep(K_MSEC(150)); singleton_connect(3);
    singleton_wait_stream(2 * SINGLETON_PASS);
    k_sleep(K_MSEC(250)); singleton_remote(61, RGB_ON_CMD);
}
static void singleton_unready(void) {
    CHECK(!device_is_ready(DEV(rgb_matrix)));
    kp_rgb_matrix_lock(); kp_rgb_global.brightness = 61; kp_rgb_matrix_unlock();
    singleton_connect(3); k_sleep(K_MSEC(1300)); CHECK(singleton_records() == 0);
    CHECK(device_init(DEV(rgb_matrix)) == 0 && device_is_ready(DEV(rgb_matrix)));
    singleton_masks(); singleton_global(RGB_BRT_SET_CMD, 61);
    singleton_wait_stream(SINGLETON_PASS); k_sleep(K_MSEC(350)); singleton_remote(61, RGB_ON_CMD);
}
static void singleton_check_order(void) {
    unsigned phase[2] = {0};
    int64_t success_at = -1, failed_at[2] = {-1, -1};
    for (unsigned i = 0; i < singleton_records(); i++) {
        struct singleton_send sent = singleton_record(i);
        if (sent.ordinary || sent.command == RGB_CLOCK_CMD) continue;
        CHECK(sent.complete);
        if (success_at >= 0) CHECK(sent.start >= success_at + 50);
        if (failed_at[sent.source] >= 0) CHECK(sent.start >= failed_at[sent.source] + 1000);
        if (!sent.accepted) { failed_at[sent.source] = sent.end; continue; }
        failed_at[sent.source] = -1;
        success_at = sent.end;
        unsigned *step = &phase[sent.source];
        if (*step == 0) CHECK(sent.command == RGB_BRT_SET_CMD);
        else if (*step == 1) CHECK(sent.command == RGB_ON_CMD || sent.command == RGB_OFF_CMD);
        else CHECK(sent.command == KP_RGB_OVERLAY_WORD_COMMAND(*step - 2));
        *step = (*step + 1) % SINGLETON_PASS;
    }
    CHECK(phase[0] == 0 && phase[1] == 0);
}
static void singleton_split(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c); k_sleep(K_MSEC(100));
    singleton_duration = 37;
    CHECK(zmk_rgb_matrix_on() == 0); CHECK(zmk_rgb_matrix_set_inhibited(true) == 0);
    if (!strcmp(split_case, "unready")) singleton_unready();
    else {
        singleton_masks(); singleton_global(RGB_BRT_SET_CMD, 61);
        if (!strncmp(split_case, "current-retry-", 14)) singleton_current_retry();
        else if (!strncmp(split_case, "race-", 5)) singleton_race();
        else if (!strcmp(split_case, "persistent-failure")) singleton_persistent_failure();
        else if (!strcmp(split_case, "global-isolation")) singleton_global_isolation();
        else if (!strcmp(split_case, "query-error")) singleton_negative_query();
        else singleton_pacing();
    }
    singleton_check_order();
    printk("PASS singleton split %s\n", split_case); exit(0);
}
K_THREAD_DEFINE(singleton_split_thread, 4096, singleton_split, NULL, NULL, NULL, 8, 0, 0);
