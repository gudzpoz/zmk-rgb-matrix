/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT keypaw_rgb_condition_effect_selected

#include <zmk/events/rgb_selector_selection_changed.h>
#include <zmk/rgb_matrix.h>
#include <zmk/rgb_selector.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct selected_cfg {
  const struct device *selector;
  const struct device *effect;
};

static bool sample(const struct device *dev) {
  const struct selected_cfg *cfg = dev->config;
  return zmk_rgb_selector_get_selected(cfg->selector) == cfg->effect;
}

static const struct kp_rgb_condition_api api = {
    .scope = KP_RGB_CONDITION_ANY_SIDE, .sample = sample};
#define INVALIDATE(inst)                                                       \
  if (event->selector == cfg_##inst.selector)                                  \
    kp_rgb_condition_invalidate(DEVICE_DT_GET(DT_DRV_INST(inst)));

#define DEFINE(inst)                                                           \
  BUILD_ASSERT(                                                                \
      DT_NODE_HAS_COMPAT(DT_INST_PHANDLE(inst, selector),                      \
                         keypaw_rgb_overlay_selector) &&                       \
          DT_NODE_HAS_STATUS(DT_INST_PHANDLE(inst, selector), okay),           \
      "selected-effect condition requires an enabled selector overlay");       \
  BUILD_ASSERT(                                                                \
      DT_NODE_HAS_STATUS(DT_INST_PHANDLE(inst, effect), okay) &&               \
          DT_SAME_NODE(DT_PARENT(DT_INST_PHANDLE(inst, effect)),               \
                       DT_INST_PHANDLE(inst, selector)),                       \
      "selected-effect condition requires an enabled effect member "           \
      "of its selector");                                                      \
  static const struct selected_cfg cfg_##inst = {                              \
      .selector = DEVICE_DT_GET(DT_INST_PHANDLE(inst, selector)),              \
      .effect = DEVICE_DT_GET(DT_INST_PHANDLE(inst, effect)),                  \
  };                                                                           \
  KP_RGB_CONDITION_DEFINE(inst, &api, &cfg_##inst, NULL)

DT_INST_FOREACH_STATUS_OKAY(DEFINE)

static int listener(const zmk_event_t *eh) {
  const struct zmk_rgb_selector_selection_changed *event =
      as_zmk_rgb_selector_selection_changed(eh);
  if (event != NULL) {
    DT_INST_FOREACH_STATUS_OKAY(INVALIDATE)
  }
  return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(kp_cond_effect_selected, listener);
ZMK_SUBSCRIPTION(kp_cond_effect_selected, zmk_rgb_selector_selection_changed);

#endif
