/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#include "rgb_matrix_internal.h"
#include "rgb_selector_internal.h"
#include "rgb_persist_internal.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/init.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zmk/rgb_selector.h>
#include "kp_rgb_test_mutations.h"
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
       .dest = (void *)&settings_storage, .descript = "Same snapshot across firmware layouts"},
      ARG_TABLE_ENDMARKER};
  native_add_command_line_opts(args);
}
NATIVE_TASK(settings_options, PRE_BOOT_1, 11);
#define DEV(label) DEVICE_DT_GET(DT_NODELABEL(label))
#define CHECK(test) do { if (!(test)) { \
  printk("FAIL scoped settings: %s line %d\n", #test, __LINE__); exit(1); \
} } while (0)
#define ROOT "keypaw/rgb_matrix/"

#if !DT_NODE_EXISTS(DT_NODELABEL(selector_a))
#include "kp_rgb_singleton_settings_test.inc"
#else
#define CAPACITY 128
struct record { uint8_t bytes[CAPACITY]; size_t len; };
struct attempt { struct record record; int64_t time; int result; };
static struct record durable;
static struct record boot_record;
static struct attempt journal[16];
static unsigned reads, saves;
static size_t requested_read;
static int read_mode;
static bool initial_load_done, provider_done;
static unsigned failure_mask;
static unsigned hook_action;
static int64_t hook_requested_at;
static K_SEM_DEFINE(write_entered, 0, 1);
static K_SEM_DEFINE(write_release, 0, 1);
static K_SEM_DEFINE(write_finished, 0, 16);
static K_SEM_DEFINE(hook_finished, 0, 1);

static size_t selector_index(const struct device *owner) {
  for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++)
    if (kp_rgb_selector_registry_at(i)->owner == owner) return i;
  CHECK(false); return 0;
}
static uint16_t member_slot(const struct device *owner, const struct device *member) {
  const struct kp_rgb_selector *s = kp_rgb_selector_registry_at(selector_index(owner));
  for (size_t i = 0; i < s->effect_count; i++)
    if (s->members[i].effect == member) return i;
  CHECK(false); return UINT16_MAX;
}
static uint16_t default_slot(size_t i) {
  const struct kp_rgb_selector *s = kp_rgb_selector_registry_at(i);
  return s->initial_effect < s->effect_count ? s->initial_effect : 0;
}
static uint16_t slot(const struct record *r, size_t i) {
  CHECK(8 + 2 * i <= r->len);
  return r->bytes[6 + 2 * i] | ((uint16_t)r->bytes[7 + 2 * i] << 8);
}
static void put_slot(struct record *r, size_t i, uint16_t value) {
  CHECK(8 + 2 * i <= sizeof(r->bytes));
  r->bytes[6 + 2 * i] = value;
  r->bytes[7 + 2 * i] = value >> 8;
}
/* Independent wire oracle: do not let an encoder/decoder bug cancel itself out. */
static struct record record(unsigned count, bool on, uint8_t brightness) {
  struct record r = {.bytes = {1, on, brightness, 0, count & 255, count >> 8},
                     .len = 6 + 2 * count};
  CHECK(r.len <= sizeof(r.bytes));
  for (size_t i = 0; i < count; i++) put_slot(&r, i, 0);
  return r;
}
static void check_runtime(const struct record *r) {
  unsigned count = r->bytes[4] | ((unsigned)r->bytes[5] << 8);
  bool on;
  CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && on == !!r->bytes[1]);
  CHECK(kp_rgb_global.brightness == r->bytes[2]);
  for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++) {
    const struct kp_rgb_selector *s = kp_rgb_selector_registry_at(i);
    uint16_t expected = i < count ? slot(r, i) : default_slot(i);
    if (expected >= s->effect_count) expected = default_slot(i);
    CHECK(zmk_rgb_selector_get_selected(s->owner) == s->members[expected].effect);
  }
}
static void check_snapshot(const struct record *r, bool on, unsigned brightness,
                           const struct device *a, const struct device *b) {
  size_t count = kp_rgb_selector_registry_count();
  CHECK(r->len == 6 + 2 * count);
  CHECK(r->bytes[0] == 1 && r->bytes[1] == on && r->bytes[2] == brightness);
  CHECK(r->bytes[3] == 0 && (r->bytes[4] | ((size_t)r->bytes[5] << 8)) == count);
  CHECK(slot(r, selector_index(DEV(selector_a))) == member_slot(DEV(selector_a), a));
  CHECK(slot(r, selector_index(DEV(selector_b))) == member_slot(DEV(selector_b), b));
  CHECK(slot(r, selector_index(DEV(preview_selector))) == 0);
}
static ssize_t read_record(void *arg, void *out, size_t len) {
  const struct record *r = arg;
  reads++; requested_read = len;
  CHECK(len <= MIN(r->len, 6 + 2 * kp_rgb_selector_registry_count()));
  if (read_mode < 0) return -EIO;
  len = MIN(len, r->len);
  if (read_mode > 0 && len) len--;
  memcpy(out, r->bytes, len);
  return len;
}
static void stage_path(const char *path, const struct record *r) {
  /* Zephyr logs h_set failures and returns zero; commit state is the oracle. */
  CHECK(settings_call_set_handler(path, r->len, read_record, (void *)r, NULL) == 0);
}
static void stage(const struct record *r) { stage_path(KP_RGB_PERSIST_KEY, r); }
static int load(struct settings_store *store, const struct settings_load_arg *arg) {
  ARG_UNUSED(store);
  struct record loaded = durable;
  if (!initial_load_done) {
    loaded.bytes[2] = 17;
    for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++) put_slot(&loaded, i, 0);
  }
  CHECK(settings_call_set_handler(KP_RGB_PERSIST_KEY, loaded.len, read_record,
                                  &loaded, arg) == 0);
  initial_load_done = true;
  return 0;
}
static int save(struct settings_store *store, const char *name, const char *bytes, size_t len) {
  ARG_UNUSED(store);
  CHECK(!strcmp(name, KP_RGB_PERSIST_KEY));
  CHECK(len == 6 + 2 * kp_rgb_selector_registry_count());
  CHECK(saves < ARRAY_SIZE(journal));
  unsigned index = saves++;
  struct attempt *a = &journal[index];
  a->record.len = len;
  memcpy(a->record.bytes, bytes, len);
  a->time = k_uptime_get();
  a->result = failure_mask & BIT(index) ? -EIO : 0;
  if (hook_action) {
    k_sem_give(&write_entered);
    CHECK(k_sem_take(&write_release, K_MSEC(500)) == 0);
    /* The backend still owns the supplied pointer after concurrent mutations. */
    CHECK(!memcmp(bytes, a->record.bytes, len));
  }
  if (!a->result) durable = a->record;
  k_sem_give(&write_finished);
  return a->result;
}
static const struct settings_store_itf interface = {.csi_load = load, .csi_save = save};
static struct settings_store store = {.cs_itf = &interface};
int settings_backend_init(void) {
  size_t count = kp_rgb_selector_registry_count();
  CHECK(count == 3);
  durable = record(count, true, 61);
  put_slot(&durable, selector_index(DEV(selector_a)),
           member_slot(DEV(selector_a), !strcmp(settings_case, "reordering") ? DEV(probe_a) : DEV(probe_c)));
  put_slot(&durable, selector_index(DEV(selector_b)), member_slot(DEV(selector_b), DEV(probe_d)));
  if (settings_storage) {
    FILE *file = fopen(settings_storage, "rb");
    if (file) {
      durable.len = fread(durable.bytes, 1, sizeof(durable.bytes), file);
      CHECK(!ferror(file) && feof(file)); CHECK(fclose(file) == 0);
      CHECK(durable.len == 12);
    } else {
      CHECK(!DT_NODE_EXISTS(DT_NODELABEL(optout)));
      file = fopen(settings_storage, "wb"); CHECK(file);
      CHECK(fwrite(durable.bytes, 1, durable.len, file) == durable.len);
      CHECK(fclose(file) == 0);
    }
  }
  boot_record = durable;
  settings_src_register(&store); settings_dst_register(&store);
  return 0;
}
static int provider_commit(void) {
  if (provider_done) return 0;
  CHECK(initial_load_done);
  CHECK(kp_rgb_global.brightness == 100);
  for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++)
    CHECK(zmk_rgb_selector_get_selected(kp_rgb_selector_registry_at(i)->owner) == NULL);
  stage(&durable);
  for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++)
    CHECK(zmk_rgb_selector_get_selected(kp_rgb_selector_registry_at(i)->owner) == NULL);
  provider_done = true;
  printk("PASS scoped settings provider before restoration\n");
  return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE_WITH_CPRIO(scoped_settings_provider, "test/rgb_provider",
                                        NULL, NULL, provider_commit, NULL, INT_MAX - 1);
static void select_member(const struct device *owner, const struct device *member) {
  struct zmk_behavior_binding binding = {.behavior_dev = owner->name,
      .param1 = RGB_EFS_CMD, .param2 = member_slot(owner, member)};
  struct zmk_behavior_binding converted = binding;
  CHECK(behavior_keymap_binding_convert_central_state_dependent_params(
      &converted, (struct zmk_behavior_binding_event){0}) == 0);
  CHECK(!strcmp(converted.behavior_dev, owner->name));
  CHECK(zmk_behavior_invoke_binding(&binding, (struct zmk_behavior_binding_event){0}, true) == 0);
  CHECK(zmk_behavior_invoke_binding(&binding, (struct zmk_behavior_binding_event){0}, false) == 0);
}
static void settle(unsigned expected) {
  k_sleep(K_MSEC(80)); CHECK(saves == expected);
}
static void no_retry(unsigned expected) {
  k_sleep(K_MSEC(1150)); CHECK(saves == expected);
}
static void commit(void) { CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0); }
static void legacy(void) {
  const char *paths[] = {ROOT "state", ROOT "state/effects/alt", ROOT "effects/alt",
      ROOT "selectors/a/state", ROOT "selectors/selector_a/state",
      ROOT "selectors/selector_b/state", ROOT "selectors/selector_a/effects/a_motion",
      ROOT "selectors/missing/state", ROOT "unknown", ROOT "snapshot/extra"};
  struct record r = record(3, false, 19);
  unsigned before = reads, writes = saves;
  for (size_t i = 0; i < ARRAY_SIZE(paths); i++) {
    for (int mode = -1; mode <= 1; mode++) {
      read_mode = mode; stage_path(paths[i], &r);
    }
  }
  read_mode = 0;
  CHECK(reads == before && saves == writes);
  commit();
  CHECK(reads == before && saves == writes);
  printk("PASS scoped settings legacy records ignored\n");
}
static void validation(void) {
  size_t count = kp_rgb_selector_registry_count();
  struct record valid = record(count, false, 66);
  put_slot(&valid, selector_index(DEV(selector_a)), member_slot(DEV(selector_a), DEV(probe_a)));
  put_slot(&valid, selector_index(DEV(selector_b)), member_slot(DEV(selector_b), DEV(probe_d)));
  for (unsigned kind = 0; kind < 12; kind++) {
    stage(&valid);
    struct record bad = valid;
    switch (kind) {
    case 0: bad.bytes[0] = 99; break;
    case 1: bad.bytes[1] = 2; break;
    case 2: bad.bytes[2] = 101; break;
    case 3: bad.bytes[3] = 1; break;
    case 4: bad.len = 5; break;
    case 5: bad.len--; break;
    case 6: bad.len++; break;
    case 7: bad.bytes[4] = 255; bad.bytes[5] = 255; break;
    case 8: read_mode = -1; break;
    case 9: read_mode = 1; break;
    case 10: bad.len = 0; break;
    case 11: bad.bytes[4] = 0; bad.bytes[5] = 0; break;
    }
    stage(&bad); read_mode = 0;
    commit(); check_runtime(&valid);
    CHECK(kp_test_select(DEV(selector_a), DEV(probe_c)) == 0);
  }
  for (unsigned n = 0; n <= count + 2; n++) {
    struct record r = record(n, n % 2, 27 + n);
    for (unsigned i = 0; i < n; i++) put_slot(&r, i, 1);
    stage(&r);
    CHECK(requested_read == MIN(r.len, 6 + 2 * count));
    commit(); check_runtime(&r);
  }
  struct record huge = record(count, true, 49);
  huge.bytes[4] = 255; huge.bytes[5] = 255;
  huge.len = 6 + 2 * (size_t)UINT16_MAX;
  stage(&huge);
  CHECK(requested_read == 6 + 2 * count);
  commit(); check_runtime(&huge);
  for (size_t invalid = 0; invalid < count; invalid++) {
    struct record r = record(count, false, 38);
    for (size_t i = 0; i < count; i++) put_slot(&r, i, 1);
    put_slot(&r, invalid, UINT16_MAX);
    stage(&r); commit(); check_runtime(&r);
    put_slot(&r, invalid, kp_rgb_selector_registry_at(invalid)->effect_count);
    stage(&r); commit(); check_runtime(&r);
  }
  struct record bounds = record(count, true, 0);
  stage(&bounds); commit(); check_runtime(&bounds);
  bounds.bytes[2] = 100; stage(&bounds); commit(); check_runtime(&bounds);
  legacy(); no_retry(0);
}
static void parameters_and_reload(void) {
  const struct device *effects[] = {DEV(probe_a), DEV(probe_b), DEV(probe_c), DEV(probe_d)};
  const uint32_t commands[] = {RGB_HUI_CMD, RGB_HUD_CMD, RGB_SAI_CMD, RGB_SAD_CMD,
                              RGB_SPI_CMD, RGB_SPD_CMD, RGB_COLOR_HSB_CMD};
  for (size_t i = 0; i < ARRAY_SIZE(commands); i++) {
    CHECK(kp_rgb_selector_binding_apply(DEV(selector_a), commands[i],
        commands[i] == RGB_COLOR_HSB_CMD ? RGB_COLOR_HSB_VAL(211, 37, 43) : 0) == 0);
  }
  CHECK(kp_rgb_selector_binding_apply(DEV(selector_a),
      KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD,
          member_slot(DEV(selector_a), DEV(probe_a))), RGB_COLOR_HSB_VAL(288, 55, 29)) == 0);
  CHECK(kp_rgb_selector_binding_apply(DEV(selector_b),
      KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD,
          member_slot(DEV(selector_b), DEV(probe_b))), 3888) == 0);
  CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == 288);
  CHECK(kp_rgb_effect_period(DEV(probe_b)) == 3888);
  for (size_t i = 0; i < ARRAY_SIZE(effects); i++) {
    struct kp_rgb_hsb color = {.h = 211 + i, .s = 37, .b = 43};
    CHECK(kp_test_set_color(effects[i], &color) == 0);
    CHECK(kp_test_set_period(effects[i], 2300 + i) == 0);
  }
  CHECK(kp_test_select(DEV(selector_a), DEV(probe_a)) == 0);
  CHECK(kp_test_select(DEV(selector_b), DEV(probe_b)) == 0);
  CHECK(zmk_rgb_matrix_off() == 0);
  no_retry(0);
  for (unsigned pass = 0; pass < 4; pass++) {
    CHECK((pass % 2 ? settings_load_subtree("keypaw/rgb_matrix") : settings_load()) == 0);
    check_runtime(&durable);
    CHECK((pass % 2 ? settings_commit_subtree("keypaw/rgb_matrix") : settings_commit()) == 0);
    for (size_t i = 0; i < ARRAY_SIZE(effects); i++) {
      const struct kp_rgb_hsb *color = &kp_rgb_effect_data(effects[i])->color;
      CHECK(color->h == 211 + i && color->s == 37 && color->b == 43);
      CHECK(kp_rgb_effect_period(effects[i]) == 2300 + i);
    }
  }
  settle(0);
  CHECK(kp_test_select(DEV(selector_b), DEV(probe_b)) == 0);
  select_member(DEV(selector_a), DEV(probe_c)); /* Successful no-op still saves B. */
  settle(1);
  check_snapshot(&durable, true, 61, DEV(probe_c), DEV(probe_b));
  CHECK(settings_load() == 0);
  for (size_t i = 0; i < ARRAY_SIZE(effects); i++) {
    CHECK(kp_rgb_effect_data(effects[i])->color.h == 211 + i);
    CHECK(kp_rgb_effect_period(effects[i]) == 2300 + i);
  }
  no_retry(1);
}
static void debounce(void) {
  select_member(DEV(selector_a), DEV(probe_c));
  k_sleep(K_MSEC(12)); CHECK(saves == 0);
  select_member(DEV(selector_a), DEV(probe_c));
  int64_t last = k_uptime_get();
  k_sleep(K_MSEC(12)); CHECK(saves == 0); /* Past the first request's wake. */
  settle(1);
  CHECK(journal[0].time >= last + CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE);
  check_snapshot(&durable, true, 61, DEV(probe_c), DEV(probe_d));
  no_retry(1);
}
static void failure(void) {
  struct record old = durable;
  failure_mask = BIT(0);
  struct kp_rgb_hsb color = {.h = 279, .s = 39, .b = 17};
  CHECK(kp_test_set_color(DEV(probe_a), &color) == 0);
  CHECK(kp_test_set_period(DEV(probe_a), 2789) == 0);
  CHECK(kp_test_select(DEV(selector_b), DEV(probe_b)) == 0);
  select_member(DEV(selector_a), DEV(probe_a));
  settle(1); CHECK(journal[0].result == -EIO);
  CHECK(durable.len == old.len && !memcmp(durable.bytes, old.bytes, old.len));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
  CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == 279);
  CHECK(kp_rgb_effect_data(DEV(probe_a))->color.s == 39);
  CHECK(kp_rgb_effect_data(DEV(probe_a))->color.b == 17);
  CHECK(kp_rgb_effect_period(DEV(probe_a)) == 2789);
  no_retry(1);
  select_member(DEV(selector_a), DEV(probe_a));
  settle(2);
  check_snapshot(&durable, true, 61, DEV(probe_a), DEV(probe_b));
  no_retry(2);
}
static void storage_hook(void *a, void *b, void *c) {
  ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
  for (;;) {
    k_sem_take(&write_entered, K_FOREVER);
    unsigned action = hook_action;
    hook_action = 0;
    bool on;
    CHECK(zmk_rgb_matrix_get_user_state(&on) == 0);
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) ==
          (action == 1 ? DEV(probe_a) : DEV(probe_c)));
    hook_requested_at = k_uptime_get();
    if (action == 1) select_member(DEV(selector_a), DEV(probe_c));
    else if (action == 2) CHECK(kp_rgb_selector_binding_apply(DEV(selector_a), RGB_RESET_CMD, 0) == 0);
    else if (action == 3) kp_rgb_reset_state();
    else CHECK(false);
    /* Release before the new debounce deadline; completion must preserve it. */
    k_sem_give(&write_release);
    k_sem_give(&hook_finished);
  }
}
K_THREAD_DEFINE(storage_query_thread, 2048, storage_hook, NULL, NULL, NULL, 5, 0, 0);
static void concurrent_save(void) {
  bool reset = strstr(settings_case, "reset") != NULL;
  bool global = strstr(settings_case, "global") != NULL;
  bool fail_first = strstr(settings_case, "failed") != NULL;
  bool fail_second = strstr(settings_case, "second-failure") != NULL;
  struct record old = durable;
  failure_mask = (fail_first ? BIT(0) : 0) | (fail_second ? BIT(1) : 0);
  CHECK(kp_test_select(DEV(selector_b), DEV(probe_b)) == 0);
  hook_action = global ? 3 : reset ? 2 : 1;
  select_member(DEV(selector_a), reset ? DEV(probe_c) : DEV(probe_a));
  CHECK(k_sem_take(&hook_finished, K_MSEC(500)) == 0);
  CHECK(k_sem_take(&write_finished, K_MSEC(500)) == 0);
  CHECK(k_sem_take(&write_finished, K_MSEC(500)) == 0);
  CHECK(saves == 2);
  CHECK(journal[0].result == (fail_first ? -EIO : 0));
  CHECK(journal[1].result == (fail_second ? -EIO : 0));
  check_snapshot(&journal[0].record, true, 61,
                 reset ? DEV(probe_c) : DEV(probe_a), DEV(probe_b));
  check_snapshot(&journal[1].record, true, global ? 100 : 61,
                 reset ? DEV(probe_a) : DEV(probe_c), DEV(probe_b));
  if (!global) CHECK(journal[1].time >= hook_requested_at + CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE);
  else CHECK(journal[1].time - hook_requested_at < CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE);
  const struct record *expected = fail_second ? (fail_first ? &old : &journal[0].record) : &journal[1].record;
  CHECK(durable.len == expected->len && !memcmp(durable.bytes, expected->bytes, expected->len));
  check_runtime(&journal[1].record);
  no_retry(2);
  select_member(DEV(selector_a), reset ? DEV(probe_a) : DEV(probe_c));
  settle(3); check_runtime(&durable); no_retry(3);
}
static void staged_reset(void) {
  struct record r = record(kp_rgb_selector_registry_count(), false, 33);
  put_slot(&r, selector_index(DEV(selector_a)), member_slot(DEV(selector_a), DEV(probe_c)));
  put_slot(&r, selector_index(DEV(selector_b)), member_slot(DEV(selector_b), DEV(probe_d)));
  struct kp_rgb_hsb color = {.h = 267, .s = 61, .b = 27};
  CHECK(kp_test_set_color(DEV(probe_b), &color) == 0);
  CHECK(kp_test_set_period(DEV(probe_b), 2345) == 0);
  CHECK(kp_test_set_color(DEV(probe_a), &color) == 0);
  stage(&r);
  CHECK(kp_rgb_selector_binding_apply(DEV(selector_a), RGB_RESET_CMD, 0) == 0);
  commit();
  put_slot(&r, selector_index(DEV(selector_a)), default_slot(selector_index(DEV(selector_a))));
  check_runtime(&r);
  CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h == 120);
  CHECK(kp_rgb_effect_data(DEV(probe_b))->color.h == 267);
  CHECK(kp_rgb_effect_period(DEV(probe_b)) == 2345);
  settle(1);
  stage(&r); kp_rgb_reset_state(); commit();
  struct record defaults = record(kp_rgb_selector_registry_count(), true, 100);
  for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++) put_slot(&defaults, i, default_slot(i));
  check_runtime(&defaults);
  CHECK(kp_rgb_effect_data(DEV(probe_b))->color.h == 60);
  settle(2); CHECK(settings_load() == 0); check_runtime(&defaults);
}
static void reordering(void) {
  CHECK(settings_storage);
  CHECK(boot_record.len == 12 && boot_record.bytes[1] == 1 && boot_record.bytes[2] == 61);
  /* Original: preview=0, A=0, B=1. The exact same bytes mean B=0, A=1 after reorder. */
  CHECK(slot(&boot_record, 0) == 0 && slot(&boot_record, 1) == 0 && slot(&boot_record, 2) == 1);
#if DT_NODE_EXISTS(DT_NODELABEL(optout))
  CHECK(selector_index(DEV(selector_b)) == 1 && selector_index(DEV(selector_a)) == 2);
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_c));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_d));
  bool on = true;
  for (unsigned i = 0; i < 100 && on; i++) {
    CHECK(zmk_rgb_matrix_get_user_state(&on) == 0); k_sleep(K_MSEC(2));
  }
  CHECK(!on); settle(1);
  CHECK(durable.bytes[1] == 0 && durable.bytes[2] == 61);
#else
  CHECK(selector_index(DEV(selector_a)) == 1 && selector_index(DEV(selector_b)) == 2);
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_d));
  bool on; CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && on); settle(0);
#endif
  printk("PASS settings positional boot action\n");
}
static void scenario(void *a, void *b, void *c) {
  ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
  for (unsigned i = 0; i < 100 && !zmk_rgb_selector_get_selected(DEV(selector_a)); i++) k_sleep(K_MSEC(5));
  CHECK(provider_done && zmk_rgb_selector_get_selected(DEV(selector_a)) != NULL);
  if (!strcmp(settings_case, "reordering")) reordering();
  else {
    check_runtime(&durable);
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_c));
    CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_d));
    CHECK(zmk_behavior_get_binding(DEV(selector_a)->name) == DEV(selector_a));
    CHECK(zmk_behavior_get_binding(DEV(ordinary)->name) == NULL);
    for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++)
      CHECK(kp_rgb_selector_registry_at(i)->owner != DEV(ordinary));
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
    printk("PASS scoped settings restored at global boundary\n");
    if (!strcmp(settings_case, "validation")) validation();
    else if (!strcmp(settings_case, "existing")) { legacy(); parameters_and_reload(); }
    else if (!strcmp(settings_case, "runtime-parameters") || !strcmp(settings_case, "repeated-commits")) parameters_and_reload();
    else if (!strcmp(settings_case, "debounce")) debounce();
    else if (!strcmp(settings_case, "global-failure")) failure();
    else if (!strcmp(settings_case, "staged-reset")) staged_reset();
    else concurrent_save();
  }
  printk("PASS settings %s\n", settings_case); exit(0);
}
K_THREAD_DEFINE(settings_test_thread, 4096, scenario, NULL, NULL, NULL, 10, 0, 50);
#endif
