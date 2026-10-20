/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zmk/behavior.h>
#include <zmk/rgb_matrix.h>
#include "rgb_matrix_internal.h"

BUILD_ASSERT(KP_RGB_DEVICE_INIT_PRIORITY == 99);
BUILD_ASSERT(KP_RGB_OVERLAY_COUNT == DT_CHILD_NUM_STATUS_OKAY(DT_NODELABEL(rgb_matrix)));

#define DEV(label) DEVICE_DT_GET(DT_NODELABEL(label))
#define CHECK(expr) do { if (!(expr)) { printk("FAIL fixed scene line %u\n", __LINE__); exit(2); } } while (0)

static void mark(const char *name) {
  printk("fixed scene phase %s at %lld\n", name, (long long)k_uptime_get());
  k_sleep(K_MSEC(1));
}

static void check_gate(bool expected) {
  kp_rgb_matrix_lock();
  CHECK(((struct kp_rgb_overlay_common_data *)DEV(status_panel)->data)->gate == expected);
  kp_rgb_matrix_unlock();
}

static void toggle_gate(void) {
  struct zmk_behavior_binding binding = {.behavior_dev = DEV(ovl_tog)->name};
  CHECK(zmk_behavior_invoke_binding(&binding, (struct zmk_behavior_binding_event){0}, true) == 0);
  CHECK(zmk_behavior_invoke_binding(&binding, (struct zmk_behavior_binding_event){0}, false) == 0);
  k_sleep(K_MSEC(100));
}

static void scenario(void *a, void *b, void *c) {
  ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
  k_sleep(K_MSEC(100));
  bool on;
  CHECK(zmk_rgb_matrix_get_state(&on) == 0 && on);
  CHECK(device_is_ready(DEV(rgb_matrix)));
  CHECK(!strcmp(DEV(rgb_matrix)->name, "rgb"));
  const struct kp_rgb_hsb color = kp_rgb_effect_data(DEV(status_color))->color;
  CHECK(kp_rgb_effect_cfg(DEV(status_color))->owner == NULL);
  CHECK(zmk_rgb_matrix_set_color(DEV(status_color), &color) == 0);
  CHECK(zmk_rgb_matrix_set_period(DEV(status_color), 1000) == 0);
  kp_rgb_matrix_lock();
  CHECK(kp_rgb_global.brightness == 50);
  CHECK(kp_rgb_config.max_brightness == 75);
  CHECK(kp_rgb_config.idle_brightness == 25);
  kp_rgb_matrix_unlock();
  check_gate(false);
  mark("empty");
  toggle_gate();
  check_gate(true);
  mark("status");
  CHECK(zmk_rgb_matrix_off() == 0);
  k_sleep(K_MSEC(100));
  toggle_gate(); check_gate(false);
  toggle_gate(); check_gate(true);
  mark("off");
  CHECK(zmk_rgb_matrix_set_inhibited(true) == 0);
  CHECK(zmk_rgb_matrix_on() == 0);
  toggle_gate(); check_gate(false);
  toggle_gate(); check_gate(true);
  mark("inhibited");
  CHECK(zmk_rgb_matrix_set_inhibited(false) == 0);
  k_sleep(K_MSEC(100));
  mark("resumed");
  toggle_gate(); check_gate(false);
  mark("empty_again");
  printk("PASS fixed scene selector-free/status-only/no-active/live gates LEDs=%u\n", KP_LED_COUNT);
}

K_THREAD_DEFINE(fixed_scene_scenario, 2048, scenario, NULL, NULL, NULL, 5, 0, 0);
