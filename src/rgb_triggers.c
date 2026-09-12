/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Trigger evaluator. Owns the ordered table declared by keypaw,rgb-trigger-table
 * and invokes the winning trigger's bindings whenever the winner changes.
 *
 * Modelled on zmk/app/src/conditional_layer.c: a listener on
 * zmk_layer_state_changed, built only for the split central because layer state
 * does not exist on a peripheral. It differs from conditional layers in one
 * important way: a then-layer can be activated *and deactivated*, whereas there
 * is no way to "deactivate" an effect. So a trigger asserts on the transition
 * into winning and is otherwise left alone. That edge behaviour is what lets a
 * manual effect selection, or a relative command such as RGB_HUI, survive until
 * the mapping actually changes instead of being re-fired on every layer event.
 */

#define DT_DRV_COMPAT keypaw_rgb_trigger_table

#include <stddef.h>

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/rgb_matrix.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) && (DT_CHILD_NUM_STATUS_OKAY(DT_DRV_INST(0)) > 0)

#define KP_TRIG_TABLE DT_DRV_INST(0)

/* Declaration order is the precedence order. There is no `triggers` phandle
 * list on purpose: a parent referencing its own child is a devicetree cycle
 * (every node depends on its parent), which edtlib rejects outright or, if the
 * child is also a phandle target, silently gives both nodes an ordinal of -1.
 * This mirrors zmk/app/src/conditional_layer.c, which enumerates its configs
 * with DT_INST_FOREACH_CHILD for the same reason. */
#define KP_TRIGGERS_ONE_CHILD(node_id) DEVICE_DT_GET(node_id),

static const struct device *const kp_triggers[] = {DT_FOREACH_CHILD_STATUS_OKAY(
    KP_TRIG_TABLE, KP_TRIGGERS_ONE_CHILD)};

/* The trigger that won the previous evaluation; only a change re-fires. */
static const struct device *kp_last_winner;

static void kp_triggers_apply(const struct device *winner) {
  const struct kp_rgb_trigger_common_config *cfg = winner->config;

  /* A mostly-zeroed event is fine: the &kprgb handlers ignore it, and only the
   * already-converted binding travels the split link. */
  struct zmk_behavior_binding_event event = {.timestamp = k_uptime_get()};

  for (size_t i = 0; i < cfg->bindings_len; i++) {
    int err = zmk_behavior_invoke_binding(&cfg->bindings[i], event, true);
    if (err < 0) {
      LOG_WRN("Trigger %s binding %u failed (err %d)", winner->name, (uint32_t)i, err);
    }
  }
}

static void kp_triggers_evaluate(void) {
  const struct device *winner = NULL;

  for (size_t i = 0; i < ARRAY_SIZE(kp_triggers); i++) {
    const struct kp_rgb_trigger_api *api = kp_triggers[i]->api;
    if (api != NULL && api->active != NULL && api->active(kp_triggers[i])) {
      winner = kp_triggers[i];
      break;
    }
  }

  if (winner == kp_last_winner) {
    return;
  }

  LOG_DBG("RGB trigger winner: %s", winner != NULL ? winner->name : "(none)");
  kp_last_winner = winner;

  if (winner != NULL) {
    kp_triggers_apply(winner);
  }
}

static int kp_triggers_listener(const zmk_event_t *eh) {
  if (as_zmk_layer_state_changed(eh) != NULL) {
    kp_triggers_evaluate();
  }
  return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(kp_rgb_triggers, kp_triggers_listener);
ZMK_SUBSCRIPTION(kp_rgb_triggers, zmk_layer_state_changed);

/* Evaluate once at boot. This runs at APPLICATION, after the behavior's
 * POST_KERNEL init has applied `initial-effect`, so a layer-0 or catch-all
 * trigger takes effect immediately rather than waiting for the first layer
 * change. `kp_last_winner` starts NULL, so the first winner always fires. */
static int kp_triggers_init(void) {
  kp_triggers_evaluate();
  return 0;
}
SYS_INIT(kp_triggers_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
