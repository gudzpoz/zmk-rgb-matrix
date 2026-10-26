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
#include "kp_rgb_test_mutations.h"
#include <drivers/behavior.h>
#include <zmk/split/transport/central.h>
#include "rgb_matrix_internal.h"
#include "rgb_selector_internal.h"
#include "rgb_replication_internal.h"

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

#if DT_NODE_EXISTS(DT_NODELABEL(selector_a))
static uint16_t member_slot(const struct device *owner, const struct device *member) {
    for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++) {
        const struct kp_rgb_selector *selector = kp_rgb_selector_registry_at(i);
        if (selector->owner != owner) continue;
        for (size_t slot = 0; slot < selector->effect_count; slot++)
            if (selector->members[slot].effect == member) return slot;
    }
    CHECK(false);
    return UINT16_MAX;
}
#define A_SLOT(member) member_slot(DEV(selector_a), DEV(member))
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#if !DT_NODE_EXISTS(DT_NODELABEL(selector_a))
#include "kp_rgb_singleton_split_test.inc"
#else
static int split_provider_init(void) {
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == NULL);
    return 0;
}
SYS_INIT(split_provider_init, APPLICATION, 98);

struct attempt {
    uint8_t source;
    char endpoint[16];
    uint32_t command, argument;
    int64_t time, completion;
    bool accepted, live;
};
static K_MUTEX_DEFINE(records_lock);
static struct attempt attempts[1024];
static unsigned attempt_count, query_count, connected;
static unsigned order_floor;
static bool alternate_transport, no_transport;
static int query_error;
static uint32_t fail_kind;
static unsigned send_duration;
static int64_t reconnect_floor[2];
static zmk_split_transport_central_status_changed_cb_t status_cb;
static K_SEM_DEFINE(query_request, 0, 1);
static K_SEM_DEFINE(query_done, 0, 1);
static K_SEM_DEFINE(worker_accepted, 0, 1);
static K_SEM_DEFINE(worker_resume, 0, 1);
static bool worker_gate;
static bool worker_gate_fail;
static uint32_t worker_gate_command;
static const struct device *worker_gate_endpoint;
static unsigned worker_gate_source, worker_gate_skip;
static unsigned worker_gated_index;
static bool ordering_case(void);
static int ordering_live_send(uint8_t source, struct zmk_split_transport_central_command cmd);

static bool handler_returned = true;
static void handler_sentinel(struct k_work *work) {
    ARG_UNUSED(work);
    handler_returned = true;
}
static K_WORK_DEFINE(handler_sentinel_item, handler_sentinel);
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

static void bounded_wait(struct k_sem *sem) {
    CHECK(k_sem_take(sem, K_SECONDS(5)) == 0);
}
static void arm_worker(uint8_t source, uint32_t command, unsigned skip) {
    k_mutex_lock(&records_lock, K_FOREVER);
    CHECK(!worker_gate);
    worker_gate = true;
    worker_gate_source = source;
    worker_gate_command = command;
    worker_gate_skip = skip;
    uint32_t kind = command & KP_RGB_SELECTOR_COMMAND_MASK;
    worker_gate_endpoint = !skip && (kind == RGB_SELECTOR_COLOR_CMD ||
        kind == RGB_SELECTOR_PERIOD_CMD || kind == RGB_EFS_CMD) ? DEV(selector_a) : NULL;
    k_mutex_unlock(&records_lock);
}
static unsigned record_packet(uint8_t source, struct zmk_split_transport_central_command cmd,
                              bool accepted, bool live) {
    k_mutex_lock(&records_lock, K_FOREVER);
    CHECK(attempt_count < ARRAY_SIZE(attempts));
    unsigned index = attempt_count++;
    attempts[index] = (struct attempt){.source = source,
        .command = cmd.data.invoke_behavior.param1, .argument = cmd.data.invoke_behavior.param2,
        .time = k_uptime_get(), .accepted = accepted, .live = live};
    strcpy(attempts[index].endpoint, cmd.data.invoke_behavior.behavior_dev);
    k_mutex_unlock(&records_lock);
    return index;
}
static int send_command(uint8_t source, struct zmk_split_transport_central_command cmd) {
    CHECK(source < 2);
    CHECK(cmd.type == ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_INVOKE_BEHAVIOR);
    int live = ordering_live_send(source, cmd);
    if (live != -ENOMSG) return live;
    if (cmd.data.invoke_behavior.param1 & KP_RGB_INVOCATION_MARKER) {
        unsigned index = record_packet(source, cmd, cmd.data.invoke_behavior.state != 0, true);
        k_mutex_lock(&records_lock, K_FOREVER);
        attempts[index].completion = k_uptime_get();
        k_mutex_unlock(&records_lock);
        return 0;
    }
    CHECK(handler_returned);
    handler_returned = false;
    CHECK(k_work_submit_to_queue(kp_rgb_work_q(), &handler_sentinel_item) >= 0);
    CHECK(!(cmd.data.invoke_behavior.param1 & KP_RGB_INVOCATION_MARKER));
    CHECK(cmd.data.invoke_behavior.state == 1);
    CHECK(cmd.data.invoke_behavior.event_source == source);
    CHECK(cmd.data.invoke_behavior.position == 0);
    k_mutex_lock(&records_lock, K_FOREVER);
    CHECK(connected & BIT(source));
    CHECK(k_uptime_get() >= reconnect_floor[source]);
    uint32_t command = cmd.data.invoke_behavior.param1;
    bool accepted = !(source == 0 && fail_kind &&
        (command & KP_RGB_SELECTOR_COMMAND_MASK) == fail_kind);
    bool pause = worker_gate && source == worker_gate_source && command == worker_gate_command &&
        (!worker_gate_endpoint || !strcmp(worker_gate_endpoint->name, cmd.data.invoke_behavior.behavior_dev));
    if (pause && worker_gate_skip) { worker_gate_skip--; pause = false; }
    if (pause) {
        worker_gate = false;
        if (worker_gate_fail) { accepted = false; worker_gate_fail = false; }
    }
    k_mutex_unlock(&records_lock);
    /* Another thread must acquire the matrix mutex before transport can return. */
    k_sem_give(&query_request);
    bounded_wait(&query_done);
    unsigned index = record_packet(source, cmd, accepted, false);
    if (pause) {
        worker_gated_index = index;
        k_sem_give(&worker_accepted);
        bounded_wait(&worker_resume);
    }
    if (send_duration) k_sleep(K_MSEC(send_duration));
    k_mutex_lock(&records_lock, K_FOREVER);
    attempts[index].completion = k_uptime_get();
    k_mutex_unlock(&records_lock);
    return accepted ? 0 : -EIO;
}
static int sources(uint8_t *ids) {
    k_mutex_lock(&records_lock, K_FOREVER);
    if (query_error) { int error = query_error; k_mutex_unlock(&records_lock); return error; }
    int count = 0;
    for (unsigned i = 0; i < 2; i++) if (connected & BIT(i)) ids[count++] = i;
    k_mutex_unlock(&records_lock);
    return count;
}
static struct zmk_split_transport_status status(void) {
    k_mutex_lock(&records_lock, K_FOREVER);
    bool present = connected && !no_transport;
    struct zmk_split_transport_status value = {.available = !alternate_transport && !no_transport,
        .enabled = true, .connections = present ? ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_SOME_CONNECTED :
                                                 ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED};
    k_mutex_unlock(&records_lock);
    return value;
}
static int set_callback(zmk_split_transport_central_status_changed_cb_t cb) { status_cb = cb; return 0; }
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
    .get_status = alternate_status, .set_status_callback = set_callback, .set_enabled = set_enabled,
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
    CHECK(status_cb(&scoped_mock, status()) == (no_transport ? -ENODEV : 0));
    CHECK(status_cb(&scoped_alternate, alternate_status()) == 0);
}
static void global_apply(uint32_t command, uint32_t argument) {
    struct zmk_behavior_binding binding = {.behavior_dev = DEV(rgb_matrix)->name,
        .param1 = command, .param2 = argument};
    CHECK(behavior_keymap_binding_pressed(&binding, (struct zmk_behavior_binding_event){0}) == 0);
}
static unsigned state_count(void) {
    k_mutex_lock(&records_lock, K_FOREVER);
    unsigned count = 0;
    for (unsigned i = 0; i < attempt_count; i++)
        count += !attempts[i].live && attempts[i].command != RGB_CLOCK_CMD;
    k_mutex_unlock(&records_lock);
    return count;
}
static unsigned sweeps(unsigned source) {
    k_mutex_lock(&records_lock, K_FOREVER);
    unsigned count = 0;
    for (unsigned i = 0; i < attempt_count; i++)
        count += !attempts[i].live && attempts[i].accepted && attempts[i].source == source &&
                 attempts[i].command == RGB_BRT_SET_CMD;
    k_mutex_unlock(&records_lock);
    return count;
}
static uint32_t remote_value(unsigned source, const struct device *endpoint, uint32_t command) {
    k_mutex_lock(&records_lock, K_FOREVER);
    for (unsigned i = attempt_count; i > 0; i--) {
        const struct attempt *r = &attempts[i - 1];
        uint32_t logical = kp_rgb_invocation_command(r->command);
        bool power_command = command == RGB_ON_CMD || command == RGB_OFF_CMD;
        bool matches = power_command ? (logical == RGB_ON_CMD || logical == RGB_OFF_CMD) : logical == command;
        if (r->accepted && r->source == source && matches && !strcmp(r->endpoint, endpoint->name)) {
            uint32_t value = power_command ? logical : r->argument;
            k_mutex_unlock(&records_lock);
            return value;
        }
    }
    k_mutex_unlock(&records_lock);
    CHECK(false);
    return 0;
}
static void check_remote(unsigned source) {
    kp_rgb_matrix_lock();
    CHECK(remote_value(source, DEV(rgb_matrix), RGB_BRT_SET_CMD) == kp_rgb_global.brightness);
    uint32_t intent = kp_rgb_global.user_on ? RGB_ON_CMD : RGB_OFF_CMD;
    CHECK(remote_value(source, DEV(rgb_matrix), intent) == intent);
    for (size_t word = 0; word < KP_RGB_OVERLAY_WORDS; word++)
        CHECK(remote_value(source, DEV(rgb_matrix), KP_RGB_OVERLAY_WORD_COMMAND(word)) ==
              kp_rgb_overlay_get_word(word));
    for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++) {
        const struct kp_rgb_selector *selector = kp_rgb_selector_registry_at(i);
        for (size_t slot = 0; slot < selector->effect_count; slot++) {
            const struct device *effect = kp_rgb_selector_at(selector, slot);
            if (!effect) continue;
            struct kp_rgb_hsb color = kp_rgb_effect_data(effect)->color;
            CHECK(remote_value(source, selector->owner,
                KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, slot)) ==
                RGB_COLOR_HSB_VAL(color.h, color.s, color.b));
            CHECK(remote_value(source, selector->owner,
                KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, slot)) == kp_rgb_effect_period(effect));
        }
        CHECK(remote_value(source, selector->owner, RGB_EFS_CMD) == selector->effect_index);
    }
    kp_rgb_matrix_unlock();
}
static void settle(void) { k_sleep(K_MSEC(5000)); }
static void quiet(void) {
    unsigned before = state_count();
    k_sleep(K_MSEC(1200));
    CHECK(state_count() == before);
}
static void check_order(void) {
    for (unsigned source = 0; source < 2; source++) {
        unsigned phase = 0;
        size_t selector_index = 0, slot = 0, word = 0;
        k_mutex_lock(&records_lock, K_FOREVER);
        for (unsigned n = order_floor; n < attempt_count; n++) {
            const struct attempt *r = &attempts[n];
            if (r->source != source || r->live || !r->accepted || r->command == RGB_CLOCK_CMD) continue;
            if (r->command == RGB_BRT_SET_CMD) {
                CHECK(phase == 0);
                CHECK(!strcmp(r->endpoint, DEV(rgb_matrix)->name));
                phase = 1; selector_index = 0; slot = 0; word = 0; continue;
            }
            if (phase == 1) {
                CHECK(r->command == RGB_ON_CMD || r->command == RGB_OFF_CMD);
                phase = 2; continue;
            }
            if (phase == 2) {
                CHECK(!strcmp(r->endpoint, DEV(rgb_matrix)->name));
                CHECK(r->command == KP_RGB_OVERLAY_WORD_COMMAND(word));
                if (++word == KP_RGB_OVERLAY_WORDS) phase = 3;
                continue;
            }
            CHECK(selector_index < kp_rgb_selector_registry_count());
            const struct kp_rgb_selector *selector = kp_rgb_selector_registry_at(selector_index);
            CHECK(!strcmp(r->endpoint, selector->owner->name));
            while (slot < selector->effect_count && !kp_rgb_selector_at(selector, slot)) slot++;
            if (slot == selector->effect_count) {
                CHECK(r->command == RGB_EFS_CMD);
                selector_index++; slot = 0;
                phase = selector_index == kp_rgb_selector_registry_count() ? 0 : 3;
            } else {
                CHECK(r->command == KP_RGB_SELECTOR_MEMBER_COMMAND(
                    phase == 3 ? RGB_SELECTOR_COLOR_CMD : RGB_SELECTOR_PERIOD_CMD, slot));
                if (phase == 3) phase = 4;
                else { phase = 3; slot++; }
            }
        }
        CHECK(phase == 0);
        k_mutex_unlock(&records_lock);
    }
}
#include "kp_rgb_replication_test.inc"

#if IS_ENABLED(CONFIG_SETTINGS_CUSTOM)
#include <zephyr/settings/settings.h>
static atomic_t gate_saves;
static int gate_settings_load(struct settings_store *store, const struct settings_load_arg *arg) {
    ARG_UNUSED(store); ARG_UNUSED(arg); return 0;
}
static int gate_settings_save(struct settings_store *store, const char *name,
                              const char *value, size_t len) {
    ARG_UNUSED(store); ARG_UNUSED(value); ARG_UNUSED(len);
    if (!strcmp(name, "keypaw/rgb_matrix/snapshot")) atomic_inc(&gate_saves);
    return 0;
}
static const struct settings_store_itf gate_settings_itf = {
    .csi_load = gate_settings_load, .csi_save = gate_settings_save};
static struct settings_store gate_settings_store = {.cs_itf = &gate_settings_itf};
int settings_backend_init(void) {
    settings_src_register(&gate_settings_store);
    settings_dst_register(&gate_settings_store);
    return 0;
}

static void check_gate_dispatch(unsigned start, uint32_t first, uint32_t second) {
    CHECK(attempt_count == start + 4);
    for (unsigned i = 0; i < 4; i++) {
        const struct attempt *r = &attempts[start + i];
        CHECK(r->live && r->accepted && r->source == i % 2);
        CHECK(!strcmp(r->endpoint, DEV(rgb_matrix)->name));
        CHECK(r->command == (KP_RGB_OVERLAY_WORD_COMMAND(1 + i / 2) | KP_RGB_INVOCATION_MARKER));
        CHECK(r->argument == (i < 2 ? first : second));
    }
}

static uint32_t gate_refresh_original[2];
static K_SEM_DEFINE(gate_refresh_done, 0, 1);
static void gate_refresh_work(struct k_work *work) {
    ARG_UNUSED(work);
    kp_rgb_matrix_lock();
    struct kp_rgb_replication_status before = kp_rgb_replication_status_locked();
    CHECK(kp_rgb_overlay_refresh());
    CHECK(kp_rgb_replication_status_locked().generation == before.generation + 1);
    CHECK(kp_rgb_overlay_get_word(1) == gate_refresh_original[0] &&
          kp_rgb_overlay_get_word(2) == gate_refresh_original[1]);
    CHECK(!kp_rgb_overlay_refresh());
    CHECK(kp_rgb_replication_status_locked().generation == before.generation + 1);
    kp_rgb_matrix_unlock();
    unsigned start = attempt_count;
    kp_rgb_overlay_dispatch();
    check_gate_dispatch(start, gate_refresh_original[0], gate_refresh_original[1]);
    k_sem_give(&gate_refresh_done);
}
static K_WORK_DEFINE(gate_refresh_item, gate_refresh_work);

static void multiword_control(void) {
    CHECK(KP_RGB_OVERLAY_WORDS >= 3);
    connect(3); settle(); quiet();
    unsigned passes[2] = {sweeps(0), sweeps(1)};
    atomic_val_t saves = atomic_get(&gate_saves);
    uint64_t epoch = ordering_status().repair_epoch;
    arm_worker(0, RGB_CLOCK_CMD, 0); bounded_wait(&worker_accepted);
    uint32_t original[2] = {kp_rgb_overlay_get_word(1), kp_rgb_overlay_get_word(2)};
    for (size_t word = 1; word <= 2; word++) {
        uint32_t untouched = kp_rgb_overlay_get_word(0);
        uint32_t other = kp_rgb_overlay_get_word(word == 1 ? 2 : 1);
        for (unsigned pass = 0; pass < 2; pass++) {
            struct zmk_behavior_binding binding = {.behavior_dev = DEV(rgb_matrix)->name,
                .param1 = KP_RGB_OVERLAY_WORD_COMMAND(word),
                .param2 = original[word - 1] ^ UINT32_C(0x80000001)};
            struct kp_rgb_replication_status before = ordering_status();
            CHECK(behavior_keymap_binding_convert_central_state_dependent_params(
                &binding, (struct zmk_behavior_binding_event){0}) == 0);
            CHECK(binding.param1 == (KP_RGB_OVERLAY_WORD_COMMAND(word) | KP_RGB_INVOCATION_MARKER));
            CHECK(KP_RGB_OVERLAY_COMMAND_WORD(binding.param1) == word);
            CHECK(ordering_status().outstanding == before.outstanding + 1);
            CHECK(ordering_status().generation == before.generation);
            CHECK(ordering_callback(&binding, true) == 0);
            CHECK(binding.param1 == KP_RGB_OVERLAY_WORD_COMMAND(word));
            CHECK(kp_rgb_overlay_get_word(word) == binding.param2);
            CHECK(kp_rgb_overlay_get_word(0) == untouched);
            CHECK(kp_rgb_overlay_get_word(word == 1 ? 2 : 1) == other);
            CHECK(ordering_status().outstanding == before.outstanding);
            CHECK(ordering_status().generation == before.generation + (pass ? 1 : 2));
        }
        CHECK(kp_rgb_overlay_set_word(word, original[word - 1]));
    }
    const size_t invalid[] = {KP_RGB_OVERLAY_WORDS, UINT16_MAX};
    for (size_t i = 0; i < ARRAY_SIZE(invalid); i++) {
        struct zmk_behavior_binding binding = {.behavior_dev = DEV(rgb_matrix)->name,
            .param1 = KP_RGB_OVERLAY_WORD_COMMAND(invalid[i]), .param2 = UINT32_MAX};
        struct kp_rgb_replication_status before = ordering_status();
        CHECK(behavior_keymap_binding_convert_central_state_dependent_params(
            &binding, (struct zmk_behavior_binding_event){0}) == -EINVAL);
        CHECK(binding.param1 == KP_RGB_OVERLAY_WORD_COMMAND(invalid[i]));
        binding.param1 |= KP_RGB_INVOCATION_MARKER;
        CHECK(behavior_keymap_binding_convert_central_state_dependent_params(
            &binding, (struct zmk_behavior_binding_event){0}) == -ENOTSUP);
        CHECK(ordering_status().outstanding == before.outstanding);
        CHECK(ordering_status().generation == before.generation);
        CHECK(ordering_status().repair_epoch == before.repair_epoch);
    }
    CHECK(kp_rgb_overlay_set_word(1, original[0] ^ UINT32_C(0x80000001)));
    CHECK(kp_rgb_overlay_set_word(2, original[1] ^ UINT32_C(0x80000001)));
    unsigned start = attempt_count;
    kp_rgb_overlay_dispatch();
    check_gate_dispatch(start, original[0] ^ UINT32_C(0x80000001), original[1] ^ UINT32_C(0x80000001));
    start = attempt_count;
    kp_rgb_overlay_dispatch(); CHECK(attempt_count == start);
    gate_refresh_original[0] = original[0];
    gate_refresh_original[1] = original[1];
    k_sem_give(&worker_resume);
    CHECK(k_work_submit_to_queue(kp_rgb_work_q(), &gate_refresh_item) >= 0);
    bounded_wait(&gate_refresh_done);
    CHECK(ordering_status().repair_epoch == epoch);
    quiet();
    CHECK(sweeps(0) == passes[0] && sweeps(1) == passes[1]);
    CHECK(atomic_get(&gate_saves) == saves);

    for (size_t word = 1; word <= 2; word++) {
        struct zmk_behavior_binding binding = {.behavior_dev = DEV(rgb_matrix)->name,
            .param1 = KP_RGB_OVERLAY_WORD_COMMAND(word), .param2 = 0};
        struct kp_rgb_replication_status before = ordering_status();
        CHECK(behavior_keymap_binding_convert_central_state_dependent_params(
            &binding, (struct zmk_behavior_binding_event){0}) == 0);
        CHECK(ordering_status().outstanding == 1);
        binding.param1 = KP_RGB_OVERLAY_WORD_COMMAND(KP_RGB_OVERLAY_WORDS) | KP_RGB_INVOCATION_MARKER;
        CHECK(ordering_callback(&binding, true) == -EINVAL);
        CHECK(!(binding.param1 & KP_RGB_INVOCATION_MARKER));
        CHECK(ordering_status().outstanding == 0);
        CHECK(ordering_status().generation == before.generation + 1);
        CHECK(ordering_status().repair_epoch == before.repair_epoch + 1);
        settle(); check_remote(0); check_remote(1); check_order(); quiet();
        CHECK(sweeps(0) == passes[0] + word && sweeps(1) == passes[1] + word);
    }
    CHECK(atomic_get(&gate_saves) == saves);
    global_apply(RGB_BRT_SET_CMD, 37);
    k_sleep(K_MSEC(150));
    CHECK(atomic_get(&gate_saves) == saves + 1);
}
#endif

static void multiword_case(void) {
    CHECK(KP_RGB_OVERLAY_COUNT >= 65 && KP_RGB_OVERLAY_WORDS >= 3);
    bool final = !strcmp(split_case, "multiword-final");
    size_t target = final ? KP_RGB_OVERLAY_WORDS - 1 : 1;
    kp_rgb_matrix_lock();
    for (size_t word = 0; word < KP_RGB_OVERLAY_WORDS; word++) {
        kp_rgb_overlay_set_word(word, UINT32_C(0x80000001));
        CHECK(kp_rgb_overlay_get_word(word) == UINT32_C(0x80000001));
    }
    kp_rgb_matrix_unlock();
    worker_gate_fail = !final;
    arm_worker(0, KP_RGB_OVERLAY_WORD_COMMAND(target), 0);
    connect(3); bounded_wait(&worker_accepted);
    CHECK(remote_value(0, DEV(rgb_matrix), KP_RGB_OVERLAY_WORD_COMMAND(0)) == UINT32_C(0x80000001));
    struct kp_rgb_replication_status before = ordering_status();
    kp_rgb_matrix_lock();
    CHECK(kp_rgb_overlay_set_word(0, UINT32_C(0x12345678)));
    CHECK(kp_rgb_overlay_set_word(target, UINT32_C(0xfedcba98)));
    CHECK(kp_rgb_overlay_get_word(target) == UINT32_C(0xfedcba98));
    kp_rgb_matrix_unlock();
    CHECK(ordering_status().generation > before.generation);
    CHECK(!ordering_complete(before.generation));
    k_sem_give(&worker_resume);
    settle(); check_remote(0); check_remote(1); check_order(); quiet();
    CHECK(sweeps(0) == 2);
    int64_t success_at = -1;
    bool retried = final;
    for (unsigned i = 0; i < attempt_count; i++) {
        const struct attempt *r = &attempts[i];
        if (r->live || r->command == RGB_CLOCK_CMD) continue;
        if (success_at >= 0) CHECK(r->time >= success_at + 50);
        if (r->accepted) success_at = r->completion;
        if (!final && i > worker_gated_index && r->source == 0 && !retried) {
            CHECK(r->command == KP_RGB_OVERLAY_WORD_COMMAND(target));
            CHECK(r->argument == UINT32_C(0xfedcba98));
            CHECK(r->time >= attempts[worker_gated_index].completion + 1000);
            retried = true;
        }
    }
    CHECK(retried);
}

static void retry_case(void) {
    fail_kind = strstr(split_case, "period") ? RGB_SELECTOR_PERIOD_CMD :
                strstr(split_case, "selection") ? RGB_EFS_CMD : RGB_SELECTOR_COLOR_CMD;
    send_duration = strstr(split_case, "pacing") ? 37 : 0;
    arm_worker(0, KP_RGB_SELECTOR_MEMBER_COMMAND(fail_kind,
        fail_kind == RGB_EFS_CMD ? 0 : A_SLOT(probe_a)), 0);
    connect(3);
    bounded_wait(&worker_accepted);
    uint32_t latest = RGB_COLOR_HSB_VAL(217, 43, 0);
    CHECK(kp_test_set_color(DEV(probe_a), &(struct kp_rgb_hsb){.h=217, .s=43, .b=0}) == 0);
    CHECK(kp_test_set_period(DEV(probe_a), 2345) == 0);
    CHECK(kp_test_select(DEV(selector_a), DEV(probe_c)) == 0);
    k_sem_give(&worker_resume);
    k_sleep(K_MSEC(4600));
    check_remote(1);
    CHECK(sweeps(1) <= 2);
    k_mutex_lock(&records_lock, K_FOREVER);
    unsigned failures = 0, clocks[2] = {0};
    int64_t failed_at = -1, success_at = -1;
    for (unsigned i = 0; i < attempt_count; i++) {
        const struct attempt *r = &attempts[i];
        if (r->live) continue;
        if (r->command == RGB_CLOCK_CMD) { clocks[r->source]++; continue; }
        CHECK(r->source != 0 || strcmp(r->endpoint, DEV(selector_b)->name));
        if (success_at >= 0) CHECK(r->time >= success_at + 50);
        if (r->accepted) success_at = r->completion;
        if (!r->accepted) {
            CHECK(r->source == 0);
            if (failed_at >= 0) {
                CHECK(r->time >= failed_at + 1000 && r->time < failed_at + 1250);
                CHECK(r->argument == (fail_kind == RGB_SELECTOR_COLOR_CMD ? latest :
                    fail_kind == RGB_SELECTOR_PERIOD_CMD ? 2345 : A_SLOT(probe_c)));
            }
            failed_at = r->completion; failures++;
        }
    }
    CHECK(failures >= 5 && failures <= 6);
    CHECK(clocks[0] >= 2 && clocks[1] >= 2);
    fail_kind = 0;
    k_mutex_unlock(&records_lock);
    settle(); check_remote(0); check_remote(1); check_order(); quiet();
}
static void clock_failure(void) {
    fail_kind = RGB_CLOCK_CMD;
    connect(3); settle(); check_remote(0); check_remote(1); check_order();
    k_mutex_lock(&records_lock, K_FOREVER);
    unsigned clocks[2] = {0};
    int64_t previous[2] = {-1, -1};
    for (unsigned i = 0; i < attempt_count; i++) {
        const struct attempt *r = &attempts[i];
        if (r->command != RGB_CLOCK_CMD) continue;
        CHECK(r->accepted == (r->source == 1));
        if (previous[r->source] >= 0)
            CHECK(r->time >= previous[r->source] + CONFIG_KEYPAW_RGB_SPLIT_SYNC_CLOCK_MS &&
                  r->time < previous[r->source] + CONFIG_KEYPAW_RGB_SPLIT_SYNC_CLOCK_MS + 250);
        previous[r->source] = r->completion;
        clocks[r->source]++;
    }
    CHECK(clocks[0] >= 4 && clocks[1] >= 4);
    fail_kind = 0;
    k_mutex_unlock(&records_lock);
    quiet();
}
static void transport_case(void) {
    connect(3); settle(); check_remote(0); check_remote(1); check_order();
    if (!strcmp(split_case, "local-silence")) {
        ordering_publications(); quiet();
    } else if (!strcmp(split_case, "transport-change")) {
        connect(0); k_sleep(K_MSEC(100));
        unsigned before = state_count();
        CHECK(kp_test_set_period(DEV(probe_c), 2345) == 0);
        k_mutex_lock(&records_lock, K_FOREVER); alternate_transport = true; k_mutex_unlock(&records_lock);
        connect(3); k_sleep(K_MSEC(20)); CHECK(state_count() == before);
        settle(); check_remote(0); check_remote(1);
    } else if (!strcmp(split_case, "negative-query") || !strcmp(split_case, "no-transport")) {
        k_mutex_lock(&records_lock, K_FOREVER);
        if (!strcmp(split_case, "negative-query")) query_error = -EIO;
        else no_transport = true;
        k_mutex_unlock(&records_lock);
        if (no_transport) connect(3);
        k_sleep(K_MSEC(100));
        unsigned before = attempt_count;
        k_sleep(K_MSEC(1200)); CHECK(attempt_count == before);
        k_mutex_lock(&records_lock, K_FOREVER); no_transport = false; query_error = 0; k_mutex_unlock(&records_lock);
        connect(3); settle(); check_remote(0); check_remote(1);
        CHECK(attempt_count > before);
    } else if (!strcmp(split_case, "clock")) {
        unsigned counts[2] = {0};
        for (unsigned i = 0; i < attempt_count; i++) if (attempts[i].command == RGB_CLOCK_CMD) {
            CHECK((uint32_t)attempts[i].time - attempts[i].argument <= 1);
            counts[attempts[i].source]++;
        }
        CHECK(counts[0] >= 2 && counts[1] >= 2);
        connect(0); k_sleep(K_MSEC(100));
        unsigned before = attempt_count;
        k_sleep(K_MSEC(1100)); CHECK(attempt_count == before);
    }
    quiet();
}
static void readiness(void) {
    CHECK(!device_is_ready(DEV(rgb_matrix)));
    connect(3); k_sleep(K_MSEC(1300)); CHECK(attempt_count == 0);
    CHECK(device_init(DEV(rgb_matrix)) == 0);
    settle(); check_remote(0); check_remote(1); check_order(); quiet();
}
static void test_thread(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    for (unsigned i = 0; i < 100 && zmk_rgb_selector_get_selected(DEV(selector_b)) == NULL; i++)
        k_sleep(K_MSEC(5));
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
    k_sleep(K_MSEC(100));
    CHECK(zmk_rgb_matrix_off() == 0);
    CHECK(zmk_rgb_matrix_set_inhibited(true) == 0);
    if (ordering_case()) ordering_run();
#if IS_ENABLED(CONFIG_SETTINGS_CUSTOM)
    else if (!strcmp(split_case, "multiword-control")) multiword_control();
#endif
    else if (!strncmp(split_case, "multiword-", 10)) multiword_case();
    else if (!strcmp(split_case, "late-global")) readiness();
    else if (!strcmp(split_case, "clock-failure")) clock_failure();
    else if (!strncmp(split_case, "retry-", 6) || !strncmp(split_case, "pacing-", 7)) retry_case();
    else transport_case();
    printk("PASS split %s\n", split_case); exit(0);
}
K_THREAD_DEFINE(split_tests, 4096, test_thread, NULL, NULL, NULL, 8, 0, 0);
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
static bool irq_checked;
static void receiving_irq(const void *unused) {
    ARG_UNUSED(unused);
    CHECK(k_is_in_isr());
    zmk_rgb_matrix_flush();
    kp_rgb_condition_invalidate();
    irq_checked = true;
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
        CHECK(receive(DEV(selector_a), KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, A_SLOT(probe_c)),
                      RGB_COLOR_HSB_VAL(220, 70, levels[i])) == 0);
        CHECK(receive(DEV(selector_a), KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, A_SLOT(probe_c)), 2300) == 0);
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
    CHECK(receive(DEV(selector_a), RGB_EFS_CMD, A_SLOT(probe_c)) == 0);
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_c));
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
    CHECK(receive(DEV(selector_a), KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 2), 0) == -EINVAL);
    const uint32_t colors[] = {RGB_COLOR_HSB_VAL(361, 1, 100), RGB_COLOR_HSB_VAL(1, 101, 100),
                              RGB_COLOR_HSB_VAL(1, 1, 101), RGB_COLOR_HSB_VAL(1, 1, 255), UINT32_MAX};
    for (size_t i = 0; i < ARRAY_SIZE(colors); i++)
        CHECK(receive(DEV(selector_a), KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, A_SLOT(probe_c)), colors[i]) == -EINVAL);
    CHECK(kp_rgb_effect_data(DEV(probe_c))->color.h == 220 &&
          kp_rgb_effect_data(DEV(probe_c))->color.s == 70 && kp_rgb_effect_data(DEV(probe_c))->color.b == 0);
    const uint32_t periods[] = {0, CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS - 1,
                               CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS + 1, UINT32_MAX};
    for (size_t i = 0; i < ARRAY_SIZE(periods); i++)
        CHECK(receive(DEV(selector_a), KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, A_SLOT(probe_c)), periods[i]) == -EINVAL);
    const uint32_t slots[] = {2, UINT16_MAX, 0x10000, 0x10002, UINT32_MAX};
    for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
        CHECK(receive(DEV(selector_a), RGB_EFS_CMD, slots[i]) < 0);
        if (slots[i] <= UINT16_MAX) {
            CHECK(receive(DEV(selector_a), KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, slots[i]), 0) < 0);
            CHECK(receive(DEV(selector_a), KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, slots[i]), 2300) < 0);
        }
    }
    CHECK(receive(DEV(selector_a), 0x10a, 0) == -ENOTSUP);
    CHECK(receive(DEV(selector_a), 0xffff, 0) == -ENOTSUP);
    CHECK(kp_rgb_selector_binding_apply(NULL, RGB_EFS_CMD, 1) == -ENODEV);
    struct zmk_behavior_binding no_control = {.behavior_dev = NULL, .param1 = RGB_EFS_CMD, .param2 = 1};
    CHECK(behavior_keymap_binding_pressed(&no_control, (struct zmk_behavior_binding_event){0}) == -EINVAL);
    irq_offload(receiving_irq, NULL);
    CHECK(irq_checked);
    CHECK(receive(DEV(selector_a), KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, A_SLOT(probe_c)), RGB_COLOR_HSB_VAL(220, 70, 0)) == 0);
    CHECK(receive(DEV(selector_a), KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, A_SLOT(probe_c)), 2300) == 0);
    CHECK(receive(DEV(selector_a), RGB_EFS_CMD, A_SLOT(probe_c)) == 0);
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
    CHECK(receive(DEV(selector_a), RGB_RESET_CMD, 0) == 0);
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
    CHECK(kp_rgb_effect_data(DEV(probe_c))->color.h == 240);
    CHECK(receive(DEV(rgb_matrix), RGB_RESET_CMD, 0) == 0);
    CHECK(kp_rgb_global.brightness == 100);
    kp_rgb_matrix_lock();
    struct kp_rgb_replication_status replication_before = kp_rgb_replication_status_locked();
    kp_rgb_matrix_unlock();
    CHECK(replication_before.outstanding == 0);
    CHECK(receive(DEV(selector_a), KP_RGB_INVOCATION_MARKER |
        KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, A_SLOT(probe_c)),
        RGB_COLOR_HSB_VAL(123, 45, 67)) == 0);
    CHECK(receive(DEV(selector_a), KP_RGB_INVOCATION_MARKER |
        KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, A_SLOT(probe_c)), 2345) == 0);
    CHECK(receive(DEV(selector_a), KP_RGB_INVOCATION_MARKER | RGB_EFS_CMD, A_SLOT(probe_c)) == 0);
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_c));
    CHECK(kp_rgb_effect_data(DEV(probe_c))->color.h == 123);
    CHECK(kp_rgb_effect_period(DEV(probe_c)) == 2345);
    CHECK(receive(DEV(rgb_matrix), KP_RGB_INVOCATION_MARKER | RGB_BRT_SET_CMD, 43) == 0);
    CHECK(kp_rgb_global.brightness == 43);
    CHECK(receive(DEV(rgb_matrix), KP_RGB_INVOCATION_MARKER | RGB_ON_CMD, 0) == 0);
    CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && on);
    uint32_t high_slot = KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, UINT16_MAX);
    CHECK((kp_rgb_invocation_command(high_slot | KP_RGB_INVOCATION_MARKER) >> 16) == UINT16_MAX);
    CHECK(receive(DEV(selector_a), high_slot | KP_RGB_INVOCATION_MARKER, 0) == -EINVAL);
    CHECK(receive(DEV(selector_a), 0x10a | KP_RGB_INVOCATION_MARKER, 0) == -ENOTSUP);
    CHECK(receive(DEV(rgb_matrix), 0x10a | KP_RGB_INVOCATION_MARKER, 0) == -ENOTSUP);
    for (size_t word = 0; word < KP_RGB_OVERLAY_WORDS; word++) {
        uint32_t command = KP_RGB_OVERLAY_WORD_COMMAND(word);
        CHECK((command & 0xffffu) == RGB_OVL_STATE_CMD);
        CHECK(!(command & KP_RGB_INVOCATION_MARKER));
        CHECK(KP_RGB_OVERLAY_COMMAND_WORD(command | KP_RGB_INVOCATION_MARKER) == word);
        CHECK(kp_rgb_invocation_command(command | KP_RGB_INVOCATION_MARKER) == command);
        const uint32_t payloads[] = {0, UINT32_C(0x80000001), UINT32_MAX};
        for (size_t p = 0; p < ARRAY_SIZE(payloads); p++) {
            CHECK(receive(DEV(rgb_matrix), command | (p == 1 ? KP_RGB_INVOCATION_MARKER : 0),
                          payloads[p]) == 0);
            kp_rgb_matrix_lock();
            CHECK(kp_rgb_overlay_get_word(word) == payloads[p]);
            for (size_t previous = 0; previous < word; previous++)
                CHECK(kp_rgb_overlay_get_word(previous) == UINT32_MAX);
            kp_rgb_matrix_unlock();
        }
    }
    const size_t invalid_words[] = {KP_RGB_OVERLAY_WORDS, UINT16_MAX};
    for (size_t i = 0; i < ARRAY_SIZE(invalid_words); i++) {
        for (unsigned marked = 0; marked < 2; marked++)
            CHECK(receive(DEV(rgb_matrix), KP_RGB_OVERLAY_WORD_COMMAND(invalid_words[i]) |
                          (marked ? KP_RGB_INVOCATION_MARKER : 0), 0) == -EINVAL);
    }
    kp_rgb_matrix_lock();
    for (size_t word = 0; word < KP_RGB_OVERLAY_WORDS; word++)
        CHECK(kp_rgb_overlay_get_word(word) == UINT32_MAX);
    kp_rgb_matrix_unlock();
    kp_rgb_matrix_lock();
    struct kp_rgb_replication_status replication_after = kp_rgb_replication_status_locked();
    kp_rgb_matrix_unlock();
    CHECK(replication_after.outstanding == 0);
    CHECK(replication_after.generation == replication_before.generation);
    k_sleep(K_MSEC(200)); CHECK(peripheral_reports == 0);
    printk("PASS split receiving\n"); exit(0);
}
K_THREAD_DEFINE(split_receiving, 4096, receiving, NULL, NULL, NULL, 8, 0, 0);
#endif
