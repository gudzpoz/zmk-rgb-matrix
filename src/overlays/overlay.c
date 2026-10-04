/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * The generic "overlay" kind: a compositor. While its condition is active it
 * renders its effect into a private layer buffer and blends the targeted LEDs
 * back onto the frame, over whatever the active effect painted.
 *
 * The layer buffer is shared across instances because overlay renderers run
 * one at a time under the engine lock.
 */

#define DT_DRV_COMPAT keypaw_rgb_overlay

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#include <zmk/rgb_matrix.h>
#include "../rgb_matrix_internal.h"

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct kp_ovl_config {
  struct kp_rgb_overlay_common_config common;
  const struct device *effect;    /* the composited effect */
};

struct kp_ovl_data {
  struct kp_rgb_overlay_common_data common;
};

#if KP_LED_COUNT > 0
/* Indexed like frame->pixels; sized to this half's LED count. */
static struct led_rgb kp_ovl_layer[KP_LED_COUNT];
#endif

/* The effect the engine should deliver input events to, so a composited
 * reactive/ripple effect can see keystrokes. */
static const struct device *kp_ovl_event_target(const struct device *dev) {
  const struct kp_ovl_config *cfg = dev->config;
  return cfg->effect;
}

static void kp_ovl_render(const struct device *dev, struct kp_rgb_frame *frame) {
#if KP_LED_COUNT == 0
  ARG_UNUSED(dev);
  ARG_UNUSED(frame);
#else
  const struct kp_ovl_config *cfg = dev->config;
  const struct kp_ovl_data *data = dev->data;

  /* Effects overwrite the whole frame, so render into the shared layer buffer and
   * blend only this overlay's targets back. */
  struct led_rgb *base = frame->pixels;
  memset(kp_ovl_layer, 0, sizeof(kp_ovl_layer));
  struct kp_rgb_frame child_frame = *frame;
  child_frame.pixels = kp_ovl_layer;
  kp_rgb_effect_render(cfg->effect, &child_frame);

  if (cfg->common.all_leds) {
    /* No target list: blend the whole layer buffer straight across. */
    for (size_t led = 0; led < frame->count; led++) {
      base[led] =
          kp_rgb_rgb_mix(base[led], kp_ovl_layer[led], cfg->common.opacity);
    }
    return;
  }

  kp_rgb_overlay_paint_pixels(frame, data->common.leds, data->common.led_count,
                              kp_ovl_layer, cfg->common.opacity);
#endif /* KP_LED_COUNT > 0 */
}

#define KP_OVL_DEFINE(inst)                                                    \
  KP_RGB_OVERLAY_TARGET_ARRAYS(inst, kp_ovl_##inst);                           \
  KP_RGB_OVERLAY_EFFECT_ASSERT(DT_DRV_INST(inst))                              \
  static const struct kp_ovl_config kp_ovl_##inst##_cfg = {                    \
      .common = KP_RGB_OVERLAY_COMMON(DT_DRV_INST(inst), kp_ovl_##inst),       \
      .effect = KP_RGB_OVERLAY_EFFECT(DT_DRV_INST(inst)),                      \
  };                                                                           \
  static struct kp_ovl_data kp_ovl_##inst##_data;                              \
  KP_RGB_OVERLAY_DEFINE(inst, kp_ovl_render, kp_ovl_event_target, kp_ovl_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_OVL_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
