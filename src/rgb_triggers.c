/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Trigger evaluator: owns the ordered table declared by keypaw,rgb-trigger-table
 * and invokes the winning trigger's bindings whenever the winner changes. Built
 * for the split central only, as layer state does not exist on a peripheral.
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

/* Declaration order is the precedence order; there is no `triggers` phandle list
 * on purpose. See docs/development.md#why-there-is-no-triggers-list. */
#define KP_TRIGGERS_ONE_CHILD(node_id) DEVICE_DT_GET(node_id),

static const struct device *const kp_triggers[] = {DT_FOREACH_CHILD_STATUS_OKAY(
    KP_TRIG_TABLE, KP_TRIGGERS_ONE_CHILD)};

/* The trigger that won the previous evaluation; only a change re-fires. */
static const struct device *kp_last_winner;

static void kp_triggers_apply(const struct device *winner) {
  const struct kp_rgb_trigger_common_config *cfg = winner->config;

  /* The &kprgb handlers ignore the event; only the converted binding travels
   * the split link. */
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

/* Evaluate once at boot, after the behavior's POST_KERNEL init applied
 * `initial-effect`, so a catch-all trigger takes effect immediately.
 * `kp_last_winner` starts NULL, so the first winner always fires. */
static int kp_triggers_init(void) {
  kp_triggers_evaluate();
  return 0;
}
SYS_INIT(kp_triggers_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
