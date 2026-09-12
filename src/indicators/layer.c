/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Built-in "layer" indicator: paints its targeted LEDs while a given keymap layer
 * is active, so a layer can be shown on a single dedicated LED instead of the
 * whole matrix. Declare one node per layer.
 *
 * The state is sampled in render(): the engine repaints every tick, so no event
 * subscription (and therefore no per-device listener plumbing) is needed.
 */

#define DT_DRV_COMPAT keypaw_rgb_indicator_layer

#include <stdbool.h>

#include <zephyr/device.h>

#include <zmk/rgb_matrix.h>

/* Layers only exist where the keymap does. ZMK gates src/keymap.c (and so the
 * layer-state accessors) on "(NOT CONFIG_ZMK_SPLIT) OR
 * CONFIG_ZMK_SPLIT_ROLE_CENTRAL" in zmk/app/CMakeLists.txt, so on a split
 * peripheral this indicator has no source of truth and stays dark. That is
 * harmless in practice: a layer key usually sits on one half only, and the other
 * half's `keys` list resolves to no LED anyway. */
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define KP_IND_LAYER_HAS_KEYMAP 1
#include <zmk/keymap.h>
#else
#define KP_IND_LAYER_HAS_KEYMAP 0
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct kp_ind_layer_config {
  struct kp_rgb_indicator_common_config common;
  uint16_t layer;
  bool only_topmost;
};
struct kp_ind_layer_data {
  struct kp_rgb_indicator_common_data common;
};

static void kp_ind_layer_render(const struct device *dev, struct kp_rgb_frame *frame) {
#if KP_IND_LAYER_HAS_KEYMAP
  const struct kp_ind_layer_config *cfg = dev->config;
  const struct kp_ind_layer_data *data = dev->data;
  bool active;

  if (cfg->only_topmost) {
    /* The property is a layer id; the accessor reports an index, hence the map. */
    active = zmk_keymap_layer_index_to_id(zmk_keymap_highest_layer_active()) == cfg->layer;
  } else {
    active = zmk_keymap_layer_active((zmk_keymap_layer_id_t)cfg->layer);
  }

  if (!active) {
    return;
  }

  kp_rgb_indicator_paint(frame, data->common.leds, data->common.led_count,
                         kp_hex_to_rgb(cfg->common.color), cfg->common.brightness);
#else
  ARG_UNUSED(dev);
  ARG_UNUSED(frame);
#endif
}

#define KP_IND_LAYER_DEFINE(inst)                                                \
  KP_RGB_INDICATOR_TARGET_ARRAYS(inst, kp_ind_layer_##inst);                     \
  static const struct kp_ind_layer_config kp_ind_layer_##inst##_cfg = {          \
      .common = KP_RGB_INDICATOR_COMMON(DT_DRV_INST(inst), kp_ind_layer_##inst),  \
      .layer = DT_PROP(DT_DRV_INST(inst), layer),                                \
      .only_topmost = DT_PROP(DT_DRV_INST(inst), only_topmost),                  \
  };                                                                             \
  static struct kp_ind_layer_data kp_ind_layer_##inst##_data;                    \
  KP_RGB_INDICATOR_DEFINE(inst, kp_ind_layer_render, kp_ind_layer_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_IND_LAYER_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
