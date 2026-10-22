/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include "rgb_matrix_internal.h"
#include "rgb_selector_internal.h"
#include "rgb_selector_sync_internal.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/init.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zmk/events/rgb_selector_selection_changed.h>
#include <zmk/rgb_persist.h>
#include <zmk/rgb_selector.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>

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
#define KP_TEST_NODE_TOKEN(node) STRINGIFY(DT_NODE_FULL_NAME_TOKEN(node))
#define SELECTOR_A_PATH ROOT "selectors/" KP_TEST_NODE_TOKEN(DT_NODELABEL(selector_a)) "/"
#define SELECTOR_B_PATH ROOT "selectors/" KP_TEST_NODE_TOKEN(DT_NODELABEL(selector_b)) "/"
#define A_STATIC_NAME KP_TEST_NODE_TOKEN(DT_NODELABEL(probe_a))
#define A_ALT_NAME KP_TEST_NODE_TOKEN(DT_NODELABEL(probe_c))
#define B_STATIC_NAME KP_TEST_NODE_TOKEN(DT_NODELABEL(probe_b))
#define B_ALT_NAME KP_TEST_NODE_TOKEN(DT_NODELABEL(probe_d))


#if !DT_NODE_EXISTS(DT_NODELABEL(selector_a))
#include "kp_rgb_singleton_settings_test.inc"
#else
static uint16_t member_slot(const struct device *owner, const struct device *member);
struct record {
  char name[SETTINGS_MAX_NAME_LEN + 32];
  uint8_t bytes[36];
  size_t len;
};
static struct record records[24];
static const char *fail_path;
static bool failed;
static const char *fail_once_path;
static unsigned fail_once_attempt, fail_once_seen;
static bool initial_load_done;
static bool external_storage_loaded;
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
      CHECK(kp_rgb_selector_binding_apply(DEV(selector_a), RGB_EFS_CMD,
          member_slot(DEV(selector_a), DEV(probe_c))) == 0);
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
  CHECK(strstr(record->name, "/effects/") == NULL);
  CHECK(strcmp(record->name, ROOT "selectors/a/state"));
  CHECK(strcmp(record->name, ROOT "selectors/b/state"));
  CHECK(strcmp(record->name, ROOT "selectors/selector-a@1/state"));
  CHECK(strcmp(record->name, ROOT "selectors/selector-b@2/state"));
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
  if (initial_load_done) {
    CHECK(strstr(name, "/effects/") == NULL);
    CHECK(strcmp(name, ROOT "selectors/a/state"));
    CHECK(strcmp(name, ROOT "selectors/b/state"));
    CHECK(strcmp(name, ROOT "selectors/selector-a@1/state"));
    CHECK(strcmp(name, ROOT "selectors/selector-b@2/state"));
    CHECK(len != 0);
  }
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
  CHECK(kp_rgb_persist_encode_selector(A_ALT_NAME, &selection) == 0);
  seed(SELECTOR_A_PATH "state", &selection, sizeof(selection));
  CHECK(kp_rgb_persist_encode_selector(B_ALT_NAME, &selection) == 0);
  seed(SELECTOR_B_PATH "state", &selection, sizeof(selection));
  CHECK(kp_rgb_persist_encode_selector(A_STATIC_NAME, &selection) == 0);
  seed(ROOT "selectors/a/state", &selection, sizeof(selection));
  CHECK(kp_rgb_persist_encode_selector(B_STATIC_NAME, &selection) == 0);
  seed(ROOT "selectors/b/state", &selection, sizeof(selection));
  if (!strcmp(settings_case, "normalized-names")) {
    CHECK(kp_rgb_persist_encode_selector(A_STATIC_NAME, &selection) == 0);
    seed(ROOT "selectors/selector-a@1/state", &selection, sizeof(selection));
    CHECK(kp_rgb_persist_encode_selector(B_STATIC_NAME, &selection) == 0);
    seed(ROOT "selectors/selector-b@2/state", &selection, sizeof(selection));
  }
  /* Obsolete v1 parameter bytes are deliberately independent of production codecs. */
  const uint8_t parameters[] = {1, 0x34, 0x08, 123, 0, 70};
  const uint8_t malformed[] = {99, 0xff, 0xff, 0xff, 0xff, 101};
  const uint8_t oversized[36] = {1};
  seed(SELECTOR_A_PATH "effects/shared", parameters, sizeof(parameters));
  seed(SELECTOR_B_PATH "effects/shared", parameters, sizeof(parameters));
  seed(SELECTOR_A_PATH "effects/" A_STATIC_NAME, parameters, sizeof(parameters));
  seed(SELECTOR_B_PATH "effects/" B_STATIC_NAME, parameters, sizeof(parameters));
  seed(SELECTOR_A_PATH "effects/" A_ALT_NAME, malformed, sizeof(malformed));
  seed(SELECTOR_B_PATH "effects/" B_ALT_NAME, oversized, sizeof(oversized));
  seed(SELECTOR_A_PATH "effects/disabled", parameters, sizeof(parameters));
  seed(SELECTOR_A_PATH "effects/missing", parameters, 1);
  seed(SELECTOR_A_PATH "effects/optout", parameters, sizeof(parameters));
  seed(ROOT "selectors/missing/effects/shared", parameters, sizeof(parameters));
  uint8_t obsolete[36] = {0};
  obsolete[0] = 2;
  seed(ROOT "state/effects/alt", obsolete, sizeof(obsolete));
  if (settings_storage) {
    FILE *file = fopen(settings_storage, "rb");
    if (file) {
      CHECK(fread(records, sizeof(records), 1, file) == 1);
      CHECK(fclose(file) == 0);
      external_storage_loaded = true;
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

static int settings_provider_commit(void) {
  static bool done;
  if (done) return 0;
  done = true;
  CHECK(initial_load_done);
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == NULL);
  CHECK(kp_rgb_global.brightness == 100);
  if (strcmp(settings_case, "boundary-failure")) {
#if DT_PROP(DT_NODELABEL(probe_b), zephyr_deferred_init)
    CHECK(!device_is_ready(DEV(probe_b)));
    CHECK(device_init(DEV(probe_b)) == 0);
#endif
#if DT_PROP(DT_NODELABEL(selector_a), zephyr_deferred_init)
    CHECK(!device_is_ready(DEV(selector_a)));
    CHECK(device_init(DEV(selector_a)) == 0);
#endif
  }
  printk("PASS scoped settings provider before restoration\n");
  return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE_WITH_CPRIO(scoped_settings_provider, "test/rgb_provider",
                                        NULL, NULL, settings_provider_commit, NULL,
                                        INT_MAX - 2);

static void check_defaults(void) {
  const struct device *effects[] = {DEV(probe_a), DEV(probe_b), DEV(probe_c), DEV(probe_d)};
  const unsigned hues[] = {120, 60, 240, 180};
  for (size_t i = 0; i < ARRAY_SIZE(effects); i++) {
    const struct kp_rgb_hsb *color = &kp_rgb_effect_data(effects[i])->color;
    CHECK(color->h == hues[i] && color->s == 100 && color->b == 100);
    CHECK(kp_rgb_effect_period(effects[i]) ==
          (i == 0 || i == 2 ? CLAMP(1000, CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS,
                                   CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS)
                           : CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS));
  }
}

static void check_restored(void) {
  CHECK(initial_load_done);
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_c));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) ==
        (external_storage_loaded ? DEV(probe_b) : DEV(probe_d)));
  CHECK(!strcmp(DEV(selector_a)->name, "rgbs_a"));
  CHECK(!strcmp(DEV(selector_b)->name, "rgbs_b"));
  CHECK(zmk_behavior_get_binding(DEV(selector_a)->name) == DEV(selector_a));
  CHECK(zmk_behavior_get_binding(DEV(ordinary)->name) == NULL);
  for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++) {
    CHECK(kp_rgb_selector_registry_at(i)->owner != DEV(ordinary));
  }
  CHECK(kp_rgb_global.brightness == 61 && kp_rgb_global.user_on);
  check_defaults();
}

static bool boot_checked;
static int selection(const zmk_event_t *event) {
  ARG_UNUSED(event);
  if (!strcmp(settings_case, "boundary-failure")) {
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
    return ZMK_EV_EVENT_BUBBLE;
  }
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
                                 .s = colors[i].s == 25 ? 75 : 25, .b = (uint8_t)(i * 25)};
    CHECK(zmk_rgb_matrix_set_color(effects[i], &changed) == 0);
    CHECK(zmk_rgb_matrix_set_period(effects[i], periods[i] == 1700 ? 1800 : 1700) == 0);
    colors[i] = changed;
    periods[i] = kp_rgb_effect_period(effects[i]);
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
    CHECK(kp_rgb_effect_data(effects[i])->color.b == colors[i].b);
    CHECK(kp_rgb_effect_period(effects[i]) == periods[i]);
  }
  CHECK(saves == before);
}

static void check_selection_bytes(const void *bytes, size_t len, const char *id) {
  struct kp_rgb_persist_selector_record value;
  CHECK(kp_rgb_persist_decode_selector(bytes, len, &value) == 0);
  CHECK(!strcmp(value.selected_id, id));
}

static void durable_selection(const char *path, const char *id) {
  struct record *record = find(path);
  check_selection_bytes(record->bytes, record->len, id);
}

static unsigned durable_brightness(void) {
  struct record *record = find(ROOT "state");
  struct kp_rgb_persist_global_state value;
  CHECK(kp_rgb_persist_decode_global(record->bytes, record->len, &value) == 0);
  return value.brightness;
}

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

static void select_member(const struct device *selector, const struct device *member) {
  CHECK(kp_rgb_selector_binding_apply(selector, RGB_EFS_CMD,
                                     member_slot(selector, member)) == 0);
}

static void stage(const char *path, const void *bytes, size_t len) {
  struct record record = {.len = len};
  CHECK(len <= sizeof(record.bytes));
  strcpy(record.name, path);
  memcpy(record.bytes, bytes, len);
  CHECK(settings_call_set_handler(path, len, read_record, &record, NULL) == 0);
}

static void obsolete_parameters(void) {
  const char *paths[] = {
      SELECTOR_A_PATH "effects/shared", SELECTOR_A_PATH "effects/" A_STATIC_NAME,
      SELECTOR_A_PATH "effects/" A_ALT_NAME, SELECTOR_B_PATH "effects/shared",
      SELECTOR_B_PATH "effects/" B_ALT_NAME, SELECTOR_B_PATH "effects/" B_STATIC_NAME,
      SELECTOR_A_PATH "effects/disabled",
      SELECTOR_A_PATH "effects/missing", SELECTOR_A_PATH "effects/optout",
      ROOT "selectors/missing/effects/shared", ROOT "state/effects/any"};
  const uint8_t valid[] = {1, 0x34, 0x08, 123, 0, 70};
  const uint8_t malformed[] = {99, 0xff, 0xff, 0xff, 0xff, 101};
  const uint8_t oversized[36] = {1};
  unsigned before_reads = reads, before_saves = saves;
  for (size_t i = 0; i < ARRAY_SIZE(paths); i++) {
    stage(paths[i], valid, sizeof(valid));
    stage(paths[i], malformed, sizeof(malformed));
    stage(paths[i], valid, 1);
    stage(paths[i], oversized, sizeof(oversized));
    read_mode = -1; stage(paths[i], valid, sizeof(valid));
    read_mode = 1; stage(paths[i], valid, sizeof(valid));
    read_mode = 0;
  }
  CHECK(reads == before_reads && saves == before_saves);
}

static void invoke(const struct device *device, uint32_t command, uint32_t argument,
                   const struct device *endpoint) {
  struct zmk_behavior_binding binding = {
      .behavior_dev = device->name, .param1 = command, .param2 = argument};
  struct zmk_behavior_binding converted = binding;
  CHECK(behavior_keymap_binding_convert_central_state_dependent_params(
      &converted, (struct zmk_behavior_binding_event){0}) == 0);
  CHECK(!strcmp(converted.behavior_dev, endpoint->name));
  CHECK(zmk_behavior_invoke_binding(&binding, (struct zmk_behavior_binding_event){0}, true) == 0);
  CHECK(zmk_behavior_invoke_binding(&binding, (struct zmk_behavior_binding_event){0}, false) == 0);
}

static void qualified_parameter(const struct device *control, uint32_t command,
                                uint32_t argument) {
  struct zmk_behavior_binding binding = {
      .behavior_dev = control->name, .param1 = command, .param2 = argument};
  /* Split reception applies already-qualified bindings without central conversion. */
  CHECK(behavior_keymap_binding_pressed(&binding, (struct zmk_behavior_binding_event){0}) == 0);
  CHECK(behavior_keymap_binding_released(&binding, (struct zmk_behavior_binding_event){0}) == 0);
}

static void runtime_parameters(void) {
  unsigned before = saves;
  const uint32_t commands[] = {RGB_HUI_CMD, RGB_HUD_CMD, RGB_SAI_CMD, RGB_SAD_CMD,
                              RGB_SPI_CMD, RGB_SPD_CMD, RGB_COLOR_HSB_CMD};
  for (size_t i = 0; i < ARRAY_SIZE(commands); i++) {
    uint32_t argument = commands[i] == RGB_COLOR_HSB_CMD ? RGB_COLOR_HSB_VAL(211, 37, 43) : 0;
    invoke(DEV(selector_a), commands[i], argument, DEV(selector_a));
    invoke(DEV(selector_a), commands[i], argument, DEV(selector_a));
    k_sleep(K_MSEC(60));
    CHECK(saves == before);
  }
  CHECK(kp_rgb_effect_data(DEV(probe_c))->color.h == 211);
  CHECK(kp_rgb_effect_data(DEV(probe_c))->color.s == 37);
  CHECK(kp_rgb_effect_data(DEV(probe_c))->color.b == 43);
  qualified_parameter(DEV(selector_a),
      KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, member_slot(DEV(selector_a), DEV(probe_a))),
      RGB_COLOR_HSB_VAL(288, 55, 29));
  qualified_parameter(DEV(selector_a),
      KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, member_slot(DEV(selector_a), DEV(probe_a))),
      2888);
  qualified_parameter(DEV(selector_b),
      KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, member_slot(DEV(selector_b), DEV(probe_b))),
      3888);
  k_sleep(K_MSEC(1100)); CHECK(saves == before);
  CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == 288);
  CHECK(kp_rgb_effect_period(DEV(probe_a)) == 2888);
  CHECK(settings_load_subtree("keypaw/rgb_matrix") == 0);
  CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == 288);
  CHECK(kp_rgb_effect_data(DEV(probe_a))->color.b == 29);
  CHECK(kp_rgb_effect_data(DEV(probe_c))->color.b == 43);
  CHECK(saves == before);
  CHECK(kp_rgb_effect_period(DEV(probe_a)) == 2888);
  CHECK(kp_rgb_effect_data(DEV(probe_c))->color.h == 211);
  CHECK(kp_rgb_effect_period(DEV(probe_b)) == 3888);
  struct record before_b = *find(SELECTOR_B_PATH "state");
  struct record before_global = *find(ROOT "state");
  invoke(DEV(selector_a), RGB_EFS_CMD, member_slot(DEV(selector_a), DEV(probe_a)), DEV(selector_a));
  k_sleep(K_MSEC(100));
  CHECK(saves == before + 1);
  durable_selection(SELECTOR_A_PATH "state", A_STATIC_NAME);
  CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == 288);
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_d));
  CHECK(!strcmp(journal[0].path, SELECTOR_A_PATH "state"));
  CHECK(find(before_b.name)->len == before_b.len);
  CHECK(!memcmp(find(before_b.name)->bytes, before_b.bytes, before_b.len));
  invoke(DEV(selector_b), RGB_EFS_CMD, member_slot(DEV(selector_b), DEV(probe_b)), DEV(selector_b));
  k_sleep(K_MSEC(100)); CHECK(saves == before + 2);
  CHECK(!strcmp(journal[1].path, SELECTOR_B_PATH "state"));
  durable_selection(SELECTOR_B_PATH "state", B_STATIC_NAME);
  durable_selection(SELECTOR_A_PATH "state", A_STATIC_NAME);
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
  CHECK(kp_rgb_effect_period(DEV(probe_b)) == 3888);
  CHECK(find(before_global.name)->len == before_global.len);
  CHECK(!memcmp(find(before_global.name)->bytes, before_global.bytes, before_global.len));
  unsigned settled = saves; k_sleep(K_MSEC(1100)); CHECK(saves == settled);
  perturb_and_reload();
}

static void validation_case(void) {
  struct kp_rgb_persist_global_record valid = {.version = 3, .user_on = 0, .brightness = 66};
  struct kp_rgb_persist_selector_record selected;
  struct kp_rgb_hsb live = {.h = 211, .s = 37, .b = 41};
  CHECK(zmk_rgb_matrix_set_color(DEV(probe_a), &live) == 0);
  CHECK(zmk_rgb_matrix_set_period(DEV(probe_a), 1234) == 0);
  for (unsigned order = 0; order < 2; order++) {
    valid.brightness = 66 + order;
    CHECK(kp_rgb_persist_encode_selector(order ? A_ALT_NAME : A_STATIC_NAME, &selected) == 0);
    if (order) {
      stage(ROOT "state", &valid, sizeof(valid));
      stage(SELECTOR_A_PATH "state", &selected, sizeof(selected));
    }
    unsigned before = reads;
    uint8_t legacy[36] = {2};
    stage(ROOT "state", legacy, sizeof(legacy));
    stage(ROOT "unknown", legacy, sizeof(legacy));
    obsolete_parameters();
    CHECK(reads == before);
    if (!order) {
      stage(ROOT "state", &valid, sizeof(valid));
      stage(SELECTOR_A_PATH "state", &selected, sizeof(selected));
    }
    struct kp_rgb_persist_global_record invalid[] = {
        {.version = 2, .brightness = 66}, {.version = 3, .user_on = 2, .brightness = 66},
        {.version = 3, .brightness = 101}, {.version = 3, .brightness = 66, .reserved = 1}};
    for (size_t i = 0; i < ARRAY_SIZE(invalid); i++)
      stage(ROOT "state", &invalid[i], sizeof(invalid[i]));
    stage(ROOT "state", &valid, sizeof(valid) - 1);
    read_mode = 1; stage(ROOT "state", &valid, sizeof(valid));
    read_mode = -1; stage(ROOT "state", &valid, sizeof(valid)); read_mode = 0;
    CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
    bool on; CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && !on);
    CHECK(kp_rgb_global.brightness == valid.brightness);
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == (order ? DEV(probe_c) : DEV(probe_a)));
    CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == live.h);
    CHECK(kp_rgb_effect_data(DEV(probe_a))->color.s == live.s);
    CHECK(kp_rgb_effect_data(DEV(probe_a))->color.b == live.b);
    CHECK(kp_rgb_effect_period(DEV(probe_a)) == 1234);
    CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
  }
  CHECK(kp_rgb_persist_encode_selector(A_STATIC_NAME, &selected) == 0);
  stage(SELECTOR_A_PATH "state", &selected, sizeof(selected));
  selected.version = 99;
  stage(SELECTOR_A_PATH "state", &selected, sizeof(selected));
  stage(SELECTOR_A_PATH "state", &selected, sizeof(selected) - 1);
  read_mode = 1; stage(SELECTOR_A_PATH "state", &selected, sizeof(selected));
  read_mode = -1; stage(SELECTOR_A_PATH "state", &selected, sizeof(selected)); read_mode = 0;
  unsigned before = reads;
  stage(ROOT "selectors/missing/state", &selected, sizeof(selected));
  obsolete_parameters();
  CHECK(reads == before);
  CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
  CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_c)) == 0);
  CHECK(zmk_rgb_selector_select(DEV(selector_b), DEV(probe_d)) == 0);
  CHECK(kp_rgb_persist_encode_selector("removed", &selected) == 0);
  stage(SELECTOR_A_PATH "state", &selected, sizeof(selected));
  stage(SELECTOR_B_PATH "state", &selected, sizeof(selected));
  CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
#if DT_NODE_EXISTS(DT_NODELABEL(optout))
  CHECK(DT_PROP(DT_NODELABEL(selector_a), initial_effect) == 2);
  CHECK(DT_PROP(DT_NODELABEL(selector_b), initial_effect) == 1);
  CHECK(device_is_ready(DEV(probe_a)) && device_is_ready(DEV(probe_b)));
#endif
  CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == live.h);
  CHECK(kp_rgb_effect_period(DEV(probe_a)) == 1234);
  CHECK(kp_rgb_effect_data(DEV(probe_d))->color.h == 180);
  CHECK(kp_rgb_effect_period(DEV(probe_d)) == CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
  CHECK(kp_rgb_save_selector_state(DEV(probe_a), true) == -ENODEV);
  k_sleep(K_MSEC(100)); CHECK(saves == 0);
}

static void old_custom_ids(void) {
  const struct record old_a = *find(ROOT "selectors/a/state");
  const struct record old_b = *find(ROOT "selectors/b/state");
  const struct record global = *find(ROOT "state");
  unsigned before_reads = reads, before_saves = saves;
  stage(old_a.name, old_a.bytes, old_a.len);
  stage(old_b.name, old_b.bytes, old_b.len);
  CHECK(reads == before_reads);
  CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
  check_restored();

  const char *old_members[] = {"alt", "shared", "shared_012345678901234567890123"};
  for (size_t i = 0; i < ARRAY_SIZE(old_members); i++) {
    CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_c)) == 0);
    CHECK(zmk_rgb_selector_select(DEV(selector_b), DEV(probe_d)) == 0);
    struct kp_rgb_persist_selector_record selected;
    CHECK(kp_rgb_persist_encode_selector(old_members[i], &selected) == 0);
    before_reads = reads;
    stage(SELECTOR_A_PATH "state", &selected, sizeof(selected));
    stage(SELECTOR_B_PATH "state", &selected, sizeof(selected));
    CHECK(reads == before_reads + 2);
    CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
    bool on; CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && on);
    CHECK(kp_rgb_global.brightness == 61);
    check_defaults();
  }
  k_sleep(K_MSEC(100));
  CHECK(saves == before_saves);
  const struct record *unchanged[] = {&old_a, &old_b, &global};
  for (size_t i = 0; i < ARRAY_SIZE(unchanged); i++) {
    const struct record *current = find(unchanged[i]->name);
    CHECK(current->len == unchanged[i]->len);
    CHECK(!memcmp(current->bytes, unchanged[i]->bytes, current->len));
  }
  durable_selection(SELECTOR_A_PATH "state", A_ALT_NAME);
  durable_selection(SELECTOR_B_PATH "state", B_ALT_NAME);
}

static void normalized_names_case(void) {
  CHECK(!strcmp(SELECTOR_A_PATH, ROOT "selectors/selector_a_1/"));
  CHECK(!strcmp(SELECTOR_B_PATH, ROOT "selectors/selector_b_2/"));
  CHECK(!strcmp(A_ALT_NAME, "a_motion_3"));
  CHECK(!strcmp(B_ALT_NAME, "b_alt_4"));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_c));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_d));
  durable_selection(SELECTOR_A_PATH "state", A_ALT_NAME);
  durable_selection(SELECTOR_B_PATH "state", B_ALT_NAME);

  struct record old_a = *find(ROOT "selectors/selector-a@1/state");
  struct record old_b = *find(ROOT "selectors/selector-b@2/state");
  unsigned before_reads = reads;
  CHECK(settings_call_set_handler(old_a.name, old_a.len, read_record, &old_a, NULL) == 0);
  CHECK(settings_call_set_handler(old_b.name, old_b.len, read_record, &old_b, NULL) == 0);
  CHECK(reads == before_reads);

  struct kp_rgb_persist_selector_record legacy_member;
  CHECK(kp_rgb_persist_encode_selector("a-motion@3", &legacy_member) == 0);
  stage(SELECTOR_A_PATH "state", &legacy_member, sizeof(legacy_member));
  CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
  select_member(DEV(selector_a), DEV(probe_c));
  k_sleep(K_MSEC(100));
  CHECK(journal_count == 1);
  CHECK(!strcmp(journal[0].path, SELECTOR_A_PATH "state"));
  durable_selection(SELECTOR_A_PATH "state", A_ALT_NAME);
  perturb_and_reload();
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_c));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_d));
  CHECK(find(old_a.name)->len == old_a.len &&
        !memcmp(find(old_a.name)->bytes, old_a.bytes, old_a.len));
  CHECK(find(old_b.name)->len == old_b.len &&
        !memcmp(find(old_b.name)->bytes, old_b.bytes, old_b.len));
}

static void extended_case(void) {
  if (!strcmp(settings_case, "normalized-names")) {
    normalized_names_case();
  } else if (!strcmp(settings_case, "old-custom-ids")) {
    old_custom_ids();
  } else if (!strcmp(settings_case, "runtime-parameters")) {
    runtime_parameters();
  } else if (!strcmp(settings_case, "reordering")) {
    CHECK(settings_storage);
    obsolete_parameters();
#if DT_NODE_EXISTS(DT_NODELABEL(optout))
    CHECK(kp_rgb_effect_data(DEV(optout))->color.h == 0);
#endif
    select_member(DEV(selector_a), DEV(probe_a));
    select_member(DEV(selector_b), DEV(probe_b));
    k_sleep(K_MSEC(100));
    durable_selection(SELECTOR_A_PATH "state", A_STATIC_NAME);
    durable_selection(SELECTOR_B_PATH "state", B_STATIC_NAME);
    perturb_and_reload();
    select_member(DEV(selector_a), DEV(probe_c));
    k_sleep(K_MSEC(100));
    perturb_and_reload();
    FILE *file = fopen(settings_storage, "wb"); CHECK(file);
    CHECK(fwrite(records, sizeof(records), 1, file) == 1);
    CHECK(fclose(file) == 0);
  } else if (!strcmp(settings_case, "validation")) {
    validation_case();
  } else if (!strcmp(settings_case, "debounce")) {
    unsigned before = saves;
    int64_t started = k_uptime_get();
    select_member(DEV(selector_a), DEV(probe_a));
    k_sleep(K_MSEC(10)); CHECK(saves == before);
    select_member(DEV(selector_a), DEV(probe_c));
    k_sleep(K_MSEC(10)); CHECK(saves == before);
    k_sleep(K_MSEC(60));
    CHECK(journal_count == 1 && journal[0].time >= started + 30);
    CHECK(!strcmp(journal[0].path, SELECTOR_A_PATH "state"));
    durable_selection(SELECTOR_A_PATH "state", A_ALT_NAME);
    perturb_and_reload();
  } else if (!strcmp(settings_case, "global-failure")) {
    fail_path = ROOT "state";
    kp_rgb_matrix_lock(); kp_rgb_global.brightness = 72; kp_rgb_matrix_unlock();
    CHECK(kp_rgb_save_state() == 0);
    select_member(DEV(selector_a), DEV(probe_a));
    select_member(DEV(selector_b), DEV(probe_b));
    k_sleep(K_MSEC(2200));
    CHECK(failed && durable_brightness() == 61);
    durable_selection(SELECTOR_A_PATH "state", A_STATIC_NAME);
    durable_selection(SELECTOR_B_PATH "state", B_STATIC_NAME);
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
    struct record before_b = *find(SELECTOR_B_PATH "state");
    CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_a)) == 0);
    hook_action = 2;
    hook_path = SELECTOR_A_PATH "state";
    fail_once_path = hook_path;
    fail_once_attempt = 2;
    select_member(DEV(selector_a), DEV(probe_c));
    k_sleep(K_MSEC(100));
    CHECK(hook_calls == 1 && fail_once_seen == 1 && !failed);
    durable_selection(SELECTOR_A_PATH "state", A_ALT_NAME);
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
    check_defaults();
    k_sleep(K_MSEC(1100));
    CHECK(failed && fail_once_seen == 2);
    durable_selection(SELECTOR_A_PATH "state", A_ALT_NAME);
    k_sleep(K_MSEC(1100));
    CHECK(fail_once_seen == 3);
    durable_selection(SELECTOR_A_PATH "state", A_STATIC_NAME);
    unsigned attempts = 0; int64_t last = -1;
    for (unsigned i = 0; i < journal_count; i++) {
      CHECK(!strcmp(journal[i].path, fail_once_path));
      CHECK(last < 0 || journal[i].time - last >= 1000);
      last = journal[i].time;
      CHECK(journal[i].result == (attempts == 1 ? -EIO : 0));
      check_selection_bytes(journal[i].bytes, journal[i].len, attempts == 0 ? A_ALT_NAME : A_STATIC_NAME);
      attempts++;
    }
    CHECK(attempts == 3);
    const struct record *unchanged[] = {&before_global, &before_b};
    for (size_t i = 0; i < ARRAY_SIZE(unchanged); i++) {
      struct record *current = find(unchanged[i]->name);
      CHECK(current->len == unchanged[i]->len);
      CHECK(!memcmp(current->bytes, unchanged[i]->bytes, current->len));
    }
    unsigned settled = saves; k_sleep(K_MSEC(1100)); CHECK(saves == settled);
    perturb_and_reload();
  } else if (!strcmp(settings_case, "staged-reset")) {
    struct kp_rgb_persist_global_state global = {.user_on = false, .brightness = 33};
    struct kp_rgb_persist_global_record global_record;
    CHECK(kp_rgb_persist_encode_global(&global, &global_record) == 0);
    stage(ROOT "state", &global_record, sizeof(global_record));
    struct kp_rgb_persist_selector_record selection;
    CHECK(kp_rgb_persist_encode_selector(A_ALT_NAME, &selection) == 0);
    stage(SELECTOR_A_PATH "state", &selection, sizeof(selection));
    CHECK(kp_rgb_persist_encode_selector(B_STATIC_NAME, &selection) == 0);
    stage(SELECTOR_B_PATH "state", &selection, sizeof(selection));
    struct kp_rgb_hsb live = {.h = 267, .s = 61, .b = 27};
    CHECK(zmk_rgb_matrix_set_color(DEV(probe_b), &live) == 0);
    CHECK(zmk_rgb_matrix_set_period(DEV(probe_b), 2345) == 0);
    struct kp_rgb_hsb changed_a = {.h = 313, .s = 22, .b = 38};
    CHECK(zmk_rgb_matrix_set_color(DEV(probe_a), &changed_a) == 0);
    CHECK(zmk_rgb_matrix_set_color(DEV(probe_c), &changed_a) == 0);
    CHECK(zmk_rgb_matrix_set_period(DEV(probe_a), 2777) == 0);
    CHECK(zmk_rgb_matrix_set_period(DEV(probe_c), 3777) == 0);
    obsolete_parameters();
    CHECK(kp_rgb_selector_binding_apply(DEV(selector_a), RGB_RESET_CMD, 0) == 0);
    CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
    CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == 120);
    CHECK(kp_rgb_effect_data(DEV(probe_c))->color.h == 240);
    CHECK(kp_rgb_effect_data(DEV(probe_a))->color.s == 100);
    CHECK(kp_rgb_effect_data(DEV(probe_c))->color.s == 100);
    CHECK(kp_rgb_effect_data(DEV(probe_a))->color.b == 100);
    CHECK(kp_rgb_effect_data(DEV(probe_c))->color.b == 100);
    CHECK(kp_rgb_effect_period(DEV(probe_a)) == 1000);
    CHECK(kp_rgb_effect_period(DEV(probe_c)) == 1000);
#if DT_NODE_EXISTS(DT_NODELABEL(optout))
    CHECK(kp_rgb_effect_data(DEV(optout))->color.h == 0);
#endif
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
    CHECK(kp_rgb_effect_data(DEV(probe_b))->color.h == 267);
    CHECK(kp_rgb_effect_data(DEV(probe_b))->color.b == 27);
    CHECK(kp_rgb_effect_period(DEV(probe_b)) == 2345);
    bool on; CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && !on);
    CHECK(kp_rgb_global.brightness == 33);
    CHECK(kp_rgb_save_state() == 0);
    CHECK(kp_rgb_save_selector_state(DEV(selector_b), false) == 0);
    k_sleep(K_MSEC(100)); perturb_and_reload();
    CHECK(kp_rgb_persist_encode_selector(A_ALT_NAME, &selection) == 0);
    stage(SELECTOR_A_PATH "state", &selection, sizeof(selection));
    CHECK(kp_rgb_persist_encode_selector(B_ALT_NAME, &selection) == 0);
    stage(SELECTOR_B_PATH "state", &selection, sizeof(selection));
    stage(ROOT "state", &global_record, sizeof(global_record));
    obsolete_parameters();
    kp_rgb_reset_state();
    CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
    check_defaults();
    CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && on);
    CHECK(kp_rgb_global.brightness == 100);
    k_sleep(K_MSEC(100)); perturb_and_reload();
  } else {
    bool global = !strncmp(settings_case, "global-", 7);
    bool reset = strstr(settings_case, "reset") != NULL;
    bool failed_save = strstr(settings_case, "failed") != NULL;
    hook_action = global ? (reset ? 4 : 3) : (reset ? 2 : 1);
    hook_path = global ? ROOT "state" : SELECTOR_A_PATH "state";
    if (failed_save) fail_path = hook_path;
    if (global) {
      kp_rgb_matrix_lock(); kp_rgb_global.brightness = 12; kp_rgb_matrix_unlock();
      CHECK(kp_rgb_save_state() == 0);
    } else {
      /* Reset and dirty hooks have distinct final IDs. */
      if (reset) {
        CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_a)) == 0);
        select_member(DEV(selector_a), DEV(probe_c));
      } else select_member(DEV(selector_a), DEV(probe_a));
    }
    k_sleep(K_MSEC(100));
    CHECK(hook_calls == 1 && hook_path == NULL);
    CHECK(journal_count > 0 && journal[0].result == (failed_save ? -EIO : 0));
    if (global) {
      struct kp_rgb_persist_global_state captured;
      CHECK(kp_rgb_persist_decode_global(journal[0].bytes, journal[0].len, &captured) == 0);
      CHECK(captured.brightness == 12);
    } else {
      check_selection_bytes(journal[0].bytes, journal[0].len, reset ? A_ALT_NAME : A_STATIC_NAME);
      durable_selection(SELECTOR_A_PATH "state", failed_save || reset ? A_ALT_NAME : A_STATIC_NAME);
    }
    fail_path = NULL;
    k_sleep(K_MSEC(1100));
    if (global) CHECK(durable_brightness() == (reset ? 100 : 45));
    else durable_selection(SELECTOR_A_PATH "state", reset ? A_STATIC_NAME : A_ALT_NAME);
    int64_t last = -1;
    unsigned attempts = 0;
    for (unsigned i = 0; i < journal_count; i++) {
      const char *path = global ? ROOT "state" : SELECTOR_A_PATH "state";
      if (strcmp(journal[i].path, path)) continue;
      /* Global reset can queue one immediate follow-up during backend I/O. */
      CHECK(last < 0 || (global && reset && attempts == 1) ||
            journal[i].time - last >= 1000);
      last = journal[i].time;
      attempts++;
    }
    CHECK(attempts >= 2);
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
  if (!strcmp(settings_case, "boundary-failure")) {
    k_sleep(K_MSEC(100));
    CHECK(initial_load_done && !boot_checked);
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
#if DT_PROP(DT_NODELABEL(probe_b), zephyr_deferred_init)
    CHECK(!device_is_ready(DEV(probe_b)));
    CHECK(kp_rgb_global.brightness == 100);
    CHECK(settings_load_subtree("keypaw/rgb_matrix") == -ENODEV);
    CHECK(kp_rgb_global.brightness == 100);
    CHECK(device_init(DEV(probe_b)) == 0);
#endif
#if DT_PROP(DT_NODELABEL(selector_a), zephyr_deferred_init)
    CHECK(!device_is_ready(DEV(selector_a)));
    CHECK(kp_rgb_global.brightness == 100);
    CHECK(settings_load_subtree("keypaw/rgb_matrix") == -ENODEV);
    CHECK(kp_rgb_global.brightness == 100);
    CHECK(device_init(DEV(selector_a)) == 0);
#endif
    k_sleep(K_MSEC(200));
    CHECK(!boot_checked);
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
    CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
    CHECK(kp_rgb_global.brightness == 61);
    bool on;
    CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && on);
    kp_rgb_matrix_lock();
    for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++) {
      const struct kp_rgb_selector *selector = kp_rgb_selector_registry_at(i);
      if (selector->owner == DEV(selector_a)) CHECK(kp_rgb_selector_selected(selector) == DEV(probe_c));
      if (selector->owner == DEV(selector_b)) CHECK(kp_rgb_selector_selected(selector) == DEV(probe_d));
    }
    kp_rgb_matrix_unlock();
    check_defaults();
    k_sleep(K_MSEC(200));
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
    CHECK(!boot_checked);
    printk("PASS settings boundary-failure\n");
    exit(0);
  }
  for (unsigned i = 0; i < 100 && !boot_checked; i++) k_sleep(K_MSEC(5));
  CHECK(boot_checked);
  check_restored();
  if (strcmp(settings_case, "existing")) extended_case();
  obsolete_parameters();
  uint8_t legacy_state[36] = {2};
  struct record legacy_record = {.len = sizeof(legacy_state)};
  strcpy(legacy_record.name, ROOT "state");
  memcpy(legacy_record.bytes, legacy_state, sizeof(legacy_state));
  unsigned before_reads = reads;
  /* Zephyr logs and discards h_set errors; rejection must not read or stage. */
  CHECK(settings_call_set_handler(ROOT "state", legacy_record.len, read_record,
                                  &legacy_record, NULL) == 0);
  CHECK(reads == before_reads);
  CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
  check_restored();
  k_sleep(K_MSEC(150));
  CHECK(!failed && saves == 0);
  CHECK(find(ROOT "state")->len == sizeof(struct kp_rgb_persist_global_record));
  CHECK(find(ROOT "state/effects/alt")->len == sizeof(uint8_t[36]));
  printk("PASS scoped settings legacy records ignored\n");
  k_sleep(K_MSEC(1200)); CHECK(saves == 0);

  struct record before_b = *find(SELECTOR_B_PATH "state");
  struct record before_global = *find(ROOT "state");
  CHECK(kp_rgb_selector_binding_apply(DEV(selector_b), RGB_SPI_CMD, 4200) == 0);
  k_sleep(K_MSEC(100)); CHECK(saves == 0);
  CHECK(kp_rgb_selector_binding_apply(DEV(selector_a), RGB_RESET_CMD, 0) == 0);
  k_sleep(K_MSEC(100));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_d));
  CHECK(kp_rgb_effect_period(DEV(probe_d)) == 4200);
  CHECK(!memcmp(find(ROOT "state")->bytes, before_global.bytes, before_global.len));
  CHECK(!memcmp(find(SELECTOR_B_PATH "state")->bytes, before_b.bytes, before_b.len));
  durable_selection(SELECTOR_A_PATH "state", A_STATIC_NAME);
  perturb_and_reload();
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_d));
  /* Reload keeps the changed live parameters, not the pre-reload values. */
  CHECK(kp_rgb_effect_period(DEV(probe_d)) == 1700);
  printk("PASS scoped settings addressed save/reset/reload isolation\n");

  fail_path = SELECTOR_A_PATH "state";
  failed = false;
  select_member(DEV(selector_a), DEV(probe_c));
  select_member(DEV(selector_b), DEV(probe_b));
  kp_rgb_matrix_lock(); kp_rgb_global.brightness = 47; kp_rgb_matrix_unlock();
  CHECK(kp_rgb_save_state() == 0);
  k_sleep(K_MSEC(1100)); CHECK(failed);
  CHECK(durable_brightness() == 47);
  durable_selection(SELECTOR_A_PATH "state", A_STATIC_NAME);
  durable_selection(SELECTOR_B_PATH "state", B_STATIC_NAME);
  unsigned failures = 0; int64_t last = -1;
  for (unsigned i = 0; i < journal_count; i++) if (journal[i].result < 0) {
    CHECK(!strcmp(journal[i].path, fail_path));
    CHECK(last < 0 || journal[i].time - last >= 1000);
    last = journal[i].time; failures++;
  }
  CHECK(failures >= 2);
  printk("PASS scoped settings independent save despite persistent failure\n");
  fail_path = NULL;
  k_sleep(K_MSEC(1100));
  durable_selection(SELECTOR_A_PATH "state", A_ALT_NAME);
  unsigned settled = saves; k_sleep(K_MSEC(1100)); CHECK(saves == settled);
  kp_rgb_reset_state();
  k_sleep(K_MSEC(100));
  CHECK(find(ROOT "state/effects/alt")->len == sizeof(uint8_t[36]));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
  check_defaults();
  durable_selection(SELECTOR_A_PATH "state", A_STATIC_NAME);
  durable_selection(SELECTOR_B_PATH "state", B_STATIC_NAME);
  CHECK(durable_brightness() == 100);
  perturb_and_reload();
  CHECK(saves > 0 && boot_checked);
  printk("PASS scoped settings global reset/reload\n");
  exit(0);
}
K_THREAD_DEFINE(settings_scenario, 4096, scenario, NULL, NULL, NULL, 5, 0, 0);
#endif
