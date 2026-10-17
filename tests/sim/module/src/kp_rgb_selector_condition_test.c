/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#define DT_DRV_COMPAT test_rgb_selector_event_condition
#include <zephyr/kernel.h>
#include <zmk/rgb_matrix.h>
#include <zmk/rgb_selector.h>
#include <zmk/events/rgb_selector_selection_changed.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
struct event_config { const struct device *selector, *effect; };
static atomic_t pause_sample, samples, notifications;
K_SEM_DEFINE(sample_entered, 0, 1);
K_SEM_DEFINE(sample_release, 0, 1);

void selector_test_arm_sample(void) { atomic_set(&pause_sample, 1); }
int selector_test_wait_sample(void) { return k_sem_take(&sample_entered, K_MSEC(500)); }
void selector_test_release_sample(void) { k_sem_give(&sample_release); }
unsigned selector_test_samples(void) { return atomic_get(&samples); }
unsigned selector_test_notifications(void) { return atomic_get(&notifications); }

static bool sample(const struct device *dev) {
  atomic_inc(&samples);
  if (atomic_cas(&pause_sample, 1, 0)) {
    k_sem_give(&sample_entered);
    k_sem_take(&sample_release, K_FOREVER);
  }
  const struct event_config *cfg = dev->config;
  return zmk_rgb_selector_get_selected(cfg->selector) == cfg->effect;
}
static const struct kp_rgb_condition_api api = {
  .scope = KP_RGB_CONDITION_ANY_SIDE, .sample = sample,
};
#define DEFINE(inst) \
  static const struct event_config config_##inst = { \
    .selector = DEVICE_DT_GET(DT_INST_PHANDLE(inst, selector)), \
    .effect = DEVICE_DT_GET(DT_INST_PHANDLE(inst, effect)), \
  }; \
  KP_RGB_CONDITION_DEFINE(inst, &api, &config_##inst, NULL);
DT_INST_FOREACH_STATUS_OKAY(DEFINE)

static int listener(const zmk_event_t *eh) {
  const struct zmk_rgb_selector_selection_changed *ev =
      as_zmk_rgb_selector_selection_changed(eh);
#define INVALIDATE(inst) do { \
  if (ev->selector == config_##inst.selector) { \
    atomic_inc(&notifications); \
    kp_rgb_condition_invalidate(DEVICE_DT_GET(DT_DRV_INST(inst))); \
  } \
} while (0);
  DT_INST_FOREACH_STATUS_OKAY(INVALIDATE)
  return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(selector_event_condition_test, listener);
ZMK_SUBSCRIPTION(selector_event_condition_test, zmk_rgb_selector_selection_changed);
#endif
