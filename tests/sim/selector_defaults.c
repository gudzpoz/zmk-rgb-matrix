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
#define DEFAULTS_DEFINE(inst) \
  static const struct defaults_config defaults_##inst##_cfg = {KP_RGB_EFFECT_COMMON_CONFIG(inst)}; \
  static struct defaults_data defaults_##inst##_data = {KP_RGB_EFFECT_COMMON_DATA(inst, 0, 0)}; \
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), &callbacks, defaults_##inst);
DT_INST_FOREACH_STATUS_OKAY(DEFAULTS_DEFINE)

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
  CHECK(selector->effect_count == 3);
  CHECK(selector->members[0].effect == DEV(default_ready));
  CHECK(strcmp(selector->members[0].id, "ready") == 0);
  CHECK(selector->members[1].effect == NULL && selector->members[1].id == NULL);
  CHECK(selector->members[2].effect == DEV(default_explicit));
  CHECK(strcmp(selector->members[2].id, "explicit") == 0);

  selector = find_selector("default_late_selector");
  CHECK(selector != NULL && selector->effect_count == 1);
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
  CHECK(kp_rgb_selector_count(selector) == 3);
  CHECK(kp_rgb_selector_at(selector, 1) == NULL);
  CHECK(kp_rgb_selector_calc_index(selector, 0, 1) == 2);
  CHECK(kp_rgb_selector_calc_index(selector, 2, -1) == 0);
  CHECK(kp_rgb_selector_select_locked(selector, 1) == -ENOENT);
  CHECK(kp_rgb_selector_select_locked(selector, 2) == 1);
  CHECK(kp_rgb_selector_select_locked(selector, 0) == 1);
  kp_rgb_matrix_unlock();
}

static const struct kp_rgb_hsb early_color = {.h = 137, .s = 61, .b = 100};
static const struct kp_rgb_hsb late_color = {.h = 271, .s = 42, .b = 100};

static void check_parameters(const struct device *effect,
                             struct kp_rgb_hsb color, uint16_t period) {
  const struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(effect);
  printk("defaults %s: %u/%u/%u period %u expected %u/%u/%u period %u\n",
         effect->name, data->color.h, data->color.s, data->color.b, data->duration_ms,
         color.h, color.s, color.b, period);
  CHECK(data->color.h == color.h && data->color.s == color.s && data->color.b == color.b);
  CHECK(data->duration_ms == period);
}

static int before_startup(void) {
  check_static_metadata();
  CHECK(device_is_ready(DEV(default_ready)));
  check_parameters(DEV(default_ready), (struct kp_rgb_hsb){120, 100, 100}, 2345);
  check_parameters(DEV(default_explicit), (struct kp_rgb_hsb){240, 100, 100},
                   CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
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
  kp_rgb_matrix_lock();
  CHECK(kp_rgb_selector_at(find_selector("default_late_selector"), 0) == DEV(default_late));
  kp_rgb_matrix_unlock();
  check_parameters(DEV(default_late), (struct kp_rgb_hsb){60, 100, 100},
                   CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS);
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
  CHECK(zmk_rgb_selector_get_selected(DEV(default_selector)) == DEV(default_ready));
  CHECK(zmk_rgb_selector_get_selected(DEV(default_late_selector)) == DEV(default_late));
  CHECK(zmk_rgb_selector_reset(DEV(default_selector)) == 0);
  check_parameters(DEV(default_ready), (struct kp_rgb_hsb){120, 100, 100}, 2345);
  check_parameters(DEV(default_explicit), (struct kp_rgb_hsb){240, 100, 100},
                   CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
  check_parameters(DEV(default_late), late_color, 3456);
  CHECK(zmk_rgb_selector_reset(DEV(default_late_selector)) == 0);
  check_parameters(DEV(default_late), (struct kp_rgb_hsb){60, 100, 100},
                   CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS);
  printk("PASS selector defaults shared metadata/disabled slots/live readiness/setters/reset/automatic startup\n");
  exit(0);
}
K_THREAD_DEFINE(defaults_thread, 4096, defaults_test, NULL, NULL, NULL, 5, 0, 1);
