/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT keypaw_rgb_trigger_table

#include <stddef.h>

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/rgb_matrix.h>

#include "rgb_matrix_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) && \
    (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

struct kp_trig_action {
  struct zmk_behavior_binding binding;
  bool latch;
};

struct kp_trig {
  const char *name;
  const struct device *condition;
  const struct kp_trig_action *enter;
  size_t enter_len;
  const struct kp_trig_action *exit;
  size_t exit_len;
  bool baseline;
};

#define KP_TRIG_ACTION(idx, node_id, prop)                                     \
  {.binding = {.behavior_dev =                                                 \
                   DEVICE_DT_NAME(DT_PHANDLE_BY_IDX(node_id, prop, idx)),      \
               .param1 = COND_CODE_0(                                          \
                   DT_PHA_HAS_CELL_AT_IDX(node_id, prop, idx, param1), (0),    \
                   (DT_PHA_BY_IDX(node_id, prop, idx, param1))),               \
               .param2 = COND_CODE_0(                                          \
                   DT_PHA_HAS_CELL_AT_IDX(node_id, prop, idx, param2), (0),    \
                   (DT_PHA_BY_IDX(node_id, prop, idx, param2)))},              \
   .latch = DT_NODE_HAS_COMPAT(DT_PHANDLE_BY_IDX(node_id, prop, idx),          \
                               keypaw_behavior_rgb_overlay_toggle)}

#define KP_TRIG_ACTIONS(node_id, prop)                                         \
  COND_CODE_1(                                                                 \
      DT_NODE_HAS_PROP(node_id, prop),                                         \
      ((const struct kp_trig_action[]){LISTIFY(                                \
          DT_PROP_LEN(node_id, prop), KP_TRIG_ACTION, (, ), node_id, prop)}),  \
      (NULL))

#define KP_TRIG_ASSERT(node_id)                                                \
  BUILD_ASSERT(!DT_NODE_HAS_PROP(node_id, bindings),                           \
               "RGB triggers use on-enter, not bindings");                     \
  BUILD_ASSERT(DT_PROP_LEN_OR(node_id, on_enter, 0) +                          \
                       DT_PROP_LEN_OR(node_id, on_exit, 0) >                   \
                   0,                                                          \
               "RGB triggers require at least one action");
#define KP_TRIG_ASSERT_TABLE(inst)                                             \
  DT_FOREACH_CHILD_STATUS_OKAY(DT_DRV_INST(inst), KP_TRIG_ASSERT)
DT_INST_FOREACH_STATUS_OKAY(KP_TRIG_ASSERT_TABLE)

#define KP_TRIG_ONE_CHILD(node_id)                                             \
  {.name = DT_NODE_FULL_NAME(node_id),                                         \
   .condition = KP_RGB_CONDITION_PTR(node_id),                                 \
   .enter = KP_TRIG_ACTIONS(node_id, on_enter),                                \
   .enter_len = DT_PROP_LEN_OR(node_id, on_enter, 0),                          \
   .exit = KP_TRIG_ACTIONS(node_id, on_exit),                                  \
   .exit_len = DT_PROP_LEN_OR(node_id, on_exit, 0),                            \
   .baseline = DT_ENUM_IDX(node_id, startup) == 1},
#define KP_TRIG_TABLE(inst)                                                    \
  DT_FOREACH_CHILD_STATUS_OKAY(DT_DRV_INST(inst), KP_TRIG_ONE_CHILD)

static const struct kp_trig kp_triggers[] = {
    DT_INST_FOREACH_STATUS_OKAY(KP_TRIG_TABLE)};

/* All entry points run serialized on the control worker, outside engine locks. */
static struct {
  bool disabled;
  bool initialized;
  bool previous;
  bool participant;
} kp_trigger_state[ARRAY_SIZE(kp_triggers)];
static bool kp_triggers_ready;

static bool kp_trig_actions_valid(const struct kp_trig_action *actions, size_t len) {
  for (size_t i = 0; i < len; i++) {
    const struct device *dev = zmk_behavior_get_binding(actions[i].binding.behavior_dev);
    if (dev == NULL || !device_is_ready(dev)) {
      return false;
    }
    if (actions[i].latch) {
      continue;
    }
    bool supported = false;
    for (size_t j = 0; j < kp_rgb_behavior_count(); j++) {
      const struct kp_rgb_behavior_context *ctx = kp_rgb_behavior_at(j);
      if (ctx->dev == dev) {
        supported = true;
        break;
      }
      for (size_t k = 0; k < kp_rgb_effect_count(ctx); k++) {
        const struct behavior_driver_api *api = dev->api;
        if (kp_rgb_effect_at(ctx, k) == dev && api != NULL &&
            api->binding_convert_central_state_dependent_params ==
                kp_rgb_effect_convert_central_state_dependent_params) {
          supported = true;
          break;
        }
      }
      if (supported) {
        break;
      }
    }
    if (!supported) {
      return false;
    }
  }
  return true;
}

void kp_rgb_triggers_init(void) {
  if (kp_triggers_ready) {
    return;
  }
  for (size_t i = 0; i < ARRAY_SIZE(kp_triggers); i++) {
    const struct kp_trig *t = &kp_triggers[i];
    bool valid = kp_trig_actions_valid(t->enter, t->enter_len) &&
                 kp_trig_actions_valid(t->exit, t->exit_len);
    if (valid && t->condition != NULL) {
      valid = kp_rgb_condition_require(t->condition);
    }
    kp_trigger_state[i].disabled = !valid;
    if (!valid) {
      LOG_ERR("RGB trigger %s disabled: invalid action or condition", t->name);
    }
  }
  kp_triggers_ready = true;
}

void kp_rgb_triggers_begin(void) {
  for (size_t i = 0; i < ARRAY_SIZE(kp_triggers); i++) {
    kp_trigger_state[i].participant = false;
  }
}

static void kp_triggers_run(const struct kp_trig *t,
                            const struct kp_trig_action *actions, size_t len,
                            int64_t now_ms) {
  struct zmk_behavior_binding_event event = {.timestamp = now_ms};
  for (size_t i = 0; i < len; i++) {
    int err = zmk_behavior_invoke_binding(&actions[i].binding, event, true);
    if (err < 0) {
      LOG_WRN("Trigger %s binding %u failed (err %d)", t->name, (uint32_t)i, err);
    }
  }
}

bool kp_rgb_triggers_evaluate(int64_t now_ms) {
  if (!kp_triggers_ready) {
    return false;
  }
  int8_t edges[MAX(1, ARRAY_SIZE(kp_triggers))] = {0};
  for (size_t i = 0; i < ARRAY_SIZE(kp_triggers); i++) {
    const struct kp_trig *t = &kp_triggers[i];
    if (kp_trigger_state[i].disabled) {
      continue;
    }
    if (!kp_rgb_condition_valid(t->condition)) {
      kp_trigger_state[i].disabled = true;
      LOG_ERR("RGB trigger %s disabled: invalid condition snapshot", t->name);
      continue;
    }
    bool active = t->condition == NULL || kp_rgb_condition_value(t->condition);
    if (!kp_trigger_state[i].initialized) {
      if (!t->baseline && active) {
        edges[i] = 1;
      }
      kp_trigger_state[i].initialized = true;
    } else if (active != kp_trigger_state[i].previous) {
      edges[i] = active ? 1 : -1;
    }
    kp_trigger_state[i].previous = active;
  }

  bool dispatched = false;
  for (size_t i = 0; i < ARRAY_SIZE(kp_triggers); i++) {
    if (edges[i] == 0) {
      continue;
    }
    const struct kp_trig *t = &kp_triggers[i];
    const struct kp_trig_action *actions = edges[i] > 0 ? t->enter : t->exit;
    size_t len = edges[i] > 0 ? t->enter_len : t->exit_len;
    if (len == 0) {
      continue;
    }
    kp_trigger_state[i].participant = true;
    dispatched = true;
    kp_triggers_run(t, actions, len, now_ms);
  }
  return dispatched;
}

void kp_rgb_triggers_quarantine(void) {
  for (size_t i = 0; i < ARRAY_SIZE(kp_triggers); i++) {
    if (kp_trigger_state[i].participant) {
      kp_trigger_state[i].disabled = true;
      LOG_ERR("RGB trigger %s quarantined: feedback did not settle", kp_triggers[i].name);
    }
  }
}

#else
void kp_rgb_triggers_init(void) {}
void kp_rgb_triggers_begin(void) {}
bool kp_rgb_triggers_evaluate(int64_t now_ms) {
  ARG_UNUSED(now_ms);
  return false;
}
void kp_rgb_triggers_quarantine(void) {}
#endif
