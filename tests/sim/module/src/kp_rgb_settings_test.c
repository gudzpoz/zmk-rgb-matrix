/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include "rgb_matrix_internal.h"
#include <stdlib.h>
#include <string.h>
#include <zephyr/init.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>
#include <zmk/events/rgb_selector_selection_changed.h>
#include <zmk/rgb_selector.h>

#define DEV(label) DEVICE_DT_GET(DT_NODELABEL(label))
#define CHECK(test)                                                            \
  do {                                                                         \
    if (!(test)) {                                                             \
      printk("FAIL scoped settings: %s line %d\n", #test, __LINE__);           \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
#define ROOT "keypaw/rgb_matrix/"

struct record {
  char name[SETTINGS_MAX_NAME_LEN + 1];
  uint8_t bytes[sizeof(struct kp_rgb_persist_blob)];
  size_t len;
};
static struct record records[24];
static const char *fail_path;
static bool failed;
static bool initial_load_done;
static unsigned saves;

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
  len = MIN(len, record->len);
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
  if (fail_path != NULL && !strcmp(name, fail_path)) {
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
  struct kp_rgb_persist_blob legacy;
  kp_rgb_persist_pack(true, 61, "shared", &legacy);
  seed(ROOT "state", &legacy, sizeof(legacy));
  struct kp_rgb_persist_selector_record selection;
  CHECK(kp_rgb_persist_encode_selector("alt", &selection) == 0);
  seed(ROOT "selectors/a/state", &selection, sizeof(selection));
  seed(ROOT "selectors/b/state", &selection, sizeof(selection));
  struct kp_rgb_persist_effect value = {.duration_ms = 2100, .h = 42, .s = 70};
  seed(ROOT "state/effects/shared", &value, sizeof(value));
  struct kp_rgb_persist_parameters_record parameters;
  value.h = 123;
  CHECK(kp_rgb_persist_encode_parameters(&value, &parameters) == 0);
  seed(ROOT "selectors/a/effects/shared", &parameters, sizeof(parameters));
  value.h = 240;
  value.duration_ms = 3100;
  CHECK(kp_rgb_persist_encode_parameters(&value, &parameters) == 0);
  seed(ROOT "selectors/b/effects/shared", &parameters, sizeof(parameters));
  value.h = 88;
  seed(ROOT "state/effects/alt", &value, sizeof(value));
  fail_path = ROOT "selectors/a/effects/alt";
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
  CHECK(kp_rgb_effect_data(DEV(probe_c))->color.h == 88);
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

static void scenario(void *a, void *b, void *c) {
  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);
  k_sleep(K_MSEC(100));
#if !IS_ENABLED(CONFIG_KEYPAW_RGB_CONTROL_AUTO_START)
  CHECK(initial_load_done);
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
  CHECK(settings_load_subtree("keypaw/rgb_matrix") == 0);
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
  k_sleep(K_MSEC(150));
  CHECK(failed &&
        find(ROOT "state")->len == sizeof(struct kp_rgb_persist_blob));
  CHECK(find(ROOT "state/effects/alt")->len != 0);
  printk("PASS scoped settings failed migration retains legacy\n");
  fail_path = NULL;
  k_sleep(K_MSEC(1200));
  CHECK(find(ROOT "state")->len == sizeof(struct kp_rgb_persist_global_record));
  CHECK(find(ROOT "selectors/a/effects/alt")->len ==
        sizeof(struct kp_rgb_persist_parameters_record));
  CHECK(find(ROOT "state/effects/alt")->len == 0);
  printk("PASS scoped settings migration retry\n");

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
  CHECK(settings_load_subtree("keypaw/rgb_matrix") == 0);
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_d));
  CHECK(kp_rgb_effect_period(DEV(probe_d)) == 4200);
  printk("PASS scoped settings addressed save/reset/reload isolation\n");

  struct kp_rgb_persist_effect removed = {
      .duration_ms = 2000, .h = 70, .s = 50};
  seed(ROOT "state/effects/removed", &removed, sizeof(removed));
  kp_rgb_reset_state();
  k_sleep(K_MSEC(100));
  CHECK(find(ROOT "state/effects/removed")->len == 0);
  CHECK(settings_load_subtree("keypaw/rgb_matrix") == 0);
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
  CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == 120);
  CHECK(kp_rgb_effect_data(DEV(probe_b))->color.h == 60);
  CHECK(saves > 0 && boot_checked);
  printk("PASS scoped settings global reset/reload\n");
}
K_THREAD_DEFINE(settings_scenario, 4096, scenario, NULL, NULL, NULL, 5, 0, 0);
