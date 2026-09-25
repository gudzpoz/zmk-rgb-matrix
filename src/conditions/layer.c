/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Built-in "layer" condition: active while a given keymap layer is active.
 * Declare one node per layer that a consumer should react to. The predicate is
 * sampled, never cached: an overlay's gate re-evaluates it every tick and a
 * trigger re-evaluates it on every layer event, so there is nothing to sync.
 */

#define DT_DRV_COMPAT keypaw_rgb_condition_layer

#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#include <zmk/events/layer_state_changed.h>
#include <zmk/rgb_matrix.h>

/* Layers only exist where the keymap does. ZMK gates src/keymap.c (and so the
 * layer-state accessors) on "(NOT CONFIG_ZMK_SPLIT) OR
 * CONFIG_ZMK_SPLIT_ROLE_CENTRAL" in zmk/app/CMakeLists.txt, so on a split
 * peripheral this predicate has no source of truth. The node is meant to be
 * consumed by a `central-authoritative` overlay or a central-only trigger, so
 * it is never called on a peripheral -- but it must still compile and link
 * there, because a shared devicetree expands it on both halves. Gate the read,
 * not the device. */
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define KP_COND_LAYER_HAS_KEYMAP 1
#include <zmk/keymap.h>
#else
#define KP_COND_LAYER_HAS_KEYMAP 0
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct kp_cond_layer_config {
  uint16_t layer;
  bool only_topmost;
};

static bool kp_cond_layer_active(const struct device *dev) {
#if KP_COND_LAYER_HAS_KEYMAP
  const struct kp_cond_layer_config *cfg = dev->config;

  if (cfg->only_topmost) {
    /* The property is a layer id; the accessor reports an index, hence the map. */
    return zmk_keymap_layer_index_to_id(zmk_keymap_highest_layer_active()) ==
           cfg->layer;
  }
  return zmk_keymap_layer_active((zmk_keymap_layer_id_t)cfg->layer);
#else
  ARG_UNUSED(dev);
  return false;
#endif
}

#define KP_COND_LAYER_DEFINE(inst)                                             \
  static const struct kp_cond_layer_config kp_cond_layer_##inst##_cfg = {      \
      .layer = DT_PROP(DT_DRV_INST(inst), layer),                              \
      .only_topmost = DT_PROP(DT_DRV_INST(inst), only_topmost),                \
  };                                                                           \
  KP_RGB_CONDITION_DEFINE(inst, kp_cond_layer_active,                          \
                          &kp_cond_layer_##inst##_cfg)

DT_INST_FOREACH_STATUS_OKAY(KP_COND_LAYER_DEFINE)

#if KP_COND_LAYER_HAS_KEYMAP
/* Repaint on the layer edge instead of waiting for the next engine tick, so a
 * layer change is immediate on the central and the push to the peripheral
 * starts at once. Central only: there is no layer state on a peripheral. */
static int kp_cond_layer_state_listener(const zmk_event_t *eh) {
  if (as_zmk_layer_state_changed(eh) != NULL) {
    zmk_rgb_matrix_flush();
  }
  return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(kp_cond_layer_state, kp_cond_layer_state_listener);
ZMK_SUBSCRIPTION(kp_cond_layer_state, zmk_layer_state_changed);
#endif /* KP_COND_LAYER_HAS_KEYMAP */

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
