/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#include "rgb_persist_internal.h"

#if IS_ENABLED(CONFIG_SETTINGS)
static uint8_t singleton_bytes[32];
static size_t singleton_len;
static unsigned singleton_saves, singleton_reads, singleton_failures;
static bool singleton_fail;
static unsigned singleton_hook;
static int singleton_read_mode;
static int64_t singleton_request_time, singleton_return_time;
static struct {
    uint8_t bytes[6];
    int64_t time;
    bool failed;
} singleton_writes[8];
static K_SEM_DEFINE(singleton_request, 0, 1);
static K_SEM_DEFINE(singleton_done, 0, 1);

static void singleton_query(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    while (true) {
        k_sem_take(&singleton_request, K_FOREVER);
        bool on; CHECK(zmk_rgb_matrix_get_user_state(&on) == 0);
        singleton_request_time = k_uptime_get();
        if (singleton_hook == 2) kp_rgb_reset_state();
        else {
            kp_rgb_matrix_lock(); kp_rgb_global.brightness = 45; kp_rgb_matrix_unlock();
            CHECK(kp_rgb_save_state() == 0);
        }
        singleton_hook = 0;
        k_sem_give(&singleton_done);
    }
}
K_THREAD_DEFINE(singleton_storage_query, 2048, singleton_query, NULL, NULL, NULL, 5, 0, 0);

static ssize_t singleton_read(void *arg, void *out, size_t len) {
    ARG_UNUSED(arg); singleton_reads++;
    if (singleton_read_mode < 0) return -EIO;
    if (singleton_read_mode > 0 && len) len--;
    memcpy(out, singleton_bytes, MIN(len, singleton_len));
    return MIN(len, singleton_len);
}
static int singleton_load(struct settings_store *store, const struct settings_load_arg *arg) {
    ARG_UNUSED(store);
    return settings_call_set_handler(ROOT "snapshot", singleton_len, singleton_read, NULL, arg);
}
static int singleton_save(struct settings_store *store, const char *name, const char *bytes, size_t len) {
    ARG_UNUSED(store);
    CHECK(!strcmp(name, ROOT "snapshot"));
    CHECK(len == 6 && bytes != NULL);
    CHECK(singleton_saves < ARRAY_SIZE(singleton_writes));
    unsigned index = singleton_saves++;
    bool failed = singleton_fail;
    singleton_writes[index].time = k_uptime_get();
    singleton_writes[index].failed = failed;
    memcpy(singleton_writes[index].bytes, bytes, len);
    CHECK(bytes[0] == 1 && bytes[3] == 0 && bytes[4] == 0 && bytes[5] == 0);
    if (singleton_hook) {
        k_sem_give(&singleton_request);
        CHECK(k_sem_take(&singleton_done, K_MSEC(100)) == 0);
        /* Let the newer wake become due while the old backend call is still active. */
        k_sleep(K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE + 10));
        CHECK(!memcmp(bytes, singleton_writes[index].bytes, len));
        singleton_fail = false;
    }
    singleton_return_time = k_uptime_get();
    if (failed) { singleton_failures++; return -EIO; }
    memcpy(singleton_bytes, bytes, len); singleton_len = len;
    return 0;
}
static const struct settings_store_itf singleton_interface = {
    .csi_load = singleton_load, .csi_save = singleton_save};
static struct settings_store singleton_store = {.cs_itf = &singleton_interface};
int settings_backend_init(void) {
    const uint8_t initial[] = {1, 0, 61, 0, 0, 0};
    memcpy(singleton_bytes, initial, sizeof(initial)); singleton_len = sizeof(initial);
    settings_src_register(&singleton_store); settings_dst_register(&singleton_store);
    return 0;
}
static void singleton_stage(const uint8_t *bytes, size_t len) {
    CHECK(len <= sizeof(singleton_bytes));
    memcpy(singleton_bytes, bytes, len); singleton_len = len;
    CHECK(settings_call_set_handler(ROOT "snapshot", len, singleton_read, NULL, NULL) == 0);
}
static void singleton_legacy(void) {
    const char *paths[] = {
        ROOT "state", ROOT "global/state", ROOT "state/effects/any",
        ROOT "selectors/selector_a/state", ROOT "selectors/selector_a/effects/shared",
        ROOT "selectors/missing/state", ROOT "unknown", ROOT "snapshot/extra",
    };
    unsigned reads = singleton_reads, saves = singleton_saves;
    for (size_t i = 0; i < ARRAY_SIZE(paths); i++) {
        CHECK(settings_call_set_handler(paths[i], 4, singleton_read, NULL, NULL) == 0);
        CHECK(settings_call_set_handler(paths[i], SIZE_MAX, singleton_read, NULL, NULL) == 0);
    }
    CHECK(singleton_reads == reads && singleton_saves == saves);
}
static void singleton_validation(void) {
    const uint8_t valid[] = {1, 0, 66, 0, 0, 0};
    singleton_stage(valid, sizeof(valid));
    uint8_t bad[8];
    const uint8_t invalid[] = {2, 2, 101, 1, 1, 1};
    for (unsigned i = 0; i < ARRAY_SIZE(invalid); i++) {
        memcpy(bad, valid, sizeof(valid)); bad[i] = invalid[i];
        singleton_stage(bad, sizeof(valid));
    }
    for (size_t len = 0; len < sizeof(valid); len++) singleton_stage(valid, len);
    memcpy(bad, valid, sizeof(valid)); bad[6] = 0; bad[7] = 0;
    singleton_stage(bad, 7); singleton_stage(bad, 8);
    bad[4] = 0xff; bad[5] = 0xff; singleton_stage(bad, 8);
    singleton_read_mode = 1; singleton_stage(valid, sizeof(valid));
    singleton_read_mode = -1; singleton_stage(valid, sizeof(valid)); singleton_read_mode = 0;
    CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
    bool on; CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && !on);
    CHECK(kp_rgb_global.brightness == 66 && singleton_saves == 0);
    CHECK(settings_commit() == 0 && kp_rgb_global.brightness == 66);
    /* A previous firmware's selections do not invalidate the global-only consumer. */
    const uint8_t excess[] = {1, 1, 100, 0, 1, 0, 0x34, 0x12};
    singleton_stage(excess, sizeof(excess));
    CHECK(settings_commit() == 0);
    CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && on);
    CHECK(kp_rgb_global.brightness == 100);
    const uint8_t minimum[] = {1, 0, 0, 0, 0, 0};
    singleton_stage(minimum, sizeof(minimum));
    CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0 && kp_rgb_global.brightness == 0);
    singleton_stage(valid, sizeof(valid));
    CHECK(settings_commit() == 0);
}
static void singleton_codec(void) {
    struct kp_rgb_persist_state state = {.user_on = true, .brightness = 77, .selection_count = 0};
    uint8_t encoded[6];
    const uint8_t expected[] = {1, 1, 77, 0, 0, 0};
    CHECK(KP_RGB_PERSIST_SIZE(0) == sizeof(expected));
    CHECK(kp_rgb_persist_encode(&state, NULL, encoded, sizeof(encoded)) == 0);
    CHECK(!memcmp(encoded, expected, sizeof(expected)));
    CHECK(kp_rgb_persist_decode(expected, sizeof(expected), sizeof(expected), &state, NULL, 0) == 0);
    CHECK(state.user_on && state.brightness == 77 && state.selection_count == 0);
    uint16_t slots[] = {0x1234, 0xabcd}, decoded[2];
    const uint8_t ordered[] = {1, 1, 77, 0, 2, 0, 0x34, 0x12, 0xcd, 0xab};
    uint8_t wire[sizeof(ordered)]; state.selection_count = 2;
    CHECK(kp_rgb_persist_encode(&state, slots, wire, sizeof(wire)) == 0);
    CHECK(!memcmp(wire, ordered, sizeof(wire)));
    CHECK(kp_rgb_persist_decode(ordered, sizeof(ordered), sizeof(ordered), &state, decoded, 2) == 0);
    CHECK(decoded[0] == slots[0] && decoded[1] == slots[1]);
    const uint8_t high_count[] = {1, 1, 77, 0, 0, 1};
    CHECK(kp_rgb_persist_decode(high_count, sizeof(high_count), 6 + 2 * 256, &state, NULL, 0) == 0);
    CHECK(state.selection_count == 256);
}
#endif

static void singleton_settings(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c); k_sleep(K_MSEC(100));
#if IS_ENABLED(CONFIG_SETTINGS)
    bool on; CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && !on);
    CHECK(kp_rgb_global.brightness == 61 && singleton_saves == 0);
    singleton_codec();
    if (!strcmp(settings_case, "validation")) {
        singleton_validation();
    } else if (!strncmp(settings_case, "rejection-", 10)) {
        bool current_first = strstr(settings_case, "current-first") != NULL;
        const uint8_t valid[] = {1, 0, 88, 0, 0, 0};
        if (current_first) singleton_stage(valid, sizeof(valid));
        singleton_legacy();
        if (!current_first) singleton_stage(valid, sizeof(valid));
        CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
        CHECK(kp_rgb_global.brightness == 88 && singleton_saves == 0);
    } else if (!strcmp(settings_case, "debounce")) {
        kp_rgb_matrix_lock(); kp_rgb_global.brightness = 22; kp_rgb_matrix_unlock();
        CHECK(kp_rgb_save_state() == 0);
        k_sleep(K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE / 2));
        CHECK(singleton_saves == 0);
        CHECK(zmk_rgb_matrix_on() == 0);
        kp_rgb_matrix_lock(); kp_rgb_global.brightness = 77; kp_rgb_matrix_unlock();
        int64_t latest_request = k_uptime_get();
        CHECK(kp_rgb_save_state() == 0);
        k_sleep(K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE / 2));
        CHECK(singleton_saves == 0);
        k_sleep(K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE + 20));
        const uint8_t expected[] = {1, 1, 77, 0, 0, 0};
        CHECK(singleton_saves == 1);
        CHECK(singleton_writes[0].time >= latest_request + CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE);
        CHECK(!memcmp(singleton_writes[0].bytes, expected, sizeof(expected)));
        CHECK(!memcmp(singleton_bytes, expected, sizeof(expected)));
    } else if (!strcmp(settings_case, "staged-reset")) {
        const uint8_t staged[] = {1, 0, 17, 0, 0, 0};
        singleton_stage(staged, sizeof(staged));
        CHECK(kp_rgb_global.brightness == 61);
        int64_t reset_time = k_uptime_get();
        kp_rgb_reset_state();
        CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
        CHECK(settings_commit() == 0);
        CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && on);
        CHECK(kp_rgb_global.brightness == 100);
        k_sleep(K_MSEC(100));
        const uint8_t expected[] = {1, 1, 100, 0, 0, 0};
        CHECK(singleton_saves == 1 && singleton_len == sizeof(expected));
        CHECK(singleton_writes[0].time < reset_time + CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE);
        CHECK(!memcmp(singleton_bytes, expected, sizeof(expected)));
    } else if (!strcmp(settings_case, "failure")) {
        singleton_fail = true;
        CHECK(zmk_rgb_matrix_on() == 0);
        kp_rgb_matrix_lock(); kp_rgb_global.brightness = 22; kp_rgb_matrix_unlock();
        CHECK(kp_rgb_save_state() == 0);
        k_sleep(K_MSEC(10)); CHECK(singleton_saves == 0);
        k_sleep(K_MSEC(2200));
        CHECK(singleton_failures == 1 && singleton_saves == 1 && singleton_bytes[2] == 61);
        CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && on);
        CHECK(kp_rgb_global.brightness == 22);
        singleton_fail = false; k_sleep(K_MSEC(1100));
        CHECK(singleton_saves == 1 && singleton_bytes[2] == 61);
        CHECK(kp_rgb_save_state() == 0); k_sleep(K_MSEC(100));
        CHECK(singleton_saves == 2 && singleton_bytes[2] == 22);
    } else if (strstr(settings_case, "reset") || strstr(settings_case, "dirty")) {
        bool reset = strstr(settings_case, "reset") != NULL;
        bool failed = strstr(settings_case, "failed") != NULL;
        singleton_hook = reset ? 2 : 1; singleton_fail = failed;
        kp_rgb_matrix_lock(); kp_rgb_global.brightness = 12; kp_rgb_matrix_unlock();
        CHECK(kp_rgb_save_state() == 0); k_sleep(K_MSEC(200));
        CHECK(singleton_hook == 0 && singleton_saves == 2);
        const uint8_t old[] = {1, 0, 12, 0, 0, 0};
        CHECK(!memcmp(singleton_writes[0].bytes, old, sizeof(old)));
        CHECK(singleton_writes[0].failed == failed && !singleton_writes[1].failed);
        CHECK(singleton_failures == (failed ? 1 : 0));
        CHECK(singleton_writes[1].time >= singleton_request_time +
              (reset ? 0 : CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
        CHECK(singleton_writes[1].time < singleton_request_time + 200);
        CHECK(singleton_return_time < singleton_request_time + 200);
        CHECK(singleton_bytes[1] == (reset ? 1 : 0));
        CHECK(singleton_bytes[2] == (reset ? 100 : 45));
        k_sleep(K_MSEC(1100)); CHECK(singleton_saves == 2);
    } else if (!strcmp(settings_case, "repeated-commits")) {
        CHECK(zmk_rgb_matrix_on() == 0);
        kp_rgb_matrix_lock(); kp_rgb_global.brightness = 39; kp_rgb_matrix_unlock();
        for (unsigned i = 0; i < 4; i++) {
            CHECK((i % 2 ? settings_commit() : settings_commit_subtree("keypaw/rgb_matrix")) == 0);
            CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && on);
            CHECK(kp_rgb_global.brightness == 39 && singleton_saves == 0);
        }
    } else {
        singleton_legacy();
        CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
        CHECK(singleton_saves == 0 && kp_rgb_global.brightness == 61);
        kp_rgb_reset_state(); k_sleep(K_MSEC(100));
        CHECK(singleton_saves == 1 && singleton_bytes[2] == 100);
    }
    struct kp_rgb_persist_state durable;
    CHECK(kp_rgb_persist_decode(singleton_bytes, singleton_len, singleton_len, &durable, NULL, 0) == 0);
    CHECK((durable.user_on ? zmk_rgb_matrix_off() : zmk_rgb_matrix_on()) == 0);
    kp_rgb_matrix_lock(); kp_rgb_global.brightness = 19; kp_rgb_matrix_unlock();
    unsigned settled = singleton_saves; k_sleep(K_MSEC(1100)); CHECK(singleton_saves == settled);
    CHECK(settings_load_subtree("keypaw/rgb_matrix") == 0);
    CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && on == durable.user_on);
    CHECK(kp_rgb_global.brightness == durable.brightness);
#else
    CHECK(kp_rgb_control_commit() == 0 && kp_rgb_save_state() == 0);
    CHECK(kp_rgb_save_selector_state(NULL, true) == 0);
    CHECK(zmk_rgb_matrix_off() == 0); kp_rgb_reset_state();
    bool on; CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && on);
    CHECK(kp_rgb_global.brightness == 100);
    CHECK(zmk_rgb_matrix_off() == 0);
    CHECK(kp_rgb_control_commit() == 0 && kp_rgb_control_commit() == 0);
    CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && !on);
#endif
    printk("PASS singleton settings %s\n", settings_case); exit(0);
}
K_THREAD_DEFINE(singleton_settings_thread, 4096, singleton_settings, NULL, NULL, NULL, 5, 0, 0);
