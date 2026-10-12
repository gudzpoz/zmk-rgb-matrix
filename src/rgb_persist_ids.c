/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT keypaw_rgb_overlay_selector

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "rgb_persist_internal.h"

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define KP_PERSIST_ID(node_id)                                                 \
  DT_PROP_OR(node_id, persist_id, DT_NODE_FULL_NAME(node_id))

#define KP_PERSIST_MEMBER(node_id)                                             \
  {.id = KP_PERSIST_ID(node_id),                                               \
   .persist_parameters = DT_NODE_HAS_PROP(node_id, persist_id)},

#define KP_PERSIST_ID_LENGTH(node_id)                                          \
  BUILD_ASSERT(                                                                \
      sizeof(KP_PERSIST_ID(node_id)) <= KP_RGB_PERSIST_MAX_ID_LENGTH,          \
      "RGB persist-id or node name exceeds KP_RGB_PERSIST_MAX_ID_LENGTH");


#define KP_PERSIST_DEVICE(node_id) DEVICE_DT_GET(node_id),
#define KP_PERSIST_DEFINE(inst)                                                \
  KP_PERSIST_ID_LENGTH(DT_DRV_INST(inst))                                      \
  DT_FOREACH_CHILD_STATUS_OKAY(DT_DRV_INST(inst), KP_PERSIST_ID_LENGTH)        \
  static const struct kp_rgb_persist_effect_id kp_persist_##inst##_members[] = \
      {DT_FOREACH_CHILD_STATUS_OKAY(DT_DRV_INST(inst), KP_PERSIST_MEMBER)};    \
  static const struct device *const kp_persist_##inst##_devices[] = {          \
      DT_FOREACH_CHILD_STATUS_OKAY(DT_DRV_INST(inst), KP_PERSIST_DEVICE)};

DT_INST_FOREACH_STATUS_OKAY(KP_PERSIST_DEFINE)

#define KP_PERSIST_SELECTOR(inst)                                              \
  {.id = KP_PERSIST_ID(DT_DRV_INST(inst)),                                     \
   .effects = kp_persist_##inst##_members,                                     \
   .effect_count = ARRAY_SIZE(kp_persist_##inst##_members)},

static const struct kp_rgb_persist_selector_ids kp_persist_selectors[] = {
    DT_INST_FOREACH_STATUS_OKAY(KP_PERSIST_SELECTOR)};

#define KP_PERSIST_OWNER(inst) DEVICE_DT_GET(DT_DRV_INST(inst)),

static const struct device *const kp_persist_owners[] = {
    DT_INST_FOREACH_STATUS_OKAY(KP_PERSIST_OWNER)};

#define KP_PERSIST_DEVICES(inst) kp_persist_##inst##_devices,
static const struct device *const *const kp_persist_devices[] = {
    DT_INST_FOREACH_STATUS_OKAY(KP_PERSIST_DEVICES)};

const struct device *kp_rgb_persist_member_at(size_t selector, size_t member) {
  const struct kp_rgb_persist_selector_ids *ids =
      kp_rgb_persist_selector_at(selector, NULL);
  return ids != NULL && member < ids->effect_count
             ? kp_persist_devices[selector][member] : NULL;
}
size_t kp_rgb_persist_selector_count(void) {
  return ARRAY_SIZE(kp_persist_selectors);
}

const struct kp_rgb_persist_selector_ids *
kp_rgb_persist_selector_at(size_t index, const struct device **owner) {
  if (index >= kp_rgb_persist_selector_count())
    return NULL;
  if (owner != NULL)
    *owner = kp_persist_owners[index];
  return &kp_persist_selectors[index];
}

int kp_rgb_persist_ids_validate(void) {
  for (size_t i = 0; i < kp_rgb_persist_selector_count(); i++) {
    int err = kp_rgb_persist_validate_ids(&kp_persist_selectors[i], 1);
    if (err < 0) {
      LOG_ERR("RGB selector %s persistence IDs invalid (err %d): check syntax, "
              "member uniqueness and scoped key length",
              kp_persist_owners[i]->name, err);
      return err;
    }
  }
  int err = kp_rgb_persist_validate_ids(kp_persist_selectors,
                                        kp_rgb_persist_selector_count());
  if (err < 0)
    LOG_ERR("RGB selector persist-id or node name must be globally unique");
  return err;
}

#else

size_t kp_rgb_persist_selector_count(void) { return 0; }

const struct kp_rgb_persist_selector_ids *
kp_rgb_persist_selector_at(size_t index, const struct device **owner) {
  ARG_UNUSED(index);
  ARG_UNUSED(owner);
  return NULL;
}

int kp_rgb_persist_ids_validate(void) { return 0; }


const struct device *kp_rgb_persist_member_at(size_t selector, size_t member) {
  ARG_UNUSED(selector);
  ARG_UNUSED(member);
  return NULL;
}
#endif
