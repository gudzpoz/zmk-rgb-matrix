/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
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
  const struct device *effect;    /* the composited effect */
};

struct kp_ovl_data {
  struct kp_rgb_overlay_common_data common;
};

static const struct device *kp_ovl_event_target(const struct device *dev) {
  const struct kp_ovl_config *cfg = dev->config;
  return cfg->effect;
}

static bool kp_ovl_render(const struct device *dev, const struct kp_rgb_frame *frame) {
  const struct kp_ovl_config *cfg = dev->config;
  struct kp_rgb_frame child_frame = *frame;
  child_frame.pixels = frame->scratch;
  child_frame.scratch = NULL;
  memset(child_frame.pixels, 0, child_frame.count * sizeof(*child_frame.pixels));
  bool animating = kp_rgb_effect_render(cfg->effect, &child_frame);
  kp_rgb_overlay_paint_pixels(frame, frame->targets, frame->target_count,
                              frame->scratch, cfg->common.opacity);
  return animating;
}

#define KP_OVL_DEFINE(inst)                                                    \
  KP_RGB_OVERLAY_TARGET_ARRAYS(inst, kp_ovl_##inst);                           \
  KP_RGB_OVERLAY_EFFECT_ASSERT(DT_DRV_INST(inst))                              \
  static const struct kp_ovl_config kp_ovl_##inst##_cfg = {                    \
      .common = KP_RGB_OVERLAY_COMMON(DT_DRV_INST(inst), kp_ovl_##inst),       \
      .effect = KP_RGB_OVERLAY_EFFECT(DT_DRV_INST(inst)),                      \
  };                                                                           \
  static struct kp_ovl_data kp_ovl_##inst##_data;                              \
  KP_RGB_OVERLAY_DEFINE(inst, kp_ovl_render, kp_ovl_event_target, true,        \
                        kp_ovl_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_OVL_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
