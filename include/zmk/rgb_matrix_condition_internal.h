/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/device.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/util.h>

/* Private declaration storage; not a provider API. */
struct kp_rgb_condition_registration {
  const struct device *dev;
  struct kp_rgb_condition_registration *const *deps;
  size_t dep_count;

  bool live;
  bool fault;
  bool value;
};

/* The iterable section sorts these names lexically into dependency order. */
#define KP_RGB_CONDITION_REGISTRATION(node_id)                                 \
  UTIL_CAT(kp_rgb_condition_, DT_DEP_ORD_STR_SORTABLE(node_id))
#define KP_RGB_CONDITION_DEPS(node_id)                                         \
  UTIL_CAT(KP_RGB_CONDITION_REGISTRATION(node_id), _deps)

#define KP_RGB_CONDITION_DEP_DECLARE(node_id, prop, idx)                       \
  BUILD_ASSERT(                                                                \
      DT_NODE_HAS_STATUS(DT_PHANDLE_BY_IDX(node_id, prop, idx), okay),         \
      "condition dependency must be enabled");                                 \
  BUILD_ASSERT(DT_DEP_ORD(DT_PHANDLE_BY_IDX(node_id, prop, idx)) <             \
                   DT_DEP_ORD(node_id),                                        \
               "condition dependency must precede its consumer");              \
  extern struct kp_rgb_condition_registration KP_RGB_CONDITION_REGISTRATION(   \
      DT_PHANDLE_BY_IDX(node_id, prop, idx));
#define KP_RGB_CONDITION_DEP_PTR(node_id, prop, idx)                           \
  &KP_RGB_CONDITION_REGISTRATION(DT_PHANDLE_BY_IDX(node_id, prop, idx)),

#define KP_RGB_CONDITION_REGISTRATION_DEFINE(node_id)                          \
  COND_CODE_0(                                                                 \
      DT_PROP_LEN_OR(node_id, conditions, 0), (),                              \
      (DT_FOREACH_PROP_ELEM(node_id, conditions,                               \
                            KP_RGB_CONDITION_DEP_DECLARE) static struct        \
       kp_rgb_condition_registration *const KP_RGB_CONDITION_DEPS(             \
           node_id)[] = {DT_FOREACH_PROP_ELEM(node_id, conditions,             \
                                              KP_RGB_CONDITION_DEP_PTR)};))    \
  STRUCT_SECTION_ITERABLE(kp_rgb_condition_registration,                       \
                          KP_RGB_CONDITION_REGISTRATION(node_id)) = {          \
      .dev = DEVICE_DT_GET(node_id),                                           \
      .deps = COND_CODE_0(DT_PROP_LEN_OR(node_id, conditions, 0), (NULL),      \
                          (KP_RGB_CONDITION_DEPS(node_id))),                   \
      .dep_count = DT_PROP_LEN_OR(node_id, conditions, 0),                     \
  };
