/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * The generic "overlay" kind: a compositor. While its condition is active it
 * renders its effect into a private layer buffer and blends the targeted LEDs
 * back onto the frame, over whatever the active effect painted.
 *
 * The effect is either nested (one child of this node: a private preset) or
 * referenced (`effect = <&fx>;`: a shared registry effect). The layer buffer is
 * shared across instances because overlay renderers run one at a time, serialised
 * under the engine lock in the render tick; a per-instance buffer would only
 * waste RAM.
 */

#define DT_DRV_COMPAT keypaw_rgb_overlay

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct kp_ovl_config {
  struct kp_rgb_overlay_common_config common;
  const struct device *condition; /* NULL = always active */
  const struct device *effect;    /* the composited effect */
};

struct kp_ovl_data {
  struct kp_rgb_overlay_common_data common;
};

/* Indexed like frame->pixels; sized to this half's LED count. */
static struct led_rgb kp_ovl_layer[KP_LED_COUNT];

static bool kp_ovl_active(const struct device *dev) {
  const struct kp_ovl_config *cfg = dev->config;

  if (cfg->condition == NULL) {
    return true;
  }
  const struct kp_rgb_condition_api *cond =
      (const struct kp_rgb_condition_api *)cfg->condition->api;
  return cond->active(cfg->condition);
}

/* The effect the engine should deliver input events to, so a composited
 * reactive/ripple effect can see keystrokes. */
static const struct device *kp_ovl_event_target(const struct device *dev) {
  const struct kp_ovl_config *cfg = dev->config;
  return cfg->effect;
}

static void kp_ovl_render(const struct device *dev, struct kp_rgb_frame *frame) {
  const struct kp_ovl_config *cfg = dev->config;
  const struct kp_ovl_data *data = dev->data;
  const struct kp_rgb_effect_api *fx =
      (const struct kp_rgb_effect_api *)cfg->effect->api;

  /* The engine calls this only while kp_ovl_active() is true. Effects overwrite
   * the whole frame, so render into the layer buffer and then blend only this
   * overlay's targets back onto the real one. */
  struct led_rgb *base = frame->pixels;
  memset(kp_ovl_layer, 0, sizeof(kp_ovl_layer));
  frame->pixels = kp_ovl_layer;
  fx->render(cfg->effect, frame);
  frame->pixels = base;

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
}

#define KP_OVL_DEFINE(inst)                                                    \
  KP_RGB_OVERLAY_TARGET_ARRAYS(inst, kp_ovl_##inst);                           \
  KP_RGB_OVERLAY_EFFECT_ASSERT(DT_DRV_INST(inst))                              \
  static const struct kp_ovl_config kp_ovl_##inst##_cfg = {                    \
      .common = KP_RGB_OVERLAY_COMMON(DT_DRV_INST(inst), kp_ovl_##inst),       \
      .condition = KP_RGB_CONDITION_PTR(DT_DRV_INST(inst)),                   \
      .effect = KP_RGB_OVERLAY_EFFECT(DT_DRV_INST(inst)),                      \
  };                                                                           \
  static struct kp_ovl_data kp_ovl_##inst##_data;                              \
  KP_RGB_OVERLAY_DEFINE(inst, kp_ovl_active, kp_ovl_render,                    \
                        kp_ovl_event_target, kp_ovl_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_OVL_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
