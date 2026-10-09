/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT keypaw_rgb_matrix_selector_probe

#include <errno.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zmk/rgb_matrix.h>
#include <zmk/events/position_state_changed.h>
#include "rgb_matrix_internal.h"

struct probe_config {
  struct kp_rgb_effect_common_config common;
  bool animate;
};
struct probe_data {
  struct kp_rgb_effect_common_data common;
  unsigned renders, events, resets, activations, deactivations;
  bool active, restart;
};

#define CHECK(test) do { \
  if (!(test)) { \
    printk("FAIL selector: %s line %d\n", #test, __LINE__); \
    exit(1); \
  } \
} while (0)

static void reset(const struct device *dev, int64_t now) {
  ARG_UNUSED(now);
  struct probe_data *data = dev->data;
  data->resets++;
  data->restart = true;
  printk("selector child %s reset\n", dev->name);
}

static void active(const struct device *dev, bool value, int64_t now) {
  ARG_UNUSED(now);
  struct probe_data *data = dev->data;
  data->active = value;
  if (value) { data->activations++; data->restart = true; }
  else data->deactivations++;
  printk("selector child %s active=%d\n", dev->name, value);
}

static bool event(const struct device *dev, const struct kp_rgb_key_event *ev) {
  ARG_UNUSED(ev);
  struct probe_data *data = dev->data;
  CHECK(data->active);
  data->events++;
  printk("selector child %s input before renders=%u\n", dev->name, data->renders);
  return true;
}

static bool render(const struct device *dev, const struct kp_rgb_frame *frame) {
  struct probe_data *data = dev->data;
  const struct probe_config *cfg = dev->config;
  CHECK(data->active && frame->scratch == NULL);
  if (data->restart) { CHECK(frame->elapsed_ms == 0); data->restart = false; }
  data->renders++;
  for (size_t i = 0; i < frame->target_count; i++) {
    size_t led = frame->targets[i];
    CHECK(frame->pixels[led].r == 0 && frame->pixels[led].g == 0 && frame->pixels[led].b == 0);
    frame->pixels[led] = kp_rgb_hsb_to_rgb(data->common.color);
  }
  printk("selector child %s render elapsed=%u demand=%d\n", dev->name,
         (unsigned)frame->elapsed_ms, cfg->animate);
  return cfg->animate;
}

static const struct kp_rgb_effect_callbacks callbacks = {
  .render = render, .reset = reset, .set_active = active, .on_event = event,
};

#define PROBE_DEFINE(inst) \
  static const struct probe_config probe_##inst##_cfg = { \
    KP_RGB_EFFECT_COMMON_CONFIG(inst), .animate = DT_INST_PROP(inst, animate), \
  }; \
  static struct probe_data probe_##inst##_data = { KP_RGB_EFFECT_COMMON_DATA(inst, 0, 0) }; \
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), &callbacks, probe_##inst)
DT_INST_FOREACH_STATUS_OKAY(PROBE_DEFINE)

#define DEV(label) DEVICE_DT_GET(DT_NODELABEL(label))
#define DATA(label) ((struct probe_data *)DEV(label)->data)

static void mark(const char *name) {
  printk("selector phase %s at %lld\n", name, (long long)k_uptime_get());
}

static unsigned callback_count(void) {
  struct probe_data *children[] = {
    DATA(probe_a), DATA(probe_b), DATA(probe_c), DATA(probe_d),
  };
  unsigned count = 0;
  for (size_t i = 0; i < ARRAY_SIZE(children); i++) {
    const struct probe_data *data = children[i];
    count += data->renders + data->events + data->resets +
             data->activations + data->deactivations;
  }
  return count;
}

static void select_slot(const struct device *dev, unsigned slot, int expected) {
  kp_rgb_matrix_lock();
  unsigned before = callback_count();
  CHECK(kp_rgb_selector_overlay_select_locked(dev, slot) == expected);
  CHECK(before == callback_count());
  kp_rgb_matrix_unlock();
}

static void scenario(void *a, void *b, void *c) {
  ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
  k_sleep(K_MSEC(100));
  kp_rgb_matrix_lock();
  CHECK(DATA(probe_a)->renders > 0 && DATA(probe_b)->renders > 0);
  CHECK(DATA(probe_c)->renders == 0 && DATA(probe_d)->renders == 0);
  CHECK(kp_rgb_effect_period(DEV(probe_a)) == 1000);
  CHECK(kp_rgb_effect_period(DEV(probe_b)) == CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
  CHECK(kp_rgb_effect_period(DEV(probe_d)) == CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
  CHECK(kp_rgb_effect_cfg(DEV(probe_a))->owner == DEV(selector_a));
  CHECK(kp_rgb_effect_cfg(DEV(probe_a))->slot == 1);
  CHECK(kp_rgb_effect_cfg(DEV(probe_c))->slot == 2);
  CHECK(kp_rgb_effect_cfg(DEV(probe_b))->owner == DEV(selector_b));
  unsigned settled = DATA(probe_a)->renders;
  kp_rgb_matrix_unlock();
  mark("initial");
  k_sleep(K_MSEC(200));
  kp_rgb_matrix_lock(); CHECK(DATA(probe_a)->renders == settled); kp_rgb_matrix_unlock();
  select_slot(DEV(selector_a), 0, -ENOENT);
  select_slot(DEV(selector_a), 99, -EINVAL);
  select_slot(DEV(selector_a), 1, 0);

  kp_rgb_matrix_lock();
  CHECK(kp_rgb_selector_overlay_select_locked(DEV(selector_a), 2) == 1);
  CHECK(DATA(probe_c)->renders == 0 && DATA(probe_c)->activations == 0);
  raise_zmk_position_state_changed((struct zmk_position_state_changed){
    .source = 0, .position = 0, .state = true, .timestamp = k_uptime_get(),
  });
  kp_rgb_matrix_unlock();
  k_sleep(K_MSEC(100));
  kp_rgb_matrix_lock();
  CHECK(DATA(probe_c)->events == 1 && DATA(probe_c)->renders > 1);
  CHECK(!DATA(probe_a)->active && DATA(probe_b)->active);
  kp_rgb_matrix_unlock();
  mark("animated");
  k_sleep(K_MSEC(200));

  CHECK(zmk_rgb_matrix_select_effect(1) == 0);
  k_sleep(K_MSEC(100));
  select_slot(DEV(selector_a), 1, 1);
  select_slot(DEV(selector_b), 1, 1);
  mark("hidden");
  k_sleep(K_MSEC(200));
  kp_rgb_matrix_lock(); CHECK(!DATA(probe_a)->active && !DATA(probe_d)->active); kp_rgb_matrix_unlock();
  CHECK(zmk_rgb_matrix_select_effect(0) == 0);
  k_sleep(K_MSEC(100));
  mark("shown");
  k_sleep(K_MSEC(200));

  CHECK(zmk_rgb_matrix_off() == 0);
  k_sleep(K_MSEC(100));
  select_slot(DEV(selector_a), 2, 1);
  select_slot(DEV(selector_b), 0, 1);
  mark("off");
  k_sleep(K_MSEC(200));
  kp_rgb_matrix_lock(); CHECK(!DATA(probe_c)->active && !DATA(probe_b)->active); kp_rgb_matrix_unlock();
  CHECK(zmk_rgb_matrix_on() == 0);
  k_sleep(K_MSEC(100));
  mark("on");
  k_sleep(K_MSEC(200));

  CHECK(zmk_rgb_matrix_set_inhibited(true) == 0);
  k_sleep(K_MSEC(100));
  kp_rgb_matrix_lock(); unsigned resets = DATA(probe_c)->resets; kp_rgb_matrix_unlock();
  mark("inhibited");
  k_sleep(K_MSEC(200));
  CHECK(zmk_rgb_matrix_set_inhibited(false) == 0);
  k_sleep(K_MSEC(100));
  kp_rgb_matrix_lock(); CHECK(DATA(probe_c)->resets > resets && DATA(probe_c)->active); kp_rgb_matrix_unlock();
  mark("resumed");
  k_sleep(K_MSEC(200));
  kp_rgb_matrix_lock();
  unsigned inactive_resets = DATA(probe_a)->resets;
  resets = DATA(probe_c)->resets;
  kp_rgb_request_runtime_reset_locked();
  kp_rgb_matrix_unlock();
  k_sleep(K_MSEC(100));
  kp_rgb_matrix_lock();
  CHECK(DATA(probe_c)->resets > resets && DATA(probe_c)->active);
  CHECK(DATA(probe_a)->resets == inactive_resets);
  kp_rgb_matrix_unlock();
  select_slot(DEV(selector_a), 1, 1);
  k_sleep(K_MSEC(100));
  kp_rgb_matrix_lock(); CHECK(DATA(probe_a)->resets > inactive_resets); kp_rgb_matrix_unlock();
  kp_rgb_matrix_lock(); settled = DATA(probe_a)->renders; kp_rgb_matrix_unlock();
  k_sleep(K_MSEC(200));
  kp_rgb_matrix_lock(); CHECK(DATA(probe_a)->renders == settled); kp_rgb_matrix_unlock();
  printk("PASS selector runtime\n");
}

K_THREAD_DEFINE(selector_scenario, 4096, scenario, NULL, NULL, NULL, 5, 0, 0);
