/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT keypaw_rgb_matrix_selector_probe

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zmk/rgb_matrix.h>
#include <zmk/rgb_selector.h>
#include <zmk/events/rgb_selector_selection_changed.h>
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
  CHECK(zmk_rgb_selector_get_selected(cfg->common.owner) == dev);
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

static atomic_t selection_events_a, selection_events_b;

static int selection_listener(const zmk_event_t *eh) {
  const struct zmk_rgb_selector_selection_changed *changed =
      as_zmk_rgb_selector_selection_changed(eh);
  CHECK(changed != NULL);
  CHECK(zmk_rgb_selector_get_selected(changed->selector) != NULL);
  if (changed->selector == DEV(selector_a))
    atomic_inc(&selection_events_a);
  else if (changed->selector == DEV(selector_b))
    atomic_inc(&selection_events_b);
  else
    CHECK(false);
  return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(selector_public_probe, selection_listener);
ZMK_SUBSCRIPTION(selector_public_probe, zmk_rgb_selector_selection_changed);

static unsigned callback_count(void);

static void public_selection(void) {
  CHECK(zmk_rgb_selector_get_selected(NULL) == NULL);
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
  CHECK(atomic_get(&selection_events_a) == 1);
  CHECK(atomic_get(&selection_events_b) == 1);
  CHECK(zmk_rgb_selector_select(DEV(selector_a), NULL) == -EINVAL);
  CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_b)) == -EINVAL);
  CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(private_white)) == -EINVAL);
  CHECK(zmk_rgb_selector_select(NULL, DEV(probe_a)) == -ENODEV);
  CHECK(zmk_rgb_selector_reset(NULL) == -ENODEV);
  CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_a)) == 0);
  k_sleep(K_MSEC(50));
  CHECK(atomic_get(&selection_events_a) == 1);
  kp_rgb_matrix_lock();
  unsigned before = callback_count();
  CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_c)) == 0);
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_c));
  CHECK(zmk_rgb_selector_reset(DEV(selector_a)) == 0);
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
  CHECK(before == callback_count());
  kp_rgb_matrix_unlock();
  k_sleep(K_MSEC(50));
  CHECK(atomic_get(&selection_events_a) == 2);
  CHECK(atomic_get(&selection_events_b) == 1);
  printk("PASS selector public API/notifications/coalescing\n");
}

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

static struct zmk_behavior_binding binding(const struct device *dev, uint32_t cmd, uint32_t arg) {
  return (struct zmk_behavior_binding){.behavior_dev = dev->name, .param1 = cmd, .param2 = arg};
}

static void invoke(const struct device *dev, uint32_t cmd, uint32_t arg) {
  struct zmk_behavior_binding value = binding(dev, cmd, arg);
  kp_rgb_matrix_lock();
  unsigned before = callback_count();
  CHECK(zmk_behavior_invoke_binding(&value, (struct zmk_behavior_binding_event){0}, true) == 0);
  CHECK(callback_count() == before);
  CHECK(zmk_behavior_invoke_binding(&value, (struct zmk_behavior_binding_event){0}, false) == 0);
  CHECK(callback_count() == before);
  kp_rgb_matrix_unlock();
}

static void converted(const struct device *dev, uint32_t cmd, uint32_t arg,
                      const struct device *target, uint32_t absolute_cmd, uint32_t absolute_arg) {
  struct zmk_behavior_binding value = binding(dev, cmd, arg);
  kp_rgb_matrix_lock();
  unsigned before = callback_count();
  CHECK(behavior_keymap_binding_convert_central_state_dependent_params(
          &value, (struct zmk_behavior_binding_event){0}) == 0);
  CHECK(strcmp(value.behavior_dev, target->name) == 0);
  CHECK(value.param1 == absolute_cmd && value.param2 == absolute_arg);
  CHECK(callback_count() == before);
  kp_rgb_matrix_unlock();
}

K_SEM_DEFINE(isr_checked, 0, 1);

static void isr_boundaries(struct k_timer *unused) {
  ARG_UNUSED(unused);
  CHECK(k_is_in_isr());
  CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
  CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_c)) == -EWOULDBLOCK);
  CHECK(zmk_rgb_selector_reset(DEV(selector_a)) == -EWOULDBLOCK);
  CHECK(zmk_rgb_matrix_select_effect(1) == -EWOULDBLOCK);
  CHECK(zmk_rgb_matrix_cycle_effect(1) == -EWOULDBLOCK);
  CHECK(kp_rgb_selector_binding_apply(DEV(selector_a), RGB_EFS_CMD, 1) == -EWOULDBLOCK);
  struct zmk_behavior_binding value = binding(DEV(rgbs_a), RGB_EFF_CMD, 0);
  CHECK(kp_rgb_selector_binding_convert(DEV(selector_a), &value) == -EWOULDBLOCK);
  k_sem_give(&isr_checked);
}

K_TIMER_DEFINE(boundary_timer, isr_boundaries, NULL);

static void controls(void) {
  kp_rgb_matrix_lock();
  unsigned before = callback_count();
  atomic_val_t a_notifications = atomic_get(&selection_events_a);
  atomic_val_t b_notifications = atomic_get(&selection_events_b);
  struct behavior_parameter_metadata metadata;
  CHECK(behavior_get_parameter_metadata(DEV(rgbs_a), &metadata) == 0);
  CHECK(metadata.sets_len == 4);
  bool reset_found = false;
  for (size_t i = 0; i < metadata.sets_len; i++) {
    for (size_t j = 0; j < metadata.sets[i].param1_values_len; j++) {
      uint32_t cmd = metadata.sets[i].param1_values[j].value;
      CHECK(cmd != RGB_TOG_CMD && cmd != RGB_BRI_CMD && cmd != RGB_OFF_CMD);
      reset_found |= cmd == RGB_RESET_CMD;
    }
  }
  CHECK(reset_found);
  CHECK(metadata.sets[1].param2_values[0].range.max == UINT16_MAX);
  CHECK(behavior_get_parameter_metadata(DEV(probe_a), &metadata) == 0);
  CHECK(metadata.sets_len == 0);
  enum behavior_locality locality;
  CHECK(behavior_get_locality(DEV(rgbs_a), &locality) == 0);
  CHECK(locality == BEHAVIOR_LOCALITY_GLOBAL);
  const struct behavior_driver_api *private_api = DEV(private_white)->api;
  CHECK(private_api->binding_convert_central_state_dependent_params == NULL);
  CHECK(kp_rgb_selector_control(DEV(selector_a)) == DEV(rgbs_a));
  CHECK(kp_rgb_selector_control(DEV(selector_b)) == DEV(rgbs_b));
  CHECK(kp_rgb_selector_alias() == DEV(selector_a));
  CHECK(zmk_rgb_matrix_select_effect(0) == -ENOENT);
  CHECK(zmk_rgb_matrix_select_effect(UINT16_MAX) == -EINVAL);
  CHECK(zmk_rgb_matrix_select_effect(1) == 0);
  CHECK(zmk_rgb_matrix_cycle_effect(-1) == 0);
  CHECK(kp_rgb_selector_overlay_state_locked(DEV(selector_a))->active_fx == DEV(probe_c));
  CHECK(zmk_rgb_matrix_cycle_effect(1) == 0);
  CHECK(kp_rgb_selector_overlay_state_locked(DEV(selector_b))->active_fx == DEV(probe_b));
  struct zmk_behavior_binding invalid = binding(DEV(rgbs_a), RGB_OFF_CMD, 0);
  CHECK(behavior_keymap_binding_convert_central_state_dependent_params(
          &invalid, (struct zmk_behavior_binding_event){0}) == -ENOTSUP);
  invalid = binding(DEV(rgbs_a), RGB_COLOR_HSB_CMD, RGB_COLOR_HSB_VAL(361, 100, 100));
  CHECK(behavior_keymap_binding_convert_central_state_dependent_params(
          &invalid, (struct zmk_behavior_binding_event){0}) == -EINVAL);
  CHECK(before == callback_count());
  converted(DEV(fx_solid_v), 0, 0, DEV(kprgb), RGB_LEGACY_EFS_CMD, 1);
  invoke(DEV(fx_solid_v), 0, 0);
  CHECK(kp_rgb_controller.selector.effect_index == 1);
  CHECK(kp_rgb_selector_overlay_state_locked(DEV(selector_a))->active_fx == DEV(probe_a));
  invoke(DEV(fx_solid), 0, 0);
  CHECK(kp_rgb_controller.selector.effect_index == 0);
  kp_rgb_matrix_unlock();
  converted(DEV(rgbs_a), RGB_EFF_CMD, 0, DEV(rgbs_a), RGB_EFS_CMD, 2);
  converted(DEV(kprgb), RGB_EFF_CMD, 0, DEV(rgbs_a), RGB_EFS_CMD, 2);
  converted(DEV(probe_d), 0, 0, DEV(rgbs_b), RGB_EFS_CMD, 1);
  converted(DEV(rgbs_a), RGB_HUI_CMD, 0, DEV(rgbs_a), RGB_COLOR_HSB_CMD,
            RGB_COLOR_HSB_VAL((120 + CONFIG_KEYPAW_RGB_MATRIX_HUE_STEP) % 360, 100, 100));
  invoke(DEV(rgbs_a), RGB_SPI_CMD, 2000);
  CHECK(kp_rgb_effect_period(DEV(probe_a)) == 2000);
  CHECK(kp_rgb_effect_period(DEV(probe_b)) == CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
  invoke(DEV(kprgb), RGB_SPI_CMD, 1000);
  kp_rgb_matrix_lock();
  uint8_t brightness = kp_rgb_global.brightness;
  invoke(DEV(rgbs_a), RGB_HUI_CMD, 0);
  CHECK(kp_rgb_effect_data(DEV(probe_a))->color.h ==
        (120 + CONFIG_KEYPAW_RGB_MATRIX_HUE_STEP) % 360);
  CHECK(kp_rgb_effect_data(DEV(probe_b))->color.h == 60);
  invoke(DEV(kprgb), RGB_COLOR_HSB_CMD, RGB_COLOR_HSB_VAL(120, 100, 40));
  CHECK(kp_rgb_effect_data(DEV(probe_a))->color.b == 100);
  CHECK(kp_rgb_global.brightness == brightness);
  kp_rgb_matrix_unlock();
  k_timer_start(&boundary_timer, K_MSEC(1), K_NO_WAIT);
  CHECK(k_sem_take(&isr_checked, K_MSEC(100)) == 0);
  k_sleep(K_MSEC(20));
  CHECK(atomic_get(&selection_events_a) > a_notifications);
  CHECK(atomic_get(&selection_events_b) == b_notifications);
  printk("PASS selector binding conversion/metadata/alias/ISR\n");
}

static void reset_controls(void) {
  invoke(DEV(probe_c), 0, 0);
  invoke(DEV(probe_d), 0, 0);
  invoke(DEV(rgbs_a), RGB_SPI_CMD, 2000);
  invoke(DEV(rgbs_b), RGB_SPI_CMD, 3000);
  invoke(DEV(rgbs_b), RGB_COLOR_HSB_CMD, RGB_COLOR_HSB_VAL(42, 50, 100));
  k_sleep(K_MSEC(100));
  kp_rgb_matrix_lock();
  atomic_val_t a_notifications = atomic_get(&selection_events_a);
  atomic_val_t b_notifications = atomic_get(&selection_events_b);
  unsigned b_resets = DATA(probe_d)->resets;
  unsigned a_resets = DATA(probe_c)->resets;
  invoke(DEV(rgbs_a), RGB_RESET_CMD, 0);
  CHECK(kp_rgb_selector_overlay_state_locked(DEV(selector_a))->active_fx == DEV(probe_a));
  CHECK(kp_rgb_effect_period(DEV(probe_c)) == 1000);
  CHECK(kp_rgb_selector_overlay_state_locked(DEV(selector_b))->active_fx == DEV(probe_d));
  CHECK(kp_rgb_effect_period(DEV(probe_d)) == 3000);
  CHECK(kp_rgb_effect_data(DEV(probe_d))->color.h == 42);
  CHECK(DATA(probe_c)->resets == a_resets && DATA(probe_d)->resets == b_resets);
  kp_rgb_matrix_unlock();
  k_sleep(K_MSEC(100));
  kp_rgb_matrix_lock();
  CHECK(atomic_get(&selection_events_a) == a_notifications + 1);
  CHECK(atomic_get(&selection_events_b) == b_notifications);
  CHECK(DATA(probe_d)->resets == b_resets && DATA(probe_d)->active);
  invoke(DEV(kprgb), RGB_RESET_CMD, 0);
  CHECK(kp_rgb_selector_overlay_state_locked(DEV(selector_a))->active_fx == DEV(probe_a));
  CHECK(kp_rgb_selector_overlay_state_locked(DEV(selector_b))->active_fx == DEV(probe_b));
  CHECK(kp_rgb_effect_period(DEV(probe_d)) == CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
  CHECK(kp_rgb_effect_data(DEV(probe_d))->color.h == 180);
  CHECK(DATA(probe_d)->resets == b_resets);
  kp_rgb_matrix_unlock();
  k_sleep(K_MSEC(100));
  CHECK(atomic_get(&selection_events_a) == a_notifications + 1);
  CHECK(atomic_get(&selection_events_b) == b_notifications + 1);
  printk("PASS selector reset isolation/global reset\n");
}

static void scenario(void *a, void *b, void *c) {
  ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
#if !DT_NODE_HAS_PROP(DT_NODELABEL(kprgb), selector)
  k_sleep(K_MSEC(100));
  kp_rgb_matrix_lock();
  unsigned before = callback_count();
  CHECK(kp_rgb_selector_alias() == NULL);
  CHECK(zmk_rgb_matrix_select_effect(1) == -ENODEV);
  CHECK(zmk_rgb_matrix_cycle_effect(1) == -ENODEV);
  CHECK(kp_rgb_controller.selector.effect_index == 0);
  CHECK(before == callback_count());
  kp_rgb_matrix_unlock();
  printk("PASS absent public selector alias\n");
  return;
#endif
#if DT_PROP(DT_NODELABEL(rgbs_a), zephyr_deferred_init)
  CHECK(!device_is_ready(DEV(rgbs_a)));
  CHECK(kp_rgb_selector_overlays_start() == -ENODEV);
  zmk_rgb_matrix_start();
  k_sleep(K_MSEC(50));
  CHECK(callback_count() == 0);
  CHECK(zmk_rgb_matrix_select_effect(1) == -ENODEV);
  CHECK(device_init(DEV(rgbs_a)) == 0);
  CHECK(device_is_ready(DEV(rgbs_a)));
  zmk_rgb_matrix_start();
  printk("PASS canonical readiness deferred start\n");
#endif
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

  public_selection();
  controls();
  kp_rgb_matrix_lock();
  invoke(DEV(kprgb), RGB_EFF_CMD, 0);
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

  CHECK(kp_rgb_select_effect(1) == 0);
  k_sleep(K_MSEC(100));
  invoke(DEV(probe_a), 0, 0);
  invoke(DEV(probe_d), 0, 0);
  mark("hidden");
  k_sleep(K_MSEC(200));
  kp_rgb_matrix_lock(); CHECK(!DATA(probe_a)->active && !DATA(probe_d)->active); kp_rgb_matrix_unlock();
  CHECK(kp_rgb_select_effect(0) == 0);
  k_sleep(K_MSEC(100));
  mark("shown");
  k_sleep(K_MSEC(200));

  CHECK(zmk_rgb_matrix_off() == 0);
  k_sleep(K_MSEC(100));
  invoke(DEV(rgbs_a), RGB_EFF_CMD, 0);
  invoke(DEV(rgbs_b), RGB_EFR_CMD, 0);
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
  invoke(DEV(rgbs_a), RGB_EFR_CMD, 0);
  invoke(DEV(rgbs_b), RGB_EFF_CMD, 0);
  invoke(DEV(rgbs_a), RGB_EFF_CMD, 0);
  invoke(DEV(rgbs_b), RGB_EFR_CMD, 0);
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
  reset_controls();
  printk("PASS selector runtime\n");
}

K_THREAD_DEFINE(selector_scenario, 4096, scenario, NULL, NULL, NULL, 5, 0, 0);
