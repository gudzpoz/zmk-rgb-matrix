/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#define DT_DRV_COMPAT test_rgb_default_effect
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/settings/settings.h>
#include <zmk/rgb_matrix.h>
#include <zmk/rgb_selector.h>
#include "../../src/rgb_matrix_internal.h"
#include "../../src/rgb_selector_internal.h"

struct defaults_config { struct kp_rgb_effect_common_config common; };
struct defaults_data { struct kp_rgb_effect_common_data common; };
static bool defaults_render(const struct device *dev, const struct kp_rgb_frame *frame) {
  for (size_t i = 0; i < frame->target_count; i++)
    frame->pixels[frame->targets[i]] = kp_rgb_hsb_to_rgb(kp_rgb_effect_data(dev)->color);
  return false;
}
static const struct kp_rgb_effect_callbacks callbacks = {.render = defaults_render};
#define DEFAULTS_DEFAULTS(inst) KP_RGB_EFFECT_DEFAULTS(inst, 0, 0)
#define DEFAULTS_DEFINE(inst) \
  static const struct defaults_config defaults_##inst##_cfg = {KP_RGB_EFFECT_COMMON_CONFIG(inst, DEFAULTS_DEFAULTS(inst))}; \
  static struct defaults_data defaults_##inst##_data = {KP_RGB_EFFECT_COMMON_DATA(DEFAULTS_DEFAULTS(inst))}; \
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), &callbacks, defaults_##inst);
DT_INST_FOREACH_STATUS_OKAY(DEFAULTS_DEFINE)

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT test_rgb_missing_defaults
/* Model signed C macro arguments independently of DTS cell encoding. */
#define MISSING_DEFAULTS(inst) KP_RGB_EFFECT_DEFAULTS(inst, 0x008080, (int32_t)DT_INST_PROP(inst, test_period))
#define MISSING_DEFINE(inst) \
  static const struct defaults_config missing_##inst##_cfg = {KP_RGB_EFFECT_COMMON_CONFIG(inst, MISSING_DEFAULTS(inst))}; \
  static struct defaults_data missing_##inst##_data = {KP_RGB_EFFECT_COMMON_DATA(MISSING_DEFAULTS(inst))}; \
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), &callbacks, missing_##inst);
DT_INST_FOREACH_STATUS_OKAY(MISSING_DEFINE)

#define DEV(label) DEVICE_DT_GET(DT_NODELABEL(label))
#define CHECK(test) do { \
  if (!(test)) { \
    printk("FAIL selector defaults: %s line %d\n", #test, __LINE__); \
    exit(1); \
  } \
} while (0)

static struct kp_rgb_selector *find_selector(const char *id) {
  for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++) {
    struct kp_rgb_selector *selector = kp_rgb_selector_registry_at(i);
    if (selector != NULL && selector->id != NULL && strcmp(selector->id, id) == 0)
      return selector;
  }
  return NULL;
}

static void check_static_metadata(void) {
  CHECK(kp_rgb_selector_registry_at(kp_rgb_selector_registry_count()) == NULL);
  CHECK(find_selector("default_disabled") == NULL);
  CHECK(zmk_rgb_selector_get_selected(DEV(default_selector)) == NULL);
  struct kp_rgb_selector *selector = find_selector("default_selector");
  CHECK(selector != NULL);
  CHECK(selector->control == DEVICE_DT_GET(DT_PATH(behaviors, defctl)));
  CHECK(selector->effect_count == 5);
  CHECK(selector->members[0].effect == DEV(default_ready));
  CHECK(strcmp(selector->members[0].id, "ready") == 0);
  CHECK(selector->members[1].effect == NULL && selector->members[1].id == NULL);
  CHECK(selector->members[2].effect == DEV(default_explicit));
  CHECK(strcmp(selector->members[2].id, "explicit") == 0);
  CHECK(selector->members[3].effect == DEV(default_breathe));
  CHECK(selector->members[4].effect == DEV(missing_selected));
  CHECK(kp_rgb_effect_cfg(DEV(default_ready))->defaults.duration_ms == 1000);
  CHECK(kp_rgb_effect_cfg(DEV(default_ready))->defaults.color.h == 120);
  CHECK(kp_rgb_effect_cfg(DEV(default_ready))->defaults.color.s == 100);
  CHECK(kp_rgb_effect_cfg(DEV(default_ready))->defaults.color.b == 50);
  CHECK(kp_rgb_effect_cfg(DEV(default_explicit))->defaults.duration_ms == 10000);
  CHECK(kp_rgb_effect_cfg(DEV(default_breathe))->defaults.duration_ms == 2400);
  CHECK(kp_rgb_effect_cfg(DEV(default_breathe))->defaults.color.h == 240);
  CHECK(kp_rgb_effect_cfg(DEV(default_breathe))->defaults.color.s == 100);
  CHECK(kp_rgb_effect_cfg(DEV(default_breathe))->defaults.color.b == 100);
  CHECK(kp_rgb_effect_cfg(DEV(missing_selected))->defaults.duration_ms == 3210);
  CHECK(kp_rgb_effect_cfg(DEV(missing_selected))->defaults.color.h == 180);
  CHECK(kp_rgb_effect_cfg(DEV(missing_selected))->defaults.color.s == 100);
  CHECK(kp_rgb_effect_cfg(DEV(missing_selected))->defaults.color.b == 50);

  selector = find_selector("default_late_selector");
  CHECK(selector != NULL && selector->effect_count == 3);
  CHECK(selector->control == DEVICE_DT_GET(DT_PATH(behaviors, latectl)));
  CHECK(selector->members[0].effect == DEV(default_late));
  CHECK(strcmp(selector->members[0].id, "late") == 0);
  kp_rgb_matrix_lock();
  CHECK(kp_rgb_selector_at(selector, 0) == NULL);
  kp_rgb_matrix_unlock();
}

static void check_hole_selection(void) {
  struct kp_rgb_selector *selector = find_selector("default_selector");
  CHECK(selector != NULL);
  kp_rgb_matrix_lock();
  CHECK(selector->effect_count == 5);
  CHECK(kp_rgb_selector_at(selector, 1) == NULL);
  CHECK(kp_rgb_selector_calc_index(selector, 0, 1) == 2);
  CHECK(kp_rgb_selector_calc_index(selector, 2, -1) == 0);
  CHECK(kp_rgb_selector_select_locked(selector, 1) == -ENOENT);
  CHECK(kp_rgb_selector_select_locked(selector, 2) == 1);
  CHECK(kp_rgb_selector_select_locked(selector, 0) == 1);
  kp_rgb_matrix_unlock();
}

static const struct kp_rgb_hsb early_color = {.h = 137, .s = 61, .b = 37};
static const struct kp_rgb_hsb late_color = {.h = 271, .s = 42, .b = 73};

static void check_defaults_match_data(const struct device *effect) {
  const struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(effect);
  const struct kp_rgb_effect_common_data *defaults = &kp_rgb_effect_cfg(effect)->defaults;
  CHECK(data->color.h == defaults->color.h && data->color.s == defaults->color.s &&
        data->color.b == defaults->color.b && data->duration_ms == defaults->duration_ms);
}

static void check_parameters(const struct device *effect,
                             struct kp_rgb_hsb color, uint16_t period) {
  const struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(effect);
  printk("defaults %s: %u/%u/%u period %u expected %u/%u/%u period %u\n",
         effect->name, data->color.h, data->color.s, data->color.b, data->duration_ms,
         color.h, color.s, color.b, period);
  CHECK(data->color.h == color.h && data->color.s == color.s && data->color.b == color.b);
  CHECK(data->duration_ms == period);
}

struct period_case {
  const struct device *effect;
  const struct device *selector;
  uint16_t startup;
  uint16_t reset;
};
#define PARENT_CASE(name, start, reset_) \
  {DEV(parent_##name), DEV(boundary_parent_##name), start, reset_}
#define CHILD_CASE(name, value) \
  {DEV(child_##name), DEV(child_boundaries), value, value}
#define PRIVATE_CASE(name, value) {DEV(private_##name), NULL, value, value}
static const struct period_case period_cases[] = {
  PARENT_CASE(below, 1000, 1000),
  PARENT_CASE(min, 1000, 1000),
  PARENT_CASE(max, 1000, 1000),
  PARENT_CASE(above, 1000, 1000),
  PARENT_CASE(negative, 1000, 1000),
  PARENT_CASE(zero, 1000, 1000),
  PARENT_CASE(omitted, 1000, 1000),
  CHILD_CASE(below, 100), CHILD_CASE(min, 100),
  CHILD_CASE(max, 10000), CHILD_CASE(above, 10000),
  CHILD_CASE(negative, 10000), CHILD_CASE(zero, 1000),
  CHILD_CASE(omitted, 1000),
  PRIVATE_CASE(below, 100), PRIVATE_CASE(min, 100),
  PRIVATE_CASE(max, 10000), PRIVATE_CASE(above, 10000),
  PRIVATE_CASE(negative, 10000), PRIVATE_CASE(zero, 1000),
  PRIVATE_CASE(omitted, 1000),
  {DEV(disabled_below), NULL, 100, 100},
  {DEV(disabled_min), NULL, 100, 100},
  {DEV(disabled_max), NULL, 10000, 10000},
  {DEV(disabled_above), NULL, 10000, 10000},
  {DEV(disabled_negative), NULL, 10000, 10000},
  {DEV(disabled_zero), NULL, 1000, 1000},
  {DEV(disabled_omitted), NULL, 1000, 1000},
};

static void check_period_cases(bool reset) {
  for (size_t i = 0; i < ARRAY_SIZE(period_cases); i++) {
    const struct period_case *test = &period_cases[i];
    CHECK(device_is_ready(test->effect));
    check_parameters(test->effect, (struct kp_rgb_hsb){0, 0, 100},
                     reset ? test->reset : test->startup);
  }
}

static void check_period_helper(void) {
  struct kp_rgb_effect_common_data data = {0};
  const struct device fabricated = {.data = &data};
  static const struct { uint16_t raw; uint32_t expected; } cases[] = {
      {0, 1000}, {1, 100}, {99, 100}, {100, 100},
      {10000, 10000}, {10001, 10000}, {65535, 10000},
  };
  for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
    data.duration_ms = cases[i].raw;
    CHECK(kp_rgb_effect_period(&fabricated) == cases[i].expected);
    CHECK(data.duration_ms == cases[i].raw);
  }
}

static int before_startup(void) {
  check_period_cases(false);
  check_period_helper();
  check_parameters(DEV(missing_private), (struct kp_rgb_hsb){180, 100, 50}, 3210);
  check_parameters(DEV(missing_disabled), (struct kp_rgb_hsb){180, 100, 50}, 3210);
  check_parameters(DEV(missing_private_wide), (struct kp_rgb_hsb){180, 100, 50}, 10000);
  check_parameters(DEV(missing_private_negative), (struct kp_rgb_hsb){180, 100, 50}, CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
  check_parameters(DEV(missing_private_minus_one), (struct kp_rgb_hsb){180, 100, 50}, 10000);
  CHECK(!device_is_ready(DEV(missing_selected)));
  check_parameters(DEV(missing_selected), (struct kp_rgb_hsb){180, 100, 50}, 3210);
  CHECK(!device_is_ready(DEV(default_late)));
  check_parameters(DEV(default_late), (struct kp_rgb_hsb){60, 100, 50}, 1000);
  CHECK(zmk_rgb_matrix_set_color(DEV(missing_selected), &early_color) == -ENODEV);
  CHECK(zmk_rgb_matrix_set_period(DEV(missing_selected), 4321) == -ENODEV);
  CHECK(zmk_rgb_matrix_set_period(DEV(default_late), 4321) == -ENODEV);
  check_parameters(DEV(missing_selected), (struct kp_rgb_hsb){180, 100, 50}, 3210);
  check_parameters(DEV(default_late), (struct kp_rgb_hsb){60, 100, 50}, 1000);
  check_static_metadata();
  CHECK(device_is_ready(DEV(default_ready)));
  check_parameters(DEV(default_ready), (struct kp_rgb_hsb){120, 100, 50}, 1000);
  check_defaults_match_data(DEV(default_ready));
  check_parameters(DEV(default_explicit), (struct kp_rgb_hsb){240, 100, 50},
                   CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
  check_defaults_match_data(DEV(default_explicit));
  check_parameters(DEV(default_breathe), (struct kp_rgb_hsb){240, 100, 100}, 2400);
  check_defaults_match_data(DEV(default_breathe));
  check_parameters(DEV(default_black), (struct kp_rgb_hsb){0, 0, 0}, 1000);
  check_parameters(DEV(default_white), (struct kp_rgb_hsb){0, 0, 100}, 1000);
  check_parameters(DEV(default_private), (struct kp_rgb_hsb){0, 100, 50}, 1000);
  check_parameters(DEV(default_disabled_child), (struct kp_rgb_hsb){0, 100, 50}, 1000);
  CHECK(zmk_rgb_matrix_set_color(DEV(default_ready), &early_color) == 0);
  CHECK(zmk_rgb_matrix_set_period(DEV(default_ready), 4321) == 0);
  CHECK(!device_is_ready(DEV(default_late)));
  CHECK(zmk_rgb_matrix_set_color(DEV(default_late), &late_color) == -ENODEV);
  return 0;
}
SYS_INIT(before_startup, APPLICATION, 80);

static int prepare_deferred_effect(void) {
  CHECK(zmk_rgb_selector_get_selected(DEV(default_selector)) == NULL);
  CHECK(zmk_rgb_selector_get_selected(DEV(default_late_selector)) == NULL);
  CHECK(!device_is_ready(DEV(default_late)));
  CHECK(device_init(DEV(default_late)) == 0);
  CHECK(!device_is_ready(DEV(missing_selected)));
  check_parameters(DEV(missing_selected), (struct kp_rgb_hsb){180, 100, 50}, 3210);
  CHECK(device_init(DEV(missing_selected)) == 0);
  CHECK(device_is_ready(DEV(missing_selected)));
  check_parameters(DEV(missing_selected), (struct kp_rgb_hsb){180, 100, 50}, 3210);
  CHECK(zmk_rgb_matrix_set_color(DEV(missing_selected), &early_color) == 0);
  CHECK(zmk_rgb_matrix_set_period(DEV(missing_selected), 4321) == 0);
  kp_rgb_matrix_lock();
  CHECK(kp_rgb_selector_at(find_selector("default_late_selector"), 0) == DEV(default_late));
  kp_rgb_matrix_unlock();
  check_parameters(DEV(default_late), (struct kp_rgb_hsb){60, 100, 50}, 1000);
  CHECK(zmk_rgb_matrix_set_color(DEV(default_late), &late_color) == 0);
  CHECK(zmk_rgb_matrix_set_period(DEV(default_late), 3456) == 0);
  return 0;
}
SYS_INIT(prepare_deferred_effect, APPLICATION, 98);

#if IS_ENABLED(CONFIG_SETTINGS)
int settings_backend_init(void) { return 0; }

static unsigned parameter_reads;
static ssize_t read_obsolete_parameter(void *arg, void *data, size_t len) {
  ARG_UNUSED(arg); ARG_UNUSED(data); ARG_UNUSED(len);
  parameter_reads++;
  return -EIO;
}
static void stage_obsolete_parameters(void) {
  const char *paths[] = {
      "keypaw/rgb_matrix/selectors/default_selector/effects/ready",
      "keypaw/rgb_matrix/selectors/default_late_selector/effects/late",
  };
  for (size_t i = 0; i < ARRAY_SIZE(paths); i++)
    CHECK(settings_call_set_handler(paths[i], 6, read_obsolete_parameter, NULL, NULL) == 0);
  CHECK(parameter_reads == 0);
}
#endif

static void defaults_test(void *a, void *b, void *c) {
  ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
  check_parameters(DEV(default_ready), early_color, 4321);
#if IS_ENABLED(CONFIG_SETTINGS)
  stage_obsolete_parameters();
#endif
  int64_t deadline = k_uptime_get() + 1000;
  while (zmk_rgb_selector_get_selected(DEV(default_selector)) == NULL ||
         zmk_rgb_selector_get_selected(DEV(default_late_selector)) == NULL) {
    CHECK(k_uptime_get() < deadline);
    k_sleep(K_MSEC(1));
  }
  check_parameters(DEV(default_ready), early_color, 4321);
  check_hole_selection();
#if IS_ENABLED(CONFIG_SETTINGS)
  stage_obsolete_parameters();
  CHECK(settings_commit_subtree("keypaw/rgb_matrix") == 0);
#endif
  check_parameters(DEV(default_ready), early_color, 4321);
  check_parameters(DEV(default_late), late_color, 3456);
  check_period_cases(false);
  check_parameters(DEV(missing_selected), early_color, 4321);
  CHECK(zmk_rgb_matrix_set_period(DEV(default_ready), 0) == -EINVAL);
  CHECK(zmk_rgb_matrix_set_period(DEV(default_ready), 99) == -EINVAL);
  CHECK(zmk_rgb_matrix_set_period(DEV(default_ready), 10001) == -EINVAL);
  check_parameters(DEV(default_ready), early_color, 4321);
  CHECK(zmk_rgb_selector_get_selected(DEV(default_selector)) == DEV(default_ready));
  CHECK(zmk_rgb_selector_get_selected(DEV(default_late_selector)) == DEV(default_late));
  CHECK(zmk_rgb_matrix_set_color(DEV(default_breathe), &early_color) == 0);
  CHECK(zmk_rgb_matrix_set_period(DEV(default_breathe), 4321) == 0);
  CHECK(zmk_rgb_selector_reset(DEV(default_selector)) == 0);
  check_parameters(DEV(default_ready), (struct kp_rgb_hsb){120, 100, 50}, 1000);
  check_defaults_match_data(DEV(default_ready));
  check_parameters(DEV(default_explicit), (struct kp_rgb_hsb){240, 100, 50},
                   CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
  check_defaults_match_data(DEV(default_explicit));
  check_parameters(DEV(default_breathe), (struct kp_rgb_hsb){240, 100, 100}, 2400);
  check_defaults_match_data(DEV(default_breathe));
  check_parameters(DEV(default_late), late_color, 3456);
  check_parameters(DEV(missing_selected), (struct kp_rgb_hsb){180, 100, 50}, 3210);
  check_defaults_match_data(DEV(missing_selected));
  CHECK(zmk_rgb_matrix_set_color(DEV(default_black), &early_color) == 0);
  CHECK(zmk_rgb_matrix_set_color(DEV(default_white), &late_color) == 0);
  CHECK(zmk_rgb_matrix_set_period(DEV(default_black), 4567) == 0);
  CHECK(zmk_rgb_matrix_set_period(DEV(default_white), 5678) == 0);
  CHECK(zmk_rgb_selector_reset(DEV(default_late_selector)) == 0);
  check_parameters(DEV(default_late), (struct kp_rgb_hsb){60, 100, 50}, 1000);
  check_parameters(DEV(default_black), (struct kp_rgb_hsb){0, 0, 0}, 1000);
  check_parameters(DEV(default_white), (struct kp_rgb_hsb){0, 0, 100}, 1000);
  CHECK(zmk_rgb_matrix_set_color(DEV(default_ready), &early_color) == 0);
  CHECK(zmk_rgb_matrix_set_color(DEV(default_late), &late_color) == 0);
  CHECK(zmk_rgb_matrix_set_color(DEV(default_black), &early_color) == 0);
  CHECK(zmk_rgb_matrix_set_color(DEV(default_white), &late_color) == 0);
  CHECK(zmk_rgb_matrix_set_period(DEV(default_ready), 4321) == 0);
  CHECK(zmk_rgb_matrix_set_period(DEV(default_late), 3456) == 0);
  for (size_t i = 0; i < ARRAY_SIZE(period_cases); i++) {
    const struct period_case *test = &period_cases[i];
    if (test->selector != NULL) {
      CHECK(zmk_rgb_matrix_set_period(test->effect, 4321) == 0);
      CHECK(zmk_rgb_matrix_set_color(test->effect, &early_color) == 0);
    }
  }
  for (size_t i = 0; i < ARRAY_SIZE(period_cases); i++) {
    if (period_cases[i].selector != NULL)
      CHECK(zmk_rgb_selector_reset(period_cases[i].selector) == 0);
  }
  check_period_cases(true);
  CHECK(zmk_rgb_matrix_set_color(DEV(default_breathe), &late_color) == 0);
  CHECK(zmk_rgb_matrix_set_period(DEV(default_breathe), 5678) == 0);
  kp_rgb_reset_state();
  check_parameters(DEV(default_ready), (struct kp_rgb_hsb){120, 100, 50}, 1000);
  check_defaults_match_data(DEV(default_ready));
  check_parameters(DEV(default_breathe), (struct kp_rgb_hsb){240, 100, 100}, 2400);
  check_defaults_match_data(DEV(default_breathe));
  check_parameters(DEV(default_late), (struct kp_rgb_hsb){60, 100, 50}, 1000);
  check_parameters(DEV(default_black), (struct kp_rgb_hsb){0, 0, 0}, 1000);
  check_parameters(DEV(default_white), (struct kp_rgb_hsb){0, 0, 100}, 1000);
  check_period_cases(true);
  check_parameters(DEV(missing_selected), (struct kp_rgb_hsb){180, 100, 50}, 3210);
  check_defaults_match_data(DEV(missing_selected));
  check_parameters(DEV(missing_private), (struct kp_rgb_hsb){180, 100, 50}, 3210);
  check_parameters(DEV(missing_disabled), (struct kp_rgb_hsb){180, 100, 50}, 3210);
  printk("PASS selector defaults shared metadata/disabled slots/live readiness/setters/reset/automatic startup\n");
  exit(0);
}
K_THREAD_DEFINE(defaults_thread, 4096, defaults_test, NULL, NULL, NULL, 5, 0, 1);
