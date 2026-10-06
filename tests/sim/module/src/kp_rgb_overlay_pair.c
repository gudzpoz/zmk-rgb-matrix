/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT keypaw_rgb_overlay_pair

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct kp_rgb_pair_config {
  struct kp_rgb_overlay_common_config common;
};

struct kp_rgb_pair_data {
  struct kp_rgb_overlay_common_data common;
  struct kp_rgb_effect_instance first;
  struct kp_rgb_effect_instance second;
};

static size_t kp_rgb_pair_first_targets[MAX(1, KP_LED_COUNT / 2)];
static size_t kp_rgb_pair_second_targets[MAX(1, KP_LED_COUNT - KP_LED_COUNT / 2)];

static void kp_rgb_pair_set_active(const struct device *dev, bool active,
                                   int64_t now_ms) {
  struct kp_rgb_pair_data *data = dev->data;
  kp_rgb_effect_instance_set_active(&data->first, active, now_ms);
  kp_rgb_effect_instance_set_active(&data->second, active, now_ms);
}

static void kp_rgb_pair_reset(const struct device *dev, int64_t now_ms) {
  struct kp_rgb_pair_data *data = dev->data;
  kp_rgb_effect_instance_reset(&data->first, now_ms);
  kp_rgb_effect_instance_reset(&data->second, now_ms);
}

static bool kp_rgb_pair_on_event(const struct device *dev,
                                 const struct kp_rgb_key_event *event) {
  struct kp_rgb_pair_data *data = dev->data;
  return (event->position & 1) == 0
             ? kp_rgb_effect_instance_on_event(&data->first, event)
             : kp_rgb_effect_instance_on_event(&data->second, event);
}

static bool kp_rgb_pair_render(const struct device *dev,
                               const struct kp_rgb_frame *frame) {
  const struct kp_rgb_pair_config *cfg = dev->config;
  struct kp_rgb_pair_data *data = dev->data;
  if (frame->elapsed_ms == 0) {
    kp_rgb_effect_instance_restart_clock(&data->first);
    kp_rgb_effect_instance_restart_clock(&data->second);
    printk("rgb-pair restart both\n");
  }

  size_t first_count = frame->target_count / 2;
  size_t second_count = frame->target_count - first_count;
  for (size_t i = 0; i < first_count; i++) {
    kp_rgb_pair_first_targets[i] = frame->targets[i];
  }
  for (size_t i = 0; i < second_count; i++) {
    kp_rgb_pair_second_targets[i] = frame->targets[first_count + i];
  }

  struct kp_rgb_frame child = *frame;
  child.pixels = frame->scratch;
  child.scratch = NULL;
  memset(child.pixels, 0, child.count * sizeof(*child.pixels));
  bool animating;
  if ((frame->local_ms / 500) & 1) {
    child.targets = kp_rgb_pair_second_targets;
    child.target_count = second_count;
    animating = kp_rgb_effect_instance_render(&data->second, &child);
    printk("rgb-pair second demand=%d\n", animating);
  } else {
    child.targets = kp_rgb_pair_first_targets;
    child.target_count = first_count;
    animating = kp_rgb_effect_instance_render(&data->first, &child);
    printk("rgb-pair first demand=%d\n", animating);
  }
  kp_rgb_overlay_paint_pixels(frame, child.targets, child.target_count,
                              child.pixels, cfg->common.opacity);
  return animating;
}

static const struct kp_rgb_effect_callbacks kp_rgb_pair_callbacks = {
    .render = kp_rgb_pair_render,
    .on_event = kp_rgb_pair_on_event,
    .set_active = kp_rgb_pair_set_active,
    .reset = kp_rgb_pair_reset,
};

#define KP_RGB_PAIR_DEFINE(inst)                                                 \
  KP_RGB_OVERLAY_TARGET_ARRAYS(inst, kp_rgb_pair_##inst);                        \
  static const struct kp_rgb_pair_config kp_rgb_pair_##inst##_cfg = {            \
      .common = KP_RGB_OVERLAY_COMMON(DT_DRV_INST(inst), kp_rgb_pair_##inst),    \
  };                                                                             \
  static struct kp_rgb_pair_data kp_rgb_pair_##inst##_data = {                   \
      .first = KP_RGB_EFFECT_INSTANCE_INIT(                                      \
          DEVICE_DT_GET(DT_CHILD(DT_DRV_INST(inst), first))),                   \
      .second = KP_RGB_EFFECT_INSTANCE_INIT(                                     \
          DEVICE_DT_GET(DT_CHILD(DT_DRV_INST(inst), second))),                  \
  };                                                                             \
  KP_RGB_OVERLAY_DEFINE(inst, &kp_rgb_pair_callbacks, false,                    \
                        kp_rgb_pair_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_RGB_PAIR_DEFINE)

#endif
