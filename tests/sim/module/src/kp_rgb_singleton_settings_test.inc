/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#if IS_ENABLED(CONFIG_SETTINGS)
static uint8_t singleton_bytes[36];
static size_t singleton_len;
static unsigned singleton_saves, singleton_reads, singleton_failures;
static bool singleton_fail;
static unsigned singleton_hook;
static int singleton_read_mode;
static uint8_t first_saved_brightness;
static int64_t singleton_write_time;
static uint8_t singleton_write_bytes[sizeof(struct kp_rgb_persist_global_record)];
static int64_t singleton_last_failure;
static unsigned singleton_reset_revision, singleton_last_failure_revision;
static K_SEM_DEFINE(singleton_request, 0, 1);
static K_SEM_DEFINE(singleton_done, 0, 1);
static void singleton_query(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    while (true) {
        k_sem_take(&singleton_request, K_FOREVER);
        bool on; CHECK(zmk_rgb_matrix_get_user_state(&on) == 0);
        if (singleton_hook == 2) { kp_rgb_reset_state(); singleton_reset_revision++; }
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
    if (singleton_read_mode > 0) len--;
    memcpy(out, singleton_bytes, MIN(len, singleton_len));
    return MIN(len, singleton_len);
}
static int singleton_load(struct settings_store *store, const struct settings_load_arg *arg) {
    ARG_UNUSED(store);
    return settings_call_set_handler(ROOT "state", singleton_len, singleton_read, NULL, arg);
}
static int singleton_save(struct settings_store *store, const char *name, const char *bytes, size_t len) {
    ARG_UNUSED(store);
    unsigned revision = singleton_reset_revision;
    CHECK(!strcmp(name, ROOT "state")); CHECK(len == 4);
    if (singleton_saves++ == 0) first_saved_brightness = bytes[2];
    singleton_write_time = k_uptime_get();
    memcpy(singleton_write_bytes, bytes, len);
    if (singleton_hook) {
        k_sem_give(&singleton_request); CHECK(k_sem_take(&singleton_done, K_MSEC(100)) == 0);
    }
    if (singleton_fail) {
        int64_t now = k_uptime_get();
        CHECK(!singleton_failures || revision != singleton_last_failure_revision ||
              now - singleton_last_failure >= 1000);
        singleton_last_failure_revision = revision;
        singleton_last_failure = now; singleton_failures++; return -EIO;
    }
    memcpy(singleton_bytes, bytes, len); singleton_len = len; return 0;
}
static const struct settings_store_itf singleton_interface = {
    .csi_load = singleton_load, .csi_save = singleton_save};
static struct settings_store singleton_store = {.cs_itf = &singleton_interface};
int settings_backend_init(void) {
    struct kp_rgb_persist_global_state value = {.user_on = false, .brightness = 61};
    struct kp_rgb_persist_global_record bytes;
    CHECK(kp_rgb_persist_encode_global(&value, &bytes) == 0);
    memcpy(singleton_bytes, &bytes, sizeof(bytes)); singleton_len = sizeof(bytes);
    settings_src_register(&singleton_store); settings_dst_register(&singleton_store); return 0;
}
static void singleton_stage(const uint8_t *bytes, size_t len) {
    memcpy(singleton_bytes, bytes, len); singleton_len = len;
    CHECK(settings_call_set_handler(ROOT "state", len, singleton_read, NULL, NULL) == 0);
}
static void singleton_validation(void) {
    const uint8_t valid[4] = {3, 0, 66, 0};
    singleton_stage(valid, 4);
    uint8_t bad[4];
    const uint8_t invalid[] = {2, 2, 101, 1};
    for (unsigned i = 0; i < ARRAY_SIZE(invalid); i++) {
        memcpy(bad, valid, 4); bad[i] = invalid[i]; singleton_stage(bad, 4);
    }
    singleton_stage(valid, 3);
    singleton_read_mode = 1; singleton_stage(valid, 4);
    singleton_read_mode = -1; singleton_stage(valid, 4); singleton_read_mode = 0;
    CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
    bool on; CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && !on);
    CHECK(kp_rgb_global.brightness == 66 && singleton_saves == 0);
    CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0 && kp_rgb_global.brightness == 66);
    singleton_stage(valid, 4);
}
#endif
static void singleton_settings(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c); k_sleep(K_MSEC(100));
#if IS_ENABLED(CONFIG_SETTINGS)
    bool on; CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && !on);
    CHECK(kp_rgb_global.brightness == 61 && singleton_saves == 0);
    if (!strcmp(settings_case, "validation")) {
        singleton_validation();
    } else if (!strncmp(settings_case, "rejection-", 10)) {
        bool current_first = strstr(settings_case, "current-first") != NULL;
        const uint8_t valid[4] = {3, 0, 88, 0};
        uint8_t legacy[36] = {2};
        if (current_first) singleton_stage(valid, 4);
        unsigned before = singleton_reads;
        singleton_stage(legacy, 36);
        CHECK(settings_call_set_handler(ROOT "state/effects/any", SIZE_MAX, singleton_read, NULL, NULL) == 0);
        CHECK(settings_call_set_handler(ROOT "unknown", SIZE_MAX, singleton_read, NULL, NULL) == 0);
        CHECK(singleton_reads == before);
        if (!current_first) singleton_stage(valid, 4);
        CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
        CHECK(kp_rgb_global.brightness == 88 && singleton_saves == 0);
        singleton_stage(valid, 4);
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
        struct kp_rgb_persist_global_state latest = {.user_on = true, .brightness = 77};
        struct kp_rgb_persist_global_record encoded;
        CHECK(kp_rgb_persist_encode_global(&latest, &encoded) == 0);
        CHECK(singleton_saves == 1);
        CHECK(singleton_write_time >= latest_request + CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE);
        CHECK(!memcmp(singleton_write_bytes, &encoded, sizeof(encoded)));
        CHECK(!memcmp(singleton_bytes, &encoded, sizeof(encoded)));
    } else if (!strcmp(settings_case, "staged-reset")) {
        struct kp_rgb_persist_global_state staged = {.user_on = false, .brightness = 17};
        struct kp_rgb_persist_global_record encoded;
        CHECK(kp_rgb_persist_encode_global(&staged, &encoded) == 0);
        singleton_stage((const uint8_t *)&encoded, sizeof(encoded));
        CHECK(kp_rgb_global.brightness == 61);
        kp_rgb_reset_state();
        CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
        CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && on);
        CHECK(kp_rgb_global.brightness == 100);
        k_sleep(K_MSEC(100));
        staged = (struct kp_rgb_persist_global_state){.user_on = true, .brightness = 100};
        CHECK(kp_rgb_persist_encode_global(&staged, &encoded) == 0);
        CHECK(singleton_saves == 1 && singleton_len == sizeof(encoded));
        CHECK(!memcmp(singleton_bytes, &encoded, sizeof(encoded)));
    } else if (!strcmp(settings_case, "failure")) {
        singleton_fail = true;
        CHECK(zmk_rgb_matrix_on() == 0);
        kp_rgb_matrix_lock(); kp_rgb_global.brightness = 22; kp_rgb_matrix_unlock();
        CHECK(kp_rgb_save_state() == 0);
        k_sleep(K_MSEC(10)); CHECK(singleton_saves == 0);
        k_sleep(K_MSEC(2200)); CHECK(singleton_failures >= 3 && singleton_bytes[2] == 61);
        singleton_fail = false; k_sleep(K_MSEC(1100));
        CHECK(singleton_bytes[2] == 22);
    } else if (strstr(settings_case, "reset") || strstr(settings_case, "dirty")) {
        bool reset = strstr(settings_case, "reset") != NULL;
        singleton_hook = reset ? 2 : 1;
        singleton_fail = strstr(settings_case, "failed") != NULL;
        kp_rgb_matrix_lock(); kp_rgb_global.brightness = 12; kp_rgb_matrix_unlock();
        CHECK(kp_rgb_save_state() == 0); k_sleep(K_MSEC(100));
        CHECK(singleton_hook == 0 && first_saved_brightness == 12);
        singleton_fail = false;
        k_sleep(K_MSEC(1100));
        CHECK(singleton_bytes[2] == (reset ? 100 : 45));
    } else {
        /* The real handler must ignore removed records without invoking storage. */
        unsigned before = singleton_reads;
        uint8_t retained[36]; memcpy(retained, singleton_bytes, singleton_len);
        memset(singleton_bytes, 0, sizeof(singleton_bytes)); singleton_bytes[0] = 2; singleton_len = 36;
        CHECK(settings_call_set_handler(ROOT "state", 36, singleton_read, NULL, NULL) == 0);
        CHECK(settings_call_set_handler(ROOT "state/effects/any", 36, singleton_read, NULL, NULL) == 0);
        CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
        CHECK(singleton_reads == before && singleton_saves == 0 && kp_rgb_global.brightness == 61);
        memcpy(singleton_bytes, retained, 4); singleton_len = 4;
        kp_rgb_reset_state(); k_sleep(K_MSEC(100));
        CHECK(singleton_bytes[2] == 100);
    }
    struct kp_rgb_persist_global_state durable;
    CHECK(kp_rgb_persist_decode_global(singleton_bytes, singleton_len, &durable) == 0);
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
#endif
    printk("PASS singleton settings %s\n", settings_case); exit(0);
}
K_THREAD_DEFINE(singleton_settings_thread, 4096, singleton_settings, NULL, NULL, NULL, 5, 0, 0);
