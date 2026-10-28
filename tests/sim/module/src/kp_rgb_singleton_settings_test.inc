/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#if IS_ENABLED(CONFIG_SETTINGS)
static uint8_t singleton_bytes[32];
static size_t singleton_len;
static unsigned singleton_saves, singleton_reads;
static int singleton_read_mode;
static int64_t singleton_saved_at;
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
    CHECK(bytes[0] == 1 && bytes[3] == 0 && bytes[4] == 0 && bytes[5] == 0);
    singleton_saves++;
    singleton_saved_at = k_uptime_get();
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
        const uint8_t valid[] = {1, 0, 88, 0, 0, 0};
        if (current_first) singleton_stage(valid, sizeof(valid));
        singleton_legacy();
        if (!current_first) singleton_stage(valid, sizeof(valid));
        CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
        CHECK(kp_rgb_global.brightness == 88 && singleton_saves == 0);
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
        CHECK(singleton_saved_at < reset_time + CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE);
        CHECK(!memcmp(singleton_bytes, expected, sizeof(expected)));
    } else if (!strcmp(settings_case, "repeated-commits")) {
        CHECK(zmk_rgb_matrix_on() == 0);
        kp_rgb_matrix_lock(); kp_rgb_global.brightness = 39; kp_rgb_matrix_unlock();
        for (unsigned i = 0; i < 4; i++) {
            CHECK((i % 2 ? settings_commit() : settings_commit_subtree("keypaw/rgb_matrix")) == 0);
            CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && on);
            CHECK(kp_rgb_global.brightness == 39 && singleton_saves == 0);
        }
    } else if (!strcmp(settings_case, "existing")) {
        singleton_legacy();
        CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
        CHECK(singleton_saves == 0 && kp_rgb_global.brightness == 61);
        kp_rgb_reset_state(); k_sleep(K_MSEC(100));
        CHECK(singleton_saves == 1 && singleton_bytes[2] == 100);
    } else CHECK(false);
    CHECK(singleton_len == 6 && singleton_bytes[0] == 1 && singleton_bytes[1] <= 1 &&
          singleton_bytes[2] <= 100 && singleton_bytes[3] == 0 &&
          singleton_bytes[4] == 0 && singleton_bytes[5] == 0);
    const bool durable_on = singleton_bytes[1] != 0;
    const uint8_t durable_brightness = singleton_bytes[2];
    CHECK((durable_on ? zmk_rgb_matrix_off() : zmk_rgb_matrix_on()) == 0);
    kp_rgb_matrix_lock(); kp_rgb_global.brightness = 19; kp_rgb_matrix_unlock();
    unsigned settled = singleton_saves; k_sleep(K_MSEC(1100)); CHECK(singleton_saves == settled);
    CHECK(settings_load_subtree("keypaw/rgb_matrix") == 0);
    CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && on == durable_on);
    CHECK(kp_rgb_global.brightness == durable_brightness);
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
