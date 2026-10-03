/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT keypaw_rgb_condition_layer

#include <zmk/events/layer_state_changed.h>
#include <zmk/rgb_matrix.h>

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define HAS_KEYMAP 1
#include <zmk/keymap.h>
#else
#define HAS_KEYMAP 0
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct layer_cfg {
  uint16_t layer;
  bool only_topmost;
};

static bool sample(const struct device *dev, int64_t now_ms,
                   int64_t *next_wakeup_ms) {
  ARG_UNUSED(now_ms);
  ARG_UNUSED(next_wakeup_ms);
#if HAS_KEYMAP
  const struct layer_cfg *c = dev->config;
  return c->only_topmost
             ? zmk_keymap_layer_index_to_id(zmk_keymap_highest_layer_active()) == c->layer
             : zmk_keymap_layer_active((zmk_keymap_layer_id_t)c->layer);
#else
  ARG_UNUSED(dev);
  return false;
#endif
}

#if HAS_KEYMAP
#define INVALIDATE(inst)                                                       \
  kp_rgb_condition_invalidate(DEVICE_DT_GET(DT_DRV_INST(inst)));
static int listener(const zmk_event_t *eh) {
  if (as_zmk_layer_state_changed(eh)) {
    DT_INST_FOREACH_STATUS_OKAY(INVALIDATE)
  }
  return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(kp_cond_layer_state, listener);
ZMK_SUBSCRIPTION(kp_cond_layer_state, zmk_layer_state_changed);
#endif

#define DEFINE(inst)                                                           \
  static const struct layer_cfg cfg_##inst = {                                 \
      DT_PROP(DT_DRV_INST(inst), layer),                                       \
      DT_PROP(DT_DRV_INST(inst), only_topmost)};                               \
  static const struct kp_rgb_condition_api api_##inst = {                      \
      .scope = KP_RGB_CONDITION_CENTRAL_ONLY, .sample = sample};               \
  KP_RGB_CONDITION_DEFINE(inst, &api_##inst, &cfg_##inst, NULL)
DT_INST_FOREACH_STATUS_OKAY(DEFINE)

#endif
