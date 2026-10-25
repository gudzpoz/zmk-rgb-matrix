/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include "rgb_matrix_internal.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static bool initialized, in_control;

static struct kp_rgb_condition_registration *lookup(const struct device *dev) {
  STRUCT_SECTION_FOREACH(kp_rgb_condition_registration, e) {
    if (e->dev == dev)
      return e;
  }
  return NULL;
}

static bool on_worker(void) {
  return !k_is_in_isr() &&
         k_current_get() ==
             k_work_queue_thread_get(kp_rgb_work_q()) &&
         in_control;
}

static void fault(struct kp_rgb_condition_registration *e, const char *reason) {
  if (!e->fault)
    LOG_ERR("RGB condition %s: %s", e->dev->name, reason);
  e->fault = true;
}

void kp_rgb_condition_invalidate(void) {
  kp_rgb_coordinator_request(KP_RGB_REQUEST_CONTROL);
}

bool kp_rgb_condition_require(const struct device *dev) {
  if (!dev)
    return true;
  struct kp_rgb_condition_registration *e = lookup(dev);
  if (!e) {
    LOG_ERR("Unregistered RGB condition root %s", dev->name);
    return false;
  }
  e->live = true;
  return !e->fault;
}

bool kp_rgb_condition_valid(const struct device *dev) {
  if (!dev)
    return true;
  struct kp_rgb_condition_registration *e = lookup(dev);
  return initialized && e && e->live && !e->fault;
}

bool kp_rgb_condition_value(const struct device *dev) {
  __ASSERT(on_worker(), "condition cache read outside control worker");
  if (!on_worker())
    return false;

  struct kp_rgb_condition_registration *e = lookup(dev);
  return initialized && e && e->live && !e->fault && e->value;
}

static void initialize_conditions(void) {
  STRUCT_SECTION_FOREACH(kp_rgb_condition_registration, e) {
    const struct kp_rgb_condition_api *api = e->dev->api;
    if (!api || !api->sample || api->scope > KP_RGB_CONDITION_CENTRAL_ONLY) {
      fault(e, "invalid provider");
    }
  }

  kp_rgb_overlay_conditions_init();
  kp_rgb_triggers_init();

  struct kp_rgb_condition_registration *e =
      STRUCT_SECTION_END(kp_rgb_condition_registration);
  while (e != STRUCT_SECTION_START(kp_rgb_condition_registration)) {
    --e;
    if (!e->live || e->fault)
      continue;
    const struct kp_rgb_condition_api *api = e->dev->api;
    if (IS_ENABLED(CONFIG_ZMK_SPLIT) &&
        !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) &&
        api->scope == KP_RGB_CONDITION_CENTRAL_ONLY) {
      fault(e, "central-only source used on peripheral");
      continue;
    }
    for (size_t i = 0; i < e->dep_count; i++) {
      e->deps[i]->live = true;
    }
  }

  STRUCT_SECTION_FOREACH(kp_rgb_condition_registration, entry) {
    if (!entry->live || entry->fault)
      continue;
    for (size_t i = 0; i < entry->dep_count; i++) {
      if (entry->deps[i]->fault) {
        fault(entry, "invalid dependency");
        break;
      }
    }
  }
}

static void evaluate(void) {
  STRUCT_SECTION_FOREACH(kp_rgb_condition_registration, e) {
    if (!e->live || e->fault)
      continue;
    const struct kp_rgb_condition_api *api = e->dev->api;
    e->value = api->sample(e->dev);
  }
}

void kp_rgb_conditions_run(void) {
  in_control = true;
  if (!initialized) {
    initialize_conditions();
    initialized = true;
  }

  int64_t now = k_uptime_get();
  evaluate();
  bool changed = kp_rgb_overlay_refresh();
  kp_rgb_overlay_dispatch();
  if (changed)
    zmk_rgb_matrix_flush();
  kp_rgb_triggers_evaluate(now);

  in_control = false;
}

void kp_rgb_conditions_start(void) {
  kp_rgb_coordinator_start();
}
