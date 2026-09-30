/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Trigger evaluator: owns the ordered table declared by keypaw,rgb-trigger-table
 * and runs each trigger's `bindings` on its rising edge and its `on-exit` on its
 * falling edge, in declaration order. The engine samples the table every render
 * tick, so a condition with no layer event (Caps Lock, a latch) still reaches
 * it, and any change is picked up within a tick. Built for the split central
 * only, as layer state does not exist on a peripheral.
 */

#define DT_DRV_COMPAT keypaw_rgb_trigger_table

#include <stddef.h>

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/behavior.h>
#include <zmk/rgb_matrix.h>

#include "rgb_matrix_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) && (DT_CHILD_NUM_STATUS_OKAY(DT_DRV_INST(0)) > 0)

#define KP_TRIG_TABLE DT_DRV_INST(0)

/* One child of the table, resolved at build time. The table binding's
 * `child-binding` supplies the compatible-less schema; a compound literal holds
 * each variable-length binding array so a child needs no C identifier. */
struct kp_trig {
  const char *name;
  const struct device *condition; /* NULL = always active */
  const struct zmk_behavior_binding *bindings;
  size_t bindings_len;
  const struct zmk_behavior_binding *exit_bindings; /* NULL when absent */
  size_t exit_bindings_len;
};

/* ZMK_KEYMAP_EXTRACT_BINDING hardcodes the `bindings` property, so `on-exit`
 * needs its own extraction. */
#define KP_TRIG_EXIT_BINDING(idx, node_id)                                     \
  {.behavior_dev = DEVICE_DT_NAME(DT_PHANDLE_BY_IDX(node_id, on_exit, idx)),   \
   .param1 = COND_CODE_0(DT_PHA_HAS_CELL_AT_IDX(node_id, on_exit, idx, param1),\
                         (0), (DT_PHA_BY_IDX(node_id, on_exit, idx, param1))), \
   .param2 = COND_CODE_0(DT_PHA_HAS_CELL_AT_IDX(node_id, on_exit, idx, param2),\
                         (0), (DT_PHA_BY_IDX(node_id, on_exit, idx, param2)))}

/* Declaration order is the firing order; there is no `triggers` phandle list on
 * purpose. See docs/development.md#why-there-is-no-triggers-list. */
#define KP_TRIG_ONE_CHILD(node_id)                                             \
  {.name = DT_NODE_FULL_NAME(node_id),                                         \
   .condition = KP_RGB_CONDITION_PTR(node_id),                                 \
   .bindings = (const struct zmk_behavior_binding[]){                          \
       LISTIFY(DT_PROP_LEN(node_id, bindings),                                 \
               ZMK_KEYMAP_EXTRACT_BINDING, (, ), node_id)},                    \
   .bindings_len = DT_PROP_LEN(node_id, bindings),                             \
   .exit_bindings =                                                            \
       COND_CODE_1(DT_NODE_HAS_PROP(node_id, on_exit),                         \
                   ((const struct zmk_behavior_binding[]){                     \
                       LISTIFY(DT_PROP_LEN(node_id, on_exit),                  \
                               KP_TRIG_EXIT_BINDING, (, ), node_id)}),         \
                   (NULL)),                                                    \
   .exit_bindings_len = DT_PROP_LEN_OR(node_id, on_exit, 0)},

static const struct kp_trig kp_triggers[] = {
    DT_FOREACH_CHILD_STATUS_OKAY(KP_TRIG_TABLE, KP_TRIG_ONE_CHILD)};

/* One flag per trigger, so each keeps its own rising/falling edge.
   BSS-zero-ed. */
static bool kp_triggers_prev[ARRAY_SIZE(kp_triggers)];
/* Mutex to prevent initialization race condition. */
static K_MUTEX_DEFINE(kp_triggers_lock);

static bool kp_trig_active(const struct kp_trig *t) {
  if (t->condition == NULL) {
    return true;
  }
  const struct kp_rgb_condition_api *api =
      (const struct kp_rgb_condition_api *)t->condition->api;
  return api != NULL && api->active != NULL && api->active(t->condition);
}

static void kp_triggers_run(const char *name,
                            const struct zmk_behavior_binding *bindings,
                            size_t len) {
  if (bindings == NULL) {
    return;
  }

  /* The &kprgb handlers ignore the event; only the converted binding travels
   * the split link. */
  struct zmk_behavior_binding_event event = {.timestamp = k_uptime_get()};

  for (size_t i = 0; i < len; i++) {
    int err = zmk_behavior_invoke_binding(&bindings[i], event, true);
    if (err < 0) {
      LOG_WRN("Trigger %s binding %u failed (err %d)", name, (uint32_t)i, err);
    }
  }
}

struct kp_trig_edge {
  const struct kp_trig *trig;
  bool rise;
};

static void kp_triggers_evaluate(void) {
  bool now[ARRAY_SIZE(kp_triggers)];
  for (size_t i = 0; i < ARRAY_SIZE(kp_triggers); i++) {
    now[i] = kp_trig_active(&kp_triggers[i]);
  }

  struct kp_trig_edge edges[ARRAY_SIZE(kp_triggers)];
  size_t edge_count = 0;
  k_mutex_lock(&kp_triggers_lock, K_FOREVER);
  for (size_t i = 0; i < ARRAY_SIZE(kp_triggers); i++) {
    if (now[i] != kp_triggers_prev[i]) {
      kp_triggers_prev[i] = now[i];
      edges[edge_count++] = (struct kp_trig_edge){&kp_triggers[i], now[i]};
    }
  }
  k_mutex_unlock(&kp_triggers_lock);

  for (size_t j = 0; j < edge_count; j++) {
    const struct kp_trig *t = edges[j].trig;
    LOG_DBG("RGB trigger %s %s", t->name, edges[j].rise ? "on" : "off");
    kp_triggers_run(t->name, edges[j].rise ? t->bindings : t->exit_bindings,
                    edges[j].rise ? t->bindings_len : t->exit_bindings_len);
  }
}
void kp_rgb_triggers_poll(void) { kp_triggers_evaluate(); }

/* Evaluate once at boot, after the behavior's POST_KERNEL init applied
 * `initial-effect`. Every condition true at boot is a rising edge from the
 * zero-initial flags, so an unconditional trigger acts immediately. */
static int kp_triggers_init(void) {
  kp_triggers_evaluate();
  return 0;
}
SYS_INIT(kp_triggers_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#else
/* CONFIG_KEYPAW_RGB_TRIGGERS is on but there is no table to sample. */
void kp_rgb_triggers_poll(void) {}
#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
