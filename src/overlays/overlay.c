/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT keypaw_rgb_overlay

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zmk/rgb_persist.h>
#include <drivers/behavior.h>

#include "../rgb_matrix_internal.h"

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct kp_selector_config {
  struct kp_rgb_overlay_common_config common;
  const struct device *private_effect;
};

struct kp_selector_data {
  struct kp_rgb_overlay_common_data common;
  struct kp_rgb_selector selector;
  struct kp_rgb_effect_instance *instances;
  bool *reset_pending;
  size_t live_slot;
  bool active;
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
  const struct kp_selector_config *cfg = dev->config;
  if (cfg->private_effect != NULL) {
    const struct device *effect = cfg->private_effect;
    const struct kp_rgb_effect_callbacks *callbacks = effect->api;
    if (callbacks->reset != NULL) callbacks->reset(effect, now_ms);
    return;
  }
  struct kp_selector_data *data = dev->data;
  if (!data->ready)
    return;

  data->reset_requested = true;
  kp_selector_reconcile(dev, now_ms);
}

static void kp_selector_set_active(const struct device *dev, bool active,
                                   int64_t now_ms) {
  const struct kp_selector_config *cfg = dev->config;
  if (cfg->private_effect != NULL) {
    const struct device *effect = cfg->private_effect;
    const struct kp_rgb_effect_callbacks *callbacks = effect->api;
    if (callbacks->set_active != NULL) callbacks->set_active(effect, active, now_ms);
    return;
  }
  struct kp_selector_data *data = dev->data;
  if (!data->ready)
    return;

  data->active = active;
  kp_selector_reconcile(dev, now_ms);
}

static bool kp_selector_on_event(const struct device *dev,
                                 const struct kp_rgb_key_event *event) {
  const struct kp_selector_config *cfg = dev->config;
  if (cfg->private_effect != NULL) {
    const struct device *effect = cfg->private_effect;
    const struct kp_rgb_effect_callbacks *callbacks = effect->api;
    return callbacks->on_event != NULL && callbacks->on_event(effect, event);
  }
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
  const struct kp_selector_config *cfg = dev->config;
  struct kp_selector_data *data = cfg->private_effect == NULL ? dev->data : NULL;
  bool animating = false;

  if (cfg->private_effect == NULL && data->ready) {
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
  if (cfg->private_effect != NULL) {
    animating = kp_rgb_effect_render(cfg->private_effect, &child_frame);
  } else if (data->ready && data->live_slot != SIZE_MAX) {
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
  for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++) {
    if (kp_rgb_selector_registry_at(i)->owner != dev) continue;
    struct kp_selector_data *data = dev->data;
    return data->ready ? &data->selector : NULL;
  }
  return NULL;
}

int kp_rgb_selector_overlay_select_locked(const struct device *dev,
                                          uint16_t slot) {
  struct kp_rgb_selector *selector = kp_rgb_selector_overlay_state_locked(dev);
  if (selector == NULL)
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
  kp_rgb_selector_apply_defaults_locked(selector);
  selector->effect_index =
      kp_rgb_selector_at(selector, selector->initial_effect) != NULL
          ? selector->initial_effect
          : 0;
  int err = kp_rgb_selector_resolve_locked(selector);
  data->reset_requested = true;
  zmk_rgb_matrix_flush();
  return err;
}

static int kp_selector_start(const struct device *dev) {
  if (!device_is_ready(dev)) {
    LOG_ERR("RGB selector %s is not ready", dev->name);
    return -ENODEV;
  }
  struct kp_selector_data *data = dev->data;
  kp_rgb_matrix_lock();
  if (data->ready) {
    kp_rgb_matrix_unlock();
    return 0;
  }
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

#define KP_SELECTOR_ID(node_id) STRINGIFY(DT_NODE_FULL_NAME_TOKEN(node_id))

#define KP_SELECTOR_MEMBER_ID_LENGTH(node_id)                                  \
  BUILD_ASSERT(sizeof(KP_SELECTOR_ID(node_id)) <=                              \
                   KP_RGB_PERSIST_MAX_ID_LENGTH,                               \
               "RGB node name exceeds KP_RGB_PERSIST_MAX_ID_LENGTH");

#define KP_SELECTOR_MEMBER(node_id)                                            \
  {.effect = DEVICE_DT_GET(node_id), .id = KP_SELECTOR_ID(node_id)},
#define KP_SELECTOR_INSTANCE(node_id)                                          \
  KP_RGB_EFFECT_INSTANCE_INIT(DEVICE_DT_GET(node_id)),

#define KP_SELECTOR_DEFINE(inst)                                               \
  BUILD_ASSERT(sizeof(DEVICE_DT_NAME(DT_DRV_INST(inst))) <= 9,                 \
               "controlled overlay endpoint must be at most 8 bytes");         \
  BUILD_ASSERT(sizeof(KP_SELECTOR_ID(DT_DRV_INST(inst))) <=                    \
                   KP_RGB_PERSIST_MAX_SELECTOR_ID_LENGTH,                      \
               "RGB selector token exceeds "                                   \
               "KP_RGB_PERSIST_MAX_SELECTOR_ID_LENGTH");                       \
  DT_FOREACH_CHILD_STATUS_OKAY(DT_DRV_INST(inst),                              \
                               KP_SELECTOR_MEMBER_ID_LENGTH)                   \
  BUILD_ASSERT(DT_CHILD_NUM_STATUS_OKAY(DT_DRV_INST(inst)) > 0,                \
               "selector requires at least one enabled nested effect");        \
  BUILD_ASSERT(DT_CHILD_NUM_STATUS_OKAY(DT_DRV_INST(inst)) <= UINT16_MAX,      \
               "selector has too many effect slots");                          \
  BUILD_ASSERT(DT_PROP(DT_DRV_INST(inst), initial_effect) >= 0 &&              \
                   DT_PROP(DT_DRV_INST(inst), initial_effect) <= UINT16_MAX,   \
               "selector initial-effect must fit a local slot");               \
  KP_RGB_OVERLAY_TARGET_ARRAYS(inst, kp_selector_##inst);                      \
  static const struct kp_rgb_selector_member kp_selector_##inst##_members[] =  \
      {DT_FOREACH_CHILD_STATUS_OKAY(DT_DRV_INST(inst), KP_SELECTOR_MEMBER)};   \
  static struct kp_rgb_effect_instance kp_selector_##inst##_instances[] = {    \
      DT_FOREACH_CHILD_STATUS_OKAY(DT_DRV_INST(inst), KP_SELECTOR_INSTANCE)};  \
  static bool kp_selector_##inst##_reset_pending[DT_CHILD_NUM_STATUS_OKAY(     \
      DT_DRV_INST(inst))];                                                     \
  static const struct kp_selector_config kp_selector_##inst##_cfg = {          \
      .common = KP_RGB_OVERLAY_COMMON(DT_DRV_INST(inst),                       \
                                      &kp_selector_callbacks, true),           \
  };                                                                           \
  static struct kp_selector_data kp_selector_##inst##_data = {                 \
      KP_RGB_OVERLAY_COMMON_DATA(inst, kp_selector_##inst),                    \
      .selector =                                                              \
          {                                                                    \
              .owner = DEVICE_DT_GET(DT_DRV_INST(inst)),                       \
              .id = KP_SELECTOR_ID(DT_DRV_INST(inst)),                         \
              .members = kp_selector_##inst##_members,                         \
              .effect_count = ARRAY_SIZE(kp_selector_##inst##_members),        \
              .effect_index = DT_PROP(DT_DRV_INST(inst), initial_effect),      \
              .initial_effect = DT_PROP(DT_DRV_INST(inst), initial_effect),    \
          },                                                                   \
      .instances = kp_selector_##inst##_instances,                             \
      .reset_pending = kp_selector_##inst##_reset_pending,                     \
      .live_slot = SIZE_MAX,                                                   \
  };                                                                           \
  KP_RGB_OVERLAY_BEHAVIOR_DEFINE(inst, &kp_rgb_selector_behavior_api,          \
                                 kp_selector_##inst);

#define KP_PRIVATE_DEFINE(inst)                                                \
  KP_RGB_OVERLAY_EFFECT_ASSERT(DT_DRV_INST(inst))                              \
  KP_RGB_OVERLAY_TARGET_ARRAYS(inst, kp_private_##inst);                       \
  static const struct kp_selector_config kp_private_##inst##_cfg = {           \
      .common = KP_RGB_OVERLAY_COMMON(DT_DRV_INST(inst),                       \
                                      &kp_selector_callbacks, true),           \
      .private_effect = KP_RGB_OVERLAY_EFFECT(DT_DRV_INST(inst)),              \
  };                                                                           \
  static struct {                                                              \
    struct kp_rgb_overlay_common_data common;                                  \
  } kp_private_##inst##_data = {                                               \
      KP_RGB_OVERLAY_COMMON_DATA(inst, kp_private_##inst),                     \
  };                                                                           \
  KP_RGB_OVERLAY_DEFINE(inst, kp_private_##inst);

#define KP_COMPOSITION_DEFINE(inst)                                            \
  COND_CODE_1(DT_INST_PROP(inst, controllable), (KP_SELECTOR_DEFINE(inst)),    \
              (KP_PRIVATE_DEFINE(inst)))
DT_INST_FOREACH_STATUS_OKAY(KP_COMPOSITION_DEFINE)

#define KP_SELECTOR_REGISTRY_ENTRY(node_id) DEVICE_DT_GET(node_id),
static const struct device *const kp_selector_registry[] = {
    KP_RGB_FOREACH_SELECTOR(KP_SELECTOR_REGISTRY_ENTRY)};

size_t kp_rgb_selector_registry_count(void) {
  return ARRAY_SIZE(kp_selector_registry);
}

struct kp_rgb_selector *kp_rgb_selector_registry_at(size_t index) {
  if (index >= kp_rgb_selector_registry_count())
    return NULL;
  return &((struct kp_selector_data *)kp_selector_registry[index]->data)->selector;
}

int kp_rgb_selector_overlays_start(void) {
  int result = 0;
  for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++) {
    int err = kp_selector_start(kp_selector_registry[i]);
    if (result == 0)
      result = err;
  }
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
  kp_rgb_selector_apply_defaults_locked(&data->selector);
  data->selector.effect_index = data->selector.initial_effect;
  data->selector.active_fx = NULL;
  data->reset_requested = true;
  zmk_rgb_matrix_flush();
}

void kp_rgb_selector_overlays_defaults_locked(void) {
  for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++)
    kp_selector_defaults_locked(kp_selector_registry[i]);
}

#else

size_t kp_rgb_selector_registry_count(void) { return 0; }

struct kp_rgb_selector *kp_rgb_selector_registry_at(size_t index) {
  ARG_UNUSED(index);
  return NULL;
}

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
