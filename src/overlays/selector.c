/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT keypaw_rgb_overlay_selector

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "../rgb_matrix_internal.h"

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct kp_selector_config {
  struct kp_rgb_overlay_common_config common;
};

struct kp_selector_data {
  struct kp_rgb_overlay_common_data common;
  struct kp_rgb_selector selector;
  const struct device **members;
  struct kp_rgb_effect_instance *instances;
  bool *reset_pending;
  size_t live_slot;
  bool active;
  bool prepared;
  bool ready;
  bool reset_requested;
};

static void kp_selector_reconcile(const struct device *dev, int64_t now_ms) {
  struct kp_selector_data *data = dev->data;
  size_t wanted = data->active ? data->selector.effect_index : SIZE_MAX;

  if (data->live_slot != wanted && data->live_slot != SIZE_MAX) {
    kp_rgb_effect_instance_set_active(&data->instances[data->live_slot], false,
                                      now_ms);
    data->live_slot = SIZE_MAX;
  }
  if (data->reset_requested) {
    for (size_t i = 0; i < data->selector.effect_count; i++) {
      data->reset_pending[i] = true;
      if (kp_rgb_selector_at(&data->selector, i) != NULL) {
        kp_rgb_effect_instance_restart_clock(&data->instances[i]);
      }
    }
    data->reset_requested = false;
  }
  if (wanted == SIZE_MAX)
    return;

  struct kp_rgb_effect_instance *instance = &data->instances[wanted];
  if (data->reset_pending[wanted]) {
    kp_rgb_effect_instance_reset(instance, now_ms);
    data->reset_pending[wanted] = false;
  }
  kp_rgb_effect_instance_set_active(instance, true, now_ms);
  data->live_slot = wanted;
}

static void kp_selector_reset(const struct device *dev, int64_t now_ms) {
  struct kp_selector_data *data = dev->data;
  if (!data->ready)
    return;

  data->reset_requested = true;
  kp_selector_reconcile(dev, now_ms);
}

static void kp_selector_set_active(const struct device *dev, bool active,
                                   int64_t now_ms) {
  struct kp_selector_data *data = dev->data;
  if (!data->ready)
    return;

  data->active = active;
  kp_selector_reconcile(dev, now_ms);
}

static bool kp_selector_on_event(const struct device *dev,
                                 const struct kp_rgb_key_event *event) {
  struct kp_selector_data *data = dev->data;
  if (!data->ready)
    return false;

  kp_selector_reconcile(dev, k_uptime_get());
  return data->live_slot != SIZE_MAX &&
         kp_rgb_effect_instance_on_event(&data->instances[data->live_slot],
                                         event);
}

static bool kp_selector_render(const struct device *dev,
                               const struct kp_rgb_frame *frame) {
  struct kp_selector_data *data = dev->data;
  const struct kp_selector_config *cfg = dev->config;
  bool animating = false;

  if (data->ready) {
    if (frame->elapsed_ms == 0) {
      for (size_t i = 0; i < data->selector.effect_count; i++) {
        if (kp_rgb_selector_at(&data->selector, i) != NULL) {
          kp_rgb_effect_instance_restart_clock(&data->instances[i]);
        }
      }
    }
    kp_selector_reconcile(dev, frame->local_ms);
  }

  struct kp_rgb_frame child_frame = *frame;
  child_frame.pixels = frame->scratch;
  child_frame.scratch = NULL;
  for (size_t i = 0; i < frame->target_count; i++) {
    child_frame.pixels[frame->targets[i]] = (struct led_rgb){0};
  }
  if (data->ready && data->live_slot != SIZE_MAX) {
    animating = kp_rgb_effect_instance_render(&data->instances[data->live_slot],
                                              &child_frame);
  }
  kp_rgb_overlay_paint_pixels(frame, frame->targets, frame->target_count,
                              frame->scratch, cfg->common.opacity);
  return animating;
}

static const struct kp_rgb_effect_callbacks kp_selector_callbacks = {
    .render = kp_selector_render,
    .on_event = kp_selector_on_event,
    .set_active = kp_selector_set_active,
    .reset = kp_selector_reset,
};

struct kp_rgb_selector *
kp_rgb_selector_overlay_state_locked(const struct device *dev) {
  if (dev == NULL || !device_is_ready(dev))
    return NULL;
  const struct kp_rgb_overlay_api *api = dev->api;
  if (api == NULL || api->callbacks != &kp_selector_callbacks)
    return NULL;
  struct kp_selector_data *data = dev->data;
  return data->ready ? &data->selector : NULL;
}

int kp_rgb_selector_overlay_select_locked(const struct device *dev,
                                          uint16_t slot) {
  struct kp_rgb_selector *selector = kp_rgb_selector_overlay_state_locked(dev);
  if (selector == NULL)
    return -ENODEV;
  const struct device *target = kp_rgb_selector_at(selector, slot);
  if (target != NULL && !device_is_ready(target))
    return -ENODEV;
  int changed = kp_rgb_selector_select_locked(selector, slot);
  if (changed > 0)
    zmk_rgb_matrix_flush();
  return changed;
}

int kp_rgb_selector_overlay_reset_locked(const struct device *dev) {
  struct kp_rgb_selector *selector = kp_rgb_selector_overlay_state_locked(dev);
  if (selector == NULL)
    return -ENODEV;
  struct kp_selector_data *data = dev->data;
  int err = kp_rgb_selector_apply_defaults_locked(selector);
  if (kp_rgb_selector_at(selector, selector->initial_effect) == NULL) {
    selector->effect_index = 0;
    err = kp_rgb_selector_resolve_locked(selector);
  }
  data->reset_requested = true;
  zmk_rgb_matrix_flush();
  return err;
}

static void kp_selector_refresh_members(struct kp_selector_data *data) {
  for (size_t i = 0; i < data->selector.effect_count; i++) {
    const struct device *effect = data->instances[i].effect;
    data->members[i] =
        effect != NULL && device_is_ready(effect) ? effect : NULL;
  }
}

static void kp_selector_prepare_locked(struct kp_selector_data *data) {
  if (data->prepared)
    return;
  kp_selector_refresh_members(data);
  struct kp_rgb_selector defaults = data->selector;
  defaults.publish_changes = false;
  kp_rgb_selector_apply_defaults_locked(&defaults);
  data->selector.effect_index = data->selector.initial_effect;
  data->selector.active_fx = NULL;
  data->prepared = true;
}

static int kp_selector_prepare(const struct device *dev) {
  if (!device_is_ready(dev))
    return 0;
  kp_rgb_matrix_lock();
  kp_selector_prepare_locked(dev->data);
  kp_rgb_matrix_unlock();
  return 0;
}

static int kp_selector_start(const struct device *dev) {
  if (!device_is_ready(dev)) {
    LOG_ERR("RGB selector %s is not ready", dev->name);
    return -ENODEV;
  }
  const struct device *control = kp_rgb_selector_control(dev);
  if (control == NULL || !device_is_ready(control)) {
    LOG_ERR("RGB selector %s canonical control is not ready", dev->name);
    return -ENODEV;
  }
  struct kp_selector_data *data = dev->data;
  kp_rgb_matrix_lock();
  if (data->ready) {
    kp_rgb_matrix_unlock();
    return 0;
  }
  kp_selector_prepare_locked(data);
  /* Settings may restore existing member parameters before startup. */
  for (size_t i = 0; i < data->selector.effect_count; i++) {
    const struct device *effect = data->instances[i].effect;
    bool newly_ready =
        data->members[i] == NULL && effect != NULL && device_is_ready(effect);
    data->members[i] = newly_ready ? effect : NULL;
  }
  struct kp_rgb_selector defaults = data->selector;
  defaults.publish_changes = false;
  kp_rgb_selector_apply_defaults_locked(&defaults);
  kp_selector_refresh_members(data);
  if (data->selector.active_fx == NULL) {
    data->selector.effect_index = data->selector.initial_effect;
  }
  if (kp_rgb_selector_at(&data->selector, data->selector.effect_index) ==
      NULL) {
    data->selector.effect_index = 0;
  }
  int err = kp_rgb_selector_resolve_locked(&data->selector);
  data->ready = err == 0;
  if (data->ready) {
    data->selector.publish_changes = true;
    kp_rgb_selector_publish_locked(&data->selector, NULL);
  }
  kp_rgb_matrix_unlock();
  if (err < 0)
    LOG_ERR("RGB selector %s has no ready effect", dev->name);
  return err < 0 ? -ENODEV : 0;
}

#define KP_SELECTOR_DEFAULT(node_id)                                           \
  {.color = KP_RGB_HSB_FROM_HEX(DT_PROP_OR(node_id, color, 0)),                \
   .duration_ms = DT_PROP_OR(node_id, duration, 0)},

#define KP_SELECTOR_INSTANCE(node_id)                                          \
  COND_CODE_1(DT_NODE_HAS_STATUS(node_id, okay),                               \
              (KP_RGB_EFFECT_INSTANCE_INIT(DEVICE_DT_GET(node_id)), ),         \
              (KP_RGB_EFFECT_INSTANCE_INIT(NULL), ))

#define KP_SELECTOR_DEFINE(inst)                                               \
  BUILD_ASSERT(DT_CHILD_NUM_STATUS_OKAY(DT_DRV_INST(inst)) > 0,                \
               "selector requires at least one enabled nested effect");        \
  BUILD_ASSERT(DT_CHILD_NUM(DT_DRV_INST(inst)) <= UINT16_MAX,                  \
               "selector has too many effect slots");                          \
  BUILD_ASSERT(DT_PROP(DT_DRV_INST(inst), initial_effect) >= 0 &&              \
                   DT_PROP(DT_DRV_INST(inst), initial_effect) <= UINT16_MAX,   \
               "selector initial-effect must fit a local slot");               \
  KP_RGB_OVERLAY_TARGET_ARRAYS(inst, kp_selector_##inst);                      \
  static const struct device                                                   \
      *kp_selector_##inst##_effects[DT_CHILD_NUM(DT_DRV_INST(inst))];          \
  static const struct kp_rgb_effect_defaults kp_selector_##inst##_defaults[] = \
      {DT_FOREACH_CHILD(DT_DRV_INST(inst), KP_SELECTOR_DEFAULT)};              \
  static struct kp_rgb_effect_instance kp_selector_##inst##_instances[] = {    \
      DT_FOREACH_CHILD(DT_DRV_INST(inst), KP_SELECTOR_INSTANCE)};              \
  static bool                                                                  \
      kp_selector_##inst##_reset_pending[DT_CHILD_NUM(DT_DRV_INST(inst))];     \
  static const struct kp_selector_config kp_selector_##inst##_cfg = {          \
      .common = KP_RGB_OVERLAY_COMMON(DT_DRV_INST(inst), kp_selector_##inst),  \
  };                                                                           \
  static struct kp_selector_data kp_selector_##inst##_data = {                 \
      .selector =                                                              \
          {                                                                    \
              .owner = DEVICE_DT_GET(DT_DRV_INST(inst)),                       \
              .effects = kp_selector_##inst##_effects,                         \
              .effect_defaults = kp_selector_##inst##_defaults,                \
              .effect_count = DT_CHILD_NUM(DT_DRV_INST(inst)),                 \
              .initial_duration_ms =                                           \
                  DT_PROP(DT_DRV_INST(inst), initial_duration_ms),             \
              .initial_effect = DT_PROP(DT_DRV_INST(inst), initial_effect),    \
          },                                                                   \
      .members = kp_selector_##inst##_effects,                                 \
      .instances = kp_selector_##inst##_instances,                             \
      .reset_pending = kp_selector_##inst##_reset_pending,                     \
      .live_slot = SIZE_MAX,                                                   \
  };                                                                           \
  KP_RGB_OVERLAY_DEFINE(inst, &kp_selector_callbacks, true,                    \
                        kp_selector_##inst);                                   \
  static int kp_selector_##inst##_prepare(void) {                              \
    return kp_selector_prepare(DEVICE_DT_GET(DT_DRV_INST(inst)));              \
  }                                                                            \
  SYS_INIT(kp_selector_##inst##_prepare, APPLICATION,                          \
           CONFIG_APPLICATION_INIT_PRIORITY);

DT_INST_FOREACH_STATUS_OKAY(KP_SELECTOR_DEFINE)

#define KP_SELECTOR_START(inst)                                                \
  do {                                                                         \
    int err = kp_selector_start(DEVICE_DT_GET(DT_DRV_INST(inst)));             \
    if (result == 0)                                                           \
      result = err;                                                            \
  } while (0);

int kp_rgb_selector_overlays_start(void) {
  int result = 0;
  DT_INST_FOREACH_STATUS_OKAY(KP_SELECTOR_START)
  return result;
}

static void kp_selector_defaults_locked(const struct device *dev) {
  if (!device_is_ready(dev))
    return;
  struct kp_selector_data *data = dev->data;
  if (data->ready) {
    (void)kp_rgb_selector_overlay_reset_locked(dev);
    return;
  }
  kp_selector_prepare_locked(data);
  kp_selector_refresh_members(data);
  struct kp_rgb_selector defaults = data->selector;
  defaults.publish_changes = false;
  kp_rgb_selector_apply_defaults_locked(&defaults);
  data->selector.effect_index = data->selector.initial_effect;
  data->selector.active_fx = NULL;
  data->reset_requested = true;
  zmk_rgb_matrix_flush();
}

#define KP_SELECTOR_DEFAULTS(inst)                                             \
  kp_selector_defaults_locked(DEVICE_DT_GET(DT_DRV_INST(inst)));

void kp_rgb_selector_overlays_defaults_locked(void) {
  DT_INST_FOREACH_STATUS_OKAY(KP_SELECTOR_DEFAULTS)
}

#else

int kp_rgb_selector_overlays_start(void) { return 0; }

void kp_rgb_selector_overlays_defaults_locked(void) {}

struct kp_rgb_selector *
kp_rgb_selector_overlay_state_locked(const struct device *dev) {
  ARG_UNUSED(dev);
  return NULL;
}

int kp_rgb_selector_overlay_reset_locked(const struct device *dev) {
  ARG_UNUSED(dev);
  return -ENODEV;
}

int kp_rgb_selector_overlay_select_locked(const struct device *dev,
                                          uint16_t slot) {
  ARG_UNUSED(dev);
  ARG_UNUSED(slot);
  return -ENODEV;
}

#endif
