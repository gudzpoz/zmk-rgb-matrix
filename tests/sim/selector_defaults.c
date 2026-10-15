/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#define DT_DRV_COMPAT test_rgb_default_effect
#include <errno.h>
#include <stdlib.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/settings/settings.h>
#include <zmk/rgb_matrix.h>
#include <zmk/rgb_selector.h>

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

static int before_preparation(void) {
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
SYS_INIT(before_preparation, APPLICATION, 80);

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
  CHECK(zmk_rgb_matrix_start() == -ENODEV);
  check_parameters(DEV(default_ready), early_color, 4321);
  CHECK(device_init(DEV(default_late)) == 0);
  check_parameters(DEV(default_late), (struct kp_rgb_hsb){60, 100, 100},
                   CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS);
  CHECK(zmk_rgb_matrix_set_color(DEV(default_late), &late_color) == 0);
  CHECK(zmk_rgb_matrix_set_period(DEV(default_late), 3456) == 0);
  CHECK(zmk_rgb_matrix_start() == 0);
  CHECK(zmk_rgb_matrix_start() == 0);
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
  printk("PASS selector defaults ready-before-prepare/late-ready setters/reset/start retry\n");
  exit(0);
}
K_THREAD_DEFINE(defaults_thread, 4096, defaults_test, NULL, NULL, NULL, 5, 0, 1);
