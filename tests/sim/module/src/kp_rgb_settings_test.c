/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include "rgb_matrix_internal.h"
#include "rgb_selector_sync_internal.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/init.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>
#include <zmk/events/rgb_selector_selection_changed.h>
#include <zmk/rgb_selector.h>

#include "cmdline.h"
#include "soc.h"

static const char *settings_case = "existing";
static const char *settings_storage;
static void settings_options(void) {
  static struct args_struct_t args[] = {
      {.option = "settings-scenario", .name = "scenario", .type = 's',
       .dest = (void *)&settings_case, .descript = "Isolated settings regression"},
      {.option = "settings-storage", .name = "file", .type = 's',
       .dest = (void *)&settings_storage, .descript = "External mock storage across firmware layouts"},
      ARG_TABLE_ENDMARKER};
  native_add_command_line_opts(args);
}
NATIVE_TASK(settings_options, PRE_BOOT_1, 11);
#define DEV(label) DEVICE_DT_GET(DT_NODELABEL(label))
#define CHECK(test)                                                            \
  do {                                                                         \
    if (!(test)) {                                                             \
      printk("FAIL scoped settings: %s line %d\n", #test, __LINE__);           \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
#define ROOT "keypaw/rgb_matrix/"

#if !DT_NODE_EXISTS(DT_NODELABEL(selector_a))
#include "kp_rgb_singleton_settings_test.inc"
#else
struct record {
  char name[SETTINGS_MAX_NAME_LEN + 1];
  uint8_t bytes[36];
  size_t len;
};
static struct record records[24];
static const char *fail_path;
static bool failed;
static const char *fail_once_path;
static unsigned fail_once_attempt, fail_once_seen;
static bool initial_load_done;
static unsigned reads;
static unsigned saves;

static int read_mode;
static const char *hook_path;
static unsigned hook_action;
static unsigned hook_calls;
static K_SEM_DEFINE(storage_request, 0, 1);
static K_SEM_DEFINE(storage_done, 0, 1);
struct write_attempt { char path[SETTINGS_MAX_NAME_LEN + 1]; int64_t time; int result; uint8_t bytes[36]; size_t len; };
static struct write_attempt journal[256];
static unsigned journal_count;

static void storage_query(void *a, void *b, void *c) {
  ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
  while (true) {
    k_sem_take(&storage_request, K_FOREVER);
    bool on;
    CHECK(zmk_rgb_matrix_get_user_state(&on) == 0);
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) != NULL);
    unsigned action = hook_action;
    hook_action = 0;
    if (action == 1) {
      CHECK(kp_rgb_selector_binding_apply(DEV(selector_a),
          KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 1),
          RGB_COLOR_HSB_VAL(288, 55, 100)) == 0);
      CHECK(kp_rgb_selector_binding_apply(DEV(selector_a),
          KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, 1), 2888) == 0);
      CHECK(kp_rgb_selector_binding_apply(DEV(selector_a), RGB_EFS_CMD, 2) == 0);
    } else if (action == 2) {
      CHECK(kp_rgb_selector_binding_apply(DEV(selector_a), RGB_RESET_CMD, 0) == 0);
    } else if (action == 3) {
      kp_rgb_matrix_lock();
      kp_rgb_global.brightness = 45;
      kp_rgb_matrix_unlock();
      CHECK(kp_rgb_save_state() == 0);
    } else if (action == 4) {
      kp_rgb_reset_state();
    }
    hook_calls++;
    k_sem_give(&storage_done);
  }
}
K_THREAD_DEFINE(storage_query_thread, 2048, storage_query, NULL, NULL, NULL, 5, 0, 0);
static struct record *find(const char *name) {
  struct record *empty = NULL;

  for (size_t i = 0; i < ARRAY_SIZE(records); i++) {
    if (!strcmp(records[i].name, name))
      return &records[i];
    if (!records[i].name[0])
      empty = &records[i];
  }
  CHECK(empty != NULL);
  strcpy(empty->name, name);
  return empty;
}

static ssize_t read_record(void *arg, void *out, size_t len) {
  struct record *record = arg;
  reads++;
  len = MIN(len, record->len);
  if (read_mode < 0) return -EIO;
  if (read_mode > 0) len--;
  memcpy(out, record->bytes, len);
  return len;
}

static int load(struct settings_store *store,
                const struct settings_load_arg *arg) {
  ARG_UNUSED(store);
  for (size_t i = 0; i < ARRAY_SIZE(records); i++) {
    struct record *record = &records[i];
    if (record->len)
      CHECK(settings_call_set_handler(record->name, record->len, read_record,
                                      record, arg) == 0);
  }
  initial_load_done = true;
  return 0;
}

static int save(struct settings_store *store, const char *name,
                const char *bytes, size_t len) {
  ARG_UNUSED(store);
  saves++;
  bool fail_once = fail_once_path && !strcmp(name, fail_once_path) &&
                   ++fail_once_seen == fail_once_attempt;
  bool write_failed = fail_once || (fail_path && !strcmp(name, fail_path));
  if (initial_load_done) {
    CHECK(journal_count < ARRAY_SIZE(journal));
    struct write_attempt *attempt = &journal[journal_count++];
    strcpy(attempt->path, name);
    attempt->time = k_uptime_get();
    CHECK(len <= sizeof(attempt->bytes));
    memcpy(attempt->bytes, bytes, len); attempt->len = len;
    attempt->result = write_failed ? -EIO : 0;
    if (hook_path && !strcmp(name, hook_path)) {
      hook_path = NULL;
      k_sem_give(&storage_request);
      CHECK(k_sem_take(&storage_done, K_MSEC(100)) == 0);
    }
  }
  if (write_failed) {
    failed = true;
    return -EIO;
  }
  struct record *record = find(name);
  CHECK(len <= sizeof(record->bytes));
  if (len)
    memcpy(record->bytes, bytes, len);
  record->len = len;
  return 0;
}

static const struct settings_store_itf interface = {.csi_load = load,
                                                    .csi_save = save};
static struct settings_store store = {.cs_itf = &interface};

static void seed(const char *name, const void *bytes, size_t len) {
  CHECK(save(&store, name, bytes, len) == 0);
}

int settings_backend_init(void) {
  struct kp_rgb_persist_global_state global = {.user_on = true, .brightness = 61};
  struct kp_rgb_persist_global_record global_record;
  kp_rgb_persist_encode_global(&global, &global_record);
  seed(ROOT "state", &global_record, sizeof(global_record));
  struct kp_rgb_persist_selector_record selection;
  CHECK(kp_rgb_persist_encode_selector("alt", &selection) == 0);
  seed(ROOT "selectors/a/state", &selection, sizeof(selection));
  seed(ROOT "selectors/b/state", &selection, sizeof(selection));
  struct kp_rgb_persist_effect value = {.duration_ms = 2100, .h = 42, .s = 70};
  struct kp_rgb_persist_parameters_record parameters;
  value.h = 123;
  CHECK(kp_rgb_persist_encode_parameters(&value, &parameters) == 0);
  seed(ROOT "selectors/a/effects/shared", &parameters, sizeof(parameters));
  value.h = 240;
  value.duration_ms = 3100;
  CHECK(kp_rgb_persist_encode_parameters(&value, &parameters) == 0);
  seed(ROOT "selectors/b/effects/shared", &parameters, sizeof(parameters));
  value.h = 240; value.s = 100; value.duration_ms = 1000;
  CHECK(kp_rgb_persist_encode_parameters(&value, &parameters) == 0);
  seed(ROOT "selectors/a/effects/alt", &parameters, sizeof(parameters));
  value.duration_ms = CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS;
  CHECK(kp_rgb_persist_encode_parameters(&value, &parameters) == 0);
  seed(ROOT "selectors/b/effects/alt", &parameters, sizeof(parameters));
  uint8_t obsolete[36] = {0};
  obsolete[0] = 2;
  seed(ROOT "state/effects/alt", obsolete, sizeof(obsolete));
  if (settings_storage) {
    FILE *file = fopen(settings_storage, "rb");
    if (file) {
      CHECK(fread(records, sizeof(records), 1, file) == 1);
      CHECK(fclose(file) == 0);
      for (size_t i = 0; i < ARRAY_SIZE(records); i++) {
        CHECK(memchr(records[i].name, 0, sizeof(records[i].name)));
        CHECK(records[i].len <= sizeof(records[i].bytes));
      }
    }
  }
  fail_path = NULL;
  failed = false;
  saves = 0;
  settings_src_register(&store);
  settings_dst_register(&store);
  return 0;
}

static void check_restored(void) {
  CHECK(initial_load_done);
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_c));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_d));
  CHECK(kp_rgb_global.brightness == 61 && kp_rgb_global.user_on);
  CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == 123);
  CHECK(kp_rgb_effect_data(DEV(probe_b))->color.h == 240);
  CHECK(kp_rgb_effect_data(DEV(probe_c))->color.h == 240);
  CHECK(kp_rgb_effect_period(DEV(probe_b)) == 3100);
}

static bool boot_checked;
static int selection(const zmk_event_t *event) {
  ARG_UNUSED(event);
  if (!boot_checked) {
    check_restored();
    boot_checked = true;
    printk("PASS scoped settings restored before startup notification\n");
  }
  return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(scoped_settings_probe, selection);
ZMK_SUBSCRIPTION(scoped_settings_probe, zmk_rgb_selector_selection_changed);

static void perturb_and_reload(void) {
  const struct device *effects[] = {DEV(probe_a), DEV(probe_b), DEV(probe_c),
                                    DEV(probe_d)};
  struct kp_rgb_hsb colors[ARRAY_SIZE(effects)];
  uint32_t periods[ARRAY_SIZE(effects)];
  const struct device *selected_a = zmk_rgb_selector_get_selected(DEV(selector_a));
  const struct device *selected_b = zmk_rgb_selector_get_selected(DEV(selector_b));
  bool on, current_on;
  CHECK(zmk_rgb_matrix_get_user_state(&on) == 0);
  kp_rgb_matrix_lock();
  uint8_t brightness = kp_rgb_global.brightness;
  kp_rgb_matrix_unlock();
  for (size_t i = 0; i < ARRAY_SIZE(effects); i++) {
    colors[i] = kp_rgb_effect_data(effects[i])->color;
    periods[i] = kp_rgb_effect_period(effects[i]);
    struct kp_rgb_hsb changed = {.h = (colors[i].h + 17) % 360,
                                 .s = colors[i].s == 25 ? 75 : 25, .b = 100};
    CHECK(zmk_rgb_matrix_set_color(effects[i], &changed) == 0);
    CHECK(zmk_rgb_matrix_set_period(effects[i], periods[i] == 1700 ? 1800 : 1700) == 0);
  }
  CHECK(zmk_rgb_selector_select(DEV(selector_a),
        selected_a == DEV(probe_a) ? DEV(probe_c) : DEV(probe_a)) == 0);
  CHECK(zmk_rgb_selector_select(DEV(selector_b),
        selected_b == DEV(probe_b) ? DEV(probe_d) : DEV(probe_b)) == 0);
  CHECK((on ? zmk_rgb_matrix_off() : zmk_rgb_matrix_on()) == 0);
  /* There is no public local-only brightness setter. */
  kp_rgb_matrix_lock();
  kp_rgb_global.brightness = brightness == 19 ? 27 : 19;
  zmk_rgb_matrix_flush();
  kp_rgb_matrix_unlock();
  unsigned before = saves;
  k_sleep(K_MSEC(60));
  CHECK(saves == before);
  CHECK(zmk_rgb_matrix_get_user_state(&current_on) == 0 && current_on != on);
  CHECK(kp_rgb_global.brightness != brightness);
  CHECK(settings_load_subtree("keypaw/rgb_matrix") == 0);
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == selected_a);
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == selected_b);
  CHECK(zmk_rgb_matrix_get_user_state(&current_on) == 0 && current_on == on);
  CHECK(kp_rgb_global.brightness == brightness);
  for (size_t i = 0; i < ARRAY_SIZE(effects); i++) {
    CHECK(kp_rgb_effect_data(effects[i])->color.h == colors[i].h);
    CHECK(kp_rgb_effect_data(effects[i])->color.s == colors[i].s);
    CHECK(kp_rgb_effect_period(effects[i]) == periods[i]);
  }
}

static void durable_parameters(const char *path, unsigned hue, unsigned period) {
  struct record *record = find(path);
  struct kp_rgb_persist_effect value;
  CHECK(kp_rgb_persist_decode_parameters(record->bytes, record->len, &value) == 0);
  CHECK(value.h == hue && value.duration_ms == period);
}
static unsigned durable_brightness(void) {
  struct record *record = find(ROOT "state");
  struct kp_rgb_persist_global_state value;
  CHECK(kp_rgb_persist_decode_global(record->bytes, record->len, &value) == 0);
  return value.brightness;
}
static void stage(const char *path, const void *bytes, size_t len) {
  struct record record = {.len = len};
  CHECK(len <= sizeof(record.bytes));
  memcpy(record.bytes, bytes, len);
  CHECK(settings_call_set_handler(path, len, read_record, &record, NULL) == 0);
}
static void validation_case(void) {
  struct kp_rgb_persist_global_record valid = {.version = 3, .user_on = 0,
                                                    .brightness = 66};
  struct kp_rgb_persist_selector_record ordered_selection;
  CHECK(kp_rgb_persist_encode_selector("shared", &ordered_selection) == 0);
  struct kp_rgb_persist_effect ordered_value = {.h = 211, .s = 37, .duration_ms = 1234};
  struct kp_rgb_persist_parameters_record ordered_parameter;
  CHECK(kp_rgb_persist_encode_parameters(&ordered_value, &ordered_parameter) == 0);
  for (unsigned order = 0; order < 2; order++) {
    valid.brightness = 66 + order;
    CHECK(kp_rgb_persist_encode_selector(order ? "alt" : "shared", &ordered_selection) == 0);
    ordered_value.h = 211 + order;
    ordered_value.duration_ms = 1234 + order;
    CHECK(kp_rgb_persist_encode_parameters(&ordered_value, &ordered_parameter) == 0);
    if (order) {
      stage(ROOT "state", &valid, sizeof(valid));
      stage(ROOT "selectors/a/state", &ordered_selection, sizeof(ordered_selection));
      stage(ROOT "selectors/a/effects/shared", &ordered_parameter, sizeof(ordered_parameter));
    }
    unsigned before = reads;
    uint8_t legacy[36] = {2};
    stage(ROOT "state", legacy, sizeof(legacy));
    stage(ROOT "state/effects/any", legacy, sizeof(legacy));
    stage(ROOT "unknown", legacy, sizeof(legacy));
    CHECK(reads == before);
    if (!order) {
      stage(ROOT "state", &valid, sizeof(valid));
      stage(ROOT "selectors/a/state", &ordered_selection, sizeof(ordered_selection));
      stage(ROOT "selectors/a/effects/shared", &ordered_parameter, sizeof(ordered_parameter));
    }
    struct kp_rgb_persist_global_record invalid[] = {
        {.version = 2, .user_on = 0, .brightness = 66},
        {.version = 3, .user_on = 2, .brightness = 66},
        {.version = 3, .user_on = 0, .brightness = 101},
        {.version = 3, .user_on = 0, .brightness = 66, .reserved = 1}};
    for (size_t i = 0; i < ARRAY_SIZE(invalid); i++)
      stage(ROOT "state", &invalid[i], sizeof(invalid[i]));
    stage(ROOT "state", &valid, sizeof(valid) - 1);
    read_mode = 1; stage(ROOT "state", &valid, sizeof(valid));
    read_mode = -1; stage(ROOT "state", &valid, sizeof(valid)); read_mode = 0;
    CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
    bool on; CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && !on);
    CHECK(kp_rgb_global.brightness == valid.brightness);
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == (order ? DEV(probe_c) : DEV(probe_a)));
    CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == ordered_value.h);
    CHECK(kp_rgb_effect_data(DEV(probe_a))->color.s == 37);
    CHECK(kp_rgb_effect_period(DEV(probe_a)) == ordered_value.duration_ms);
    CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
  }
  struct kp_rgb_persist_selector_record selected;
  CHECK(kp_rgb_persist_encode_selector("shared", &selected) == 0);
  stage(ROOT "selectors/a/state", &selected, sizeof(selected));
  selected.version = 99;
  stage(ROOT "selectors/a/state", &selected, sizeof(selected));
  stage(ROOT "selectors/a/state", &selected, sizeof(selected) - 1);
  read_mode = 1; stage(ROOT "selectors/a/state", &selected, sizeof(selected));
  read_mode = -1; stage(ROOT "selectors/a/state", &selected, sizeof(selected)); read_mode = 0;
  struct kp_rgb_persist_effect value = {.h = 123, .s = 44, .duration_ms = 888};
  struct kp_rgb_persist_parameters_record parameter;
  CHECK(kp_rgb_persist_encode_parameters(&value, &parameter) == 0);
  stage(ROOT "selectors/a/effects/shared", &parameter, sizeof(parameter));
  struct kp_rgb_persist_parameters_record bad = parameter;
  bad.version = 99; stage(ROOT "selectors/a/effects/shared", &bad, sizeof(bad));
  bad = parameter; bad.hue_le[0] = 105; bad.hue_le[1] = 1;
  stage(ROOT "selectors/a/effects/shared", &bad, sizeof(bad));
  bad = parameter; bad.saturation = 101;
  stage(ROOT "selectors/a/effects/shared", &bad, sizeof(bad));
  stage(ROOT "selectors/a/effects/shared", &bad, sizeof(bad) - 1);
  read_mode = 1; stage(ROOT "selectors/a/effects/shared", &parameter, sizeof(parameter));
  read_mode = -1; stage(ROOT "selectors/a/effects/shared", &parameter, sizeof(parameter)); read_mode = 0;
  unsigned before = reads;
  stage(ROOT "selectors/missing/state", &selected, sizeof(selected));
  stage(ROOT "selectors/a/effects/disabled", &parameter, sizeof(parameter));
  stage(ROOT "selectors/a/effects/missing", &parameter, sizeof(parameter));
  CHECK(reads == before);
  CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
  CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == 123);
  CHECK(kp_rgb_effect_period(DEV(probe_a)) == 888);
  CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_c)) == 0);
  CHECK(zmk_rgb_selector_select(DEV(selector_b), DEV(probe_d)) == 0);
  CHECK(kp_rgb_persist_encode_selector("removed", &selected) == 0);
  stage(ROOT "selectors/a/state", &selected, sizeof(selected));
  stage(ROOT "selectors/b/state", &selected, sizeof(selected));
  value = (struct kp_rgb_persist_effect){.h = 360, .s = 0, .duration_ms = 0};
  CHECK(kp_rgb_persist_encode_parameters(&value, &parameter) == 0);
  stage(ROOT "selectors/a/effects/shared", &parameter, sizeof(parameter));
  value = (struct kp_rgb_persist_effect){.h = 359, .s = 100, .duration_ms = 65535};
  CHECK(kp_rgb_persist_encode_parameters(&value, &parameter) == 0);
  stage(ROOT "selectors/b/effects/alt", &parameter, sizeof(parameter));
  CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
#if DT_NODE_EXISTS(DT_NODELABEL(optout))
  CHECK(DT_PROP(DT_NODELABEL(selector_a), initial_effect) == 3);
  CHECK(DT_PROP(DT_NODELABEL(selector_b), initial_effect) == 1);
  CHECK(device_is_ready(DEV(probe_a)) && device_is_ready(DEV(probe_b)));
#endif
  CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == 0);
  CHECK(kp_rgb_effect_data(DEV(probe_a))->color.s == 0);
  CHECK(kp_rgb_effect_period(DEV(probe_a)) == CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS);
  CHECK(kp_rgb_effect_data(DEV(probe_d))->color.h == 359);
  CHECK(kp_rgb_effect_period(DEV(probe_d)) == CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
  CHECK(kp_rgb_save_selector_state(DEV(probe_a), true) == -ENODEV);
  k_sleep(K_MSEC(100));
  CHECK(saves == 0);
}
static void extended_case(void) {
  if (!strcmp(settings_case, "reordering")) {
    CHECK(settings_storage);
#if DT_NODE_EXISTS(DT_NODELABEL(optout))
    struct kp_rgb_persist_effect ignored = {.h = 111, .s = 22, .duration_ms = 2222};
    struct kp_rgb_persist_parameters_record ignored_bytes;
    CHECK(kp_rgb_persist_encode_parameters(&ignored, &ignored_bytes) == 0);
    struct record ignored_record = {.len = sizeof(ignored_bytes)};
    memcpy(ignored_record.bytes, &ignored_bytes, sizeof(ignored_bytes));
    unsigned before_reads = reads;
    CHECK(settings_call_set_handler(ROOT "selectors/a/effects/optout", sizeof(ignored_bytes),
                                    read_record, &ignored_record, NULL) == 0);
    CHECK(reads == before_reads);
    CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
    CHECK(kp_rgb_effect_data(DEV(optout))->color.h == 0);
#endif
    CHECK(kp_rgb_save_selector_state(DEV(selector_a), false) == 0);
    CHECK(kp_rgb_save_selector_state(DEV(selector_b), false) == 0);
    k_sleep(K_MSEC(100));
    perturb_and_reload(); check_restored();
    FILE *file = fopen(settings_storage, "wb"); CHECK(file);
    CHECK(fwrite(records, sizeof(records), 1, file) == 1);
    CHECK(fclose(file) == 0);
  } else if (!strcmp(settings_case, "validation")) {
    validation_case();
  } else if (!strcmp(settings_case, "debounce")) {
    unsigned before = saves;
    int64_t started = k_uptime_get();
    CHECK(kp_rgb_selector_binding_apply(DEV(selector_a), RGB_SPI_CMD, 2300) == 0);
    k_sleep(K_MSEC(10));
    CHECK(saves == before);
    CHECK(kp_rgb_selector_binding_apply(DEV(selector_a), RGB_SPI_CMD, 2400) == 0);
    k_sleep(K_MSEC(10)); CHECK(saves == before);
    k_sleep(K_MSEC(60));
    CHECK(journal_count > 0 && journal[0].time >= started + 30);
    durable_parameters(ROOT "selectors/a/effects/alt", 240, 2400);
    perturb_and_reload();
  } else if (!strcmp(settings_case, "global-failure")) {
    fail_path = ROOT "state";
    kp_rgb_matrix_lock(); kp_rgb_global.brightness = 72; kp_rgb_matrix_unlock();
    CHECK(kp_rgb_save_state() == 0);
    CHECK(kp_rgb_selector_binding_apply(DEV(selector_a), RGB_SPI_CMD, 2300) == 0);
    CHECK(kp_rgb_selector_binding_apply(DEV(selector_b), RGB_SPI_CMD, 4300) == 0);
    k_sleep(K_MSEC(2200));
    CHECK(failed && durable_brightness() == 61);
    durable_parameters(ROOT "selectors/a/effects/alt", 240, 2300);
    durable_parameters(ROOT "selectors/b/effects/alt", 240, 4300);
    unsigned failures = 0; int64_t last = -1;
    for (unsigned i = 0; i < journal_count; i++) if (journal[i].result < 0) {
      CHECK(!strcmp(journal[i].path, ROOT "state"));
      CHECK(last < 0 || journal[i].time - last >= 1000);
      last = journal[i].time; failures++;
    }
    CHECK(failures >= 3);
    fail_path = NULL; k_sleep(K_MSEC(1100));
    CHECK(durable_brightness() == 72);
    perturb_and_reload();
  } else if (!strcmp(settings_case, "reset-revision-second-failure")) {
    struct record before_global = *find(ROOT "state");
    struct record before_b_state = *find(ROOT "selectors/b/state");
    struct record before_b_shared = *find(ROOT "selectors/b/effects/shared");
    struct record before_b_alt = *find(ROOT "selectors/b/effects/alt");
    hook_action = 2;
    hook_path = ROOT "selectors/a/effects/shared";
    fail_once_path = hook_path;
    fail_once_attempt = 2;
    CHECK(kp_rgb_selector_binding_apply(DEV(selector_a),
        KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 1),
        RGB_COLOR_HSB_VAL(299, 70, 100)) == 0);
    k_sleep(K_MSEC(100));
    CHECK(hook_calls == 1 && fail_once_seen == 1 && !failed);
    durable_parameters(ROOT "selectors/a/effects/shared", 299, 2100);
    struct kp_rgb_persist_selector_record retained;
    struct record *selection_record = find(ROOT "selectors/a/state");
    CHECK(kp_rgb_persist_decode_selector(selection_record->bytes, selection_record->len,
                                        &retained) == 0);
    CHECK(!strcmp(retained.selected_id, "alt"));
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
    k_sleep(K_MSEC(1100));
    CHECK(failed && fail_once_seen == 2);
    durable_parameters(ROOT "selectors/a/effects/shared", 299, 2100);
    k_sleep(K_MSEC(1100));
    CHECK(fail_once_seen == 3);
    durable_parameters(ROOT "selectors/a/effects/shared", 120, 1000);
    CHECK(kp_rgb_persist_decode_selector(selection_record->bytes, selection_record->len,
                                        &retained) == 0);
    CHECK(!strcmp(retained.selected_id, "shared"));
    unsigned attempts = 0;
    int64_t last = -1;
    for (unsigned i = 0; i < journal_count; i++) {
      if (strcmp(journal[i].path, fail_once_path)) continue;
      CHECK(last < 0 || journal[i].time - last >= 1000);
      last = journal[i].time;
      CHECK(journal[i].result == (attempts == 1 ? -EIO : 0));
      struct kp_rgb_persist_effect value;
      CHECK(kp_rgb_persist_decode_parameters(journal[i].bytes, journal[i].len, &value) == 0);
      CHECK(value.h == (attempts == 0 ? 299 : 120));
      CHECK(value.duration_ms == (attempts == 0 ? 2100 : 1000));
      attempts++;
    }
    CHECK(attempts == 3);
    const struct record *unchanged[] = {&before_global, &before_b_state,
                                      &before_b_shared, &before_b_alt};
    for (size_t i = 0; i < ARRAY_SIZE(unchanged); i++) {
      struct record *current = find(unchanged[i]->name);
      CHECK(current->len == unchanged[i]->len);
      CHECK(!memcmp(current->bytes, unchanged[i]->bytes, current->len));
    }
    unsigned settled = saves;
    k_sleep(K_MSEC(1100)); CHECK(saves == settled);
    perturb_and_reload();
  } else if (!strcmp(settings_case, "staged-reset")) {
    struct kp_rgb_persist_global_state global = {.user_on = false, .brightness = 33};
    struct kp_rgb_persist_global_record global_record;
    CHECK(kp_rgb_persist_encode_global(&global, &global_record) == 0);
    stage(ROOT "state", &global_record, sizeof(global_record));
    struct kp_rgb_persist_selector_record selection;
    CHECK(kp_rgb_persist_encode_selector("shared", &selection) == 0);
    stage(ROOT "selectors/a/state", &selection, sizeof(selection));
    stage(ROOT "selectors/b/state", &selection, sizeof(selection));
    struct kp_rgb_persist_effect value = {.h = 267, .s = 61, .duration_ms = 2345};
    struct kp_rgb_persist_parameters_record parameter;
    CHECK(kp_rgb_persist_encode_parameters(&value, &parameter) == 0);
#if DT_NODE_EXISTS(DT_NODELABEL(optout))
    unsigned before_reads = reads;
    stage(ROOT "selectors/a/effects/optout", &parameter, sizeof(parameter));
    CHECK(reads == before_reads);
#endif
    stage(ROOT "selectors/a/effects/shared", &parameter, sizeof(parameter));
    stage(ROOT "selectors/b/effects/shared", &parameter, sizeof(parameter));
    stage(ROOT "selectors/a/effects/alt", &parameter, sizeof(parameter));
    CHECK(kp_rgb_selector_binding_apply(DEV(selector_a), RGB_RESET_CMD, 0) == 0);
    CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
    CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == 120);
    CHECK(kp_rgb_effect_data(DEV(probe_c))->color.h == 240);
#if DT_NODE_EXISTS(DT_NODELABEL(optout))
    CHECK(kp_rgb_effect_data(DEV(optout))->color.h == 0);
#endif
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
    CHECK(kp_rgb_effect_data(DEV(probe_b))->color.h == 267);
    bool on;
    CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && !on);
    CHECK(kp_rgb_global.brightness == 33);
    CHECK(kp_rgb_save_state() == 0);
    k_sleep(K_MSEC(100));
    CHECK(kp_rgb_save_selector_state(DEV(selector_b), false) == 0);
    k_sleep(K_MSEC(100)); perturb_and_reload();
    stage(ROOT "selectors/a/effects/shared", &parameter, sizeof(parameter));
    stage(ROOT "state", &global_record, sizeof(global_record));
    stage(ROOT "selectors/b/state", &selection, sizeof(selection));
    stage(ROOT "selectors/b/effects/shared", &parameter, sizeof(parameter));
    kp_rgb_reset_state();
    CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
    CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == 120);
    CHECK(kp_rgb_effect_data(DEV(probe_b))->color.h == 60);
    CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && on);
    CHECK(kp_rgb_global.brightness == 100);
    k_sleep(K_MSEC(100)); perturb_and_reload();
  } else {
    bool global = !strncmp(settings_case, "global-", 7);
    bool reset = strstr(settings_case, "reset") != NULL;
    bool failed_save = strstr(settings_case, "failed") != NULL;
    hook_action = global ? (reset ? 4 : 3) : (reset ? 2 : 1);
    hook_path = global ? ROOT "state" : ROOT "selectors/a/effects/shared";
    if (failed_save) fail_path = hook_path;
    if (global) {
      kp_rgb_matrix_lock(); kp_rgb_global.brightness = 12; kp_rgb_matrix_unlock();
      CHECK(kp_rgb_save_state() == 0);
    } else {
      CHECK(kp_rgb_selector_binding_apply(DEV(selector_a),
          KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 1),
          RGB_COLOR_HSB_VAL(299, 70, 100)) == 0);
    }
    k_sleep(K_MSEC(100));
    CHECK(hook_calls == 1 && hook_path == NULL);
    if (!failed_save) {
      if (global) {
        struct kp_rgb_persist_global_state captured;
        CHECK(kp_rgb_persist_decode_global(journal[0].bytes, journal[0].len, &captured) == 0);
        CHECK(captured.brightness == 12);
      } else durable_parameters(ROOT "selectors/a/effects/shared", 299, 2100);
    }
    fail_path = NULL;
    k_sleep(K_MSEC(1100));
    if (global) CHECK(durable_brightness() == (reset ? 100 : 45));
    else durable_parameters(ROOT "selectors/a/effects/shared", reset ? 120 : 288,
                            reset ? 1000 : 2888);
    unsigned settled = saves; k_sleep(K_MSEC(1100)); CHECK(saves == settled);
    perturb_and_reload();
  }
  printk("PASS settings %s\n", settings_case);
  exit(0);
}

static void scenario(void *a, void *b, void *c) {
  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);
  k_sleep(K_MSEC(100));
  if (!strncmp(settings_case, "ids-", 4)) {
    int error = !strcmp(settings_case, "ids-duplicate") ? -EEXIST : -EINVAL;
    CHECK(kp_rgb_settings_commit() == error);
    zmk_rgb_matrix_start();
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == NULL);
    bool on; CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && on);
    CHECK(kp_rgb_global.brightness == 100);
    CHECK(!boot_checked && saves == 0);
    printk("PASS settings %s\n", settings_case); exit(0);
  }
#if !IS_ENABLED(CONFIG_KEYPAW_RGB_CONTROL_AUTO_START)
  CHECK(initial_load_done);
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
  #if DT_PROP(DT_NODELABEL(probe_b), zephyr_deferred_init)
  CHECK(!device_is_ready(DEV(probe_b)));
  CHECK(settings_load_subtree("keypaw/rgb_matrix") == -ENODEV);
  bool on;
  CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && on);
  CHECK(kp_rgb_global.brightness == 100);
  zmk_rgb_matrix_start();
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
  CHECK(device_init(DEV(probe_b)) == 0);
  CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
#else
  CHECK(settings_load_subtree("keypaw/rgb_matrix") == 0);
#endif
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
#if DT_PROP(DT_NODELABEL(rgbs_a), zephyr_deferred_init)
  CHECK(!device_is_ready(DEV(rgbs_a)));
  zmk_rgb_matrix_start();
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
  CHECK(device_init(DEV(rgbs_a)) == 0);
#endif
  zmk_rgb_matrix_start();
  k_sleep(K_MSEC(50));
  printk("PASS scoped settings subtree/manual startup\n");
#endif
  check_restored();
  if (strcmp(settings_case, "existing")) extended_case();
  struct record *obsolete_record = find(ROOT "state/effects/alt");
  unsigned before_reads = reads;
  CHECK(settings_call_set_handler(ROOT "state/effects/alt", obsolete_record->len,
                                  read_record, obsolete_record, NULL) == 0);
  CHECK(reads == before_reads);
  uint8_t legacy_state[36] = {2};
  struct record legacy_record = {.len = sizeof(legacy_state)};
  memcpy(legacy_record.bytes, legacy_state, sizeof(legacy_state));
  /* Zephyr logs and discards h_set errors; rejection must not read or stage. */
  CHECK(settings_call_set_handler(ROOT "state", legacy_record.len, read_record,
                                  &legacy_record, NULL) == 0);
  CHECK(reads == before_reads);
  CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
  check_restored();
  printk("PASS legacy state rejected and obsolete effect ignored\n");
  k_sleep(K_MSEC(150));
  CHECK(!failed && saves == 0);
  CHECK(find(ROOT "state")->len == sizeof(struct kp_rgb_persist_global_record));
  CHECK(find(ROOT "state/effects/alt")->len == sizeof(uint8_t[36]));
  printk("PASS scoped settings legacy records ignored\n");
  k_sleep(K_MSEC(1200));
  CHECK(find(ROOT "state")->len == sizeof(struct kp_rgb_persist_global_record));
  CHECK(find(ROOT "state/effects/alt")->len == sizeof(uint8_t[36]));

  struct record before_b = *find(ROOT "selectors/b/state");
  struct record before_global = *find(ROOT "state");
  CHECK(kp_rgb_selector_binding_apply(DEV(selector_b), RGB_SPI_CMD, 4200) == 0);
  k_sleep(K_MSEC(100));
  CHECK(find(ROOT "selectors/b/state")->len == before_b.len);
  CHECK(kp_rgb_selector_binding_apply(DEV(selector_a), RGB_RESET_CMD, 0) == 0);
  k_sleep(K_MSEC(100));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_d));
  CHECK(kp_rgb_effect_period(DEV(probe_d)) == 4200);
  CHECK(!memcmp(find(ROOT "state")->bytes, before_global.bytes,
                before_global.len));
  CHECK(!memcmp(find(ROOT "selectors/b/state")->bytes, before_b.bytes,
                before_b.len));
  perturb_and_reload();
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_d));
  CHECK(kp_rgb_effect_period(DEV(probe_d)) == 4200);
  printk("PASS scoped settings addressed save/reset/reload isolation\n");

  fail_path = ROOT "selectors/a/effects/alt";
  failed = false;
  CHECK(kp_rgb_selector_binding_apply(DEV(selector_a), RGB_SPI_CMD, 2300) == 0);
  CHECK(kp_rgb_selector_binding_apply(DEV(selector_b), RGB_SPI_CMD, 4300) == 0);
  kp_rgb_matrix_lock();
  kp_rgb_global.brightness = 47;
  kp_rgb_matrix_unlock();
  CHECK(kp_rgb_save_state() == 0);
  k_sleep(K_MSEC(1100));
  CHECK(failed);
  struct kp_rgb_persist_global_state durable_global;
  struct record *global_record = find(ROOT "state");
  CHECK(kp_rgb_persist_decode_global(global_record->bytes, global_record->len,
                                    &durable_global) == 0);
  CHECK(durable_global.brightness == 47);
  struct kp_rgb_persist_effect durable_parameters;
  struct record *parameter_record = find(ROOT "selectors/b/effects/alt");
  CHECK(kp_rgb_persist_decode_parameters(parameter_record->bytes,
       parameter_record->len, &durable_parameters) == 0);
  CHECK(durable_parameters.duration_ms == 4300);
  printk("PASS scoped settings independent save despite persistent failure\n");
  fail_path = NULL;
  uint8_t removed[36] = {2};
  seed(ROOT "state/effects/removed", removed, sizeof(removed));
  kp_rgb_reset_state();
  k_sleep(K_MSEC(100));
  CHECK(find(ROOT "state/effects/removed")->len == sizeof(removed));
  perturb_and_reload();
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
  CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == 120);
  CHECK(kp_rgb_effect_data(DEV(probe_b))->color.h == 60);
  CHECK(saves > 0 && boot_checked);
  printk("PASS scoped settings global reset/reload\n");
  exit(0);
}
K_THREAD_DEFINE(settings_scenario, 4096, scenario, NULL, NULL, NULL, 5, 0, 0);
#endif
