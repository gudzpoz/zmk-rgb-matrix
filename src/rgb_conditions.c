/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include "rgb_matrix_internal.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct k_spinlock schedule_lock;
static atomic_t external_pending;
static bool started, initialized, in_control;

static uint64_t refresh_requested;
static void control_handler(struct k_work *work);

static K_WORK_DEFINE(control_work, control_handler);

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

void kp_rgb_condition_invalidate(const struct device *dev) {
  k_spinlock_key_t key = k_spin_lock(&schedule_lock);
  struct kp_rgb_condition_registration *e = lookup(dev);
  if (!e) {
    bool error = started || !dev;
    k_spin_unlock(&schedule_lock, key);
    __ASSERT(!error, "unregistered RGB condition");
    if (error)
      LOG_ERR("Invalid RGB condition notification");
    return;
  }

  atomic_set(&e->pending, 1);
  if (!on_worker())
    atomic_set(&external_pending, 1);
  if (started && !on_worker()) {
    k_work_submit_to_queue(kp_rgb_work_q(), &control_work);
  }
  k_spin_unlock(&schedule_lock, key);
}

void kp_rgb_conditions_request_refresh(uint64_t token) {
  k_spinlock_key_t key = k_spin_lock(&schedule_lock);
  refresh_requested = token;
  atomic_set(&external_pending, 1);
  if (started) {
    k_work_submit_to_queue(kp_rgb_work_q(), &control_work);
  }
  k_spin_unlock(&schedule_lock, key);
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
  return e && e->live && e->sampled && !e->fault;
}

bool kp_rgb_condition_value(const struct device *dev) {
  __ASSERT(on_worker(), "condition cache read outside control worker");
  if (!on_worker())
    return false;

  struct kp_rgb_condition_registration *e = lookup(dev);
  return e && e->sampled && !e->fault && e->value;
}

static void initialize_conditions(void) {
  STRUCT_SECTION_FOREACH(kp_rgb_condition_registration, e) {
    const struct kp_rgb_condition_api *api = e->dev->api;
    if (!device_is_ready(e->dev) || !api || !api->sample ||
        api->scope > KP_RGB_CONDITION_CENTRAL_ONLY) {
      fault(e, "invalid or unready provider");
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
    e->dirty = atomic_set(&e->pending, 0) || !e->sampled;
  }

  STRUCT_SECTION_FOREACH(kp_rgb_condition_registration, e) {
    if (!e->live || e->fault)
      continue;
    for (size_t i = 0; i < e->dep_count; i++) {
      struct kp_rgb_condition_registration *dep = e->deps[i];
      if (dep->fault) {
        fault(e, "faulted dependency");
        break;
      }
      e->dirty |= dep->dirty;
    }
    if (e->fault || !e->dirty)
      continue;

    const struct kp_rgb_condition_api *api = e->dev->api;
    bool active = api->sample(e->dev);
    e->value = active;
    e->sampled = true;
  }
}

static bool pending(void) {
  STRUCT_SECTION_FOREACH(kp_rgb_condition_registration, e) {
    if (e->live && !e->fault && atomic_get(&e->pending))
      return true;
  }
  return false;
}

static void control_handler(struct k_work *work) {
  ARG_UNUSED(work);
  in_control = true;
  if (!initialized) {
    initialize_conditions();
    initialized = true;
  }

  atomic_set(&external_pending, 0);
  k_spinlock_key_t refresh_key = k_spin_lock(&schedule_lock);
  uint64_t refresh_token = refresh_requested;
  refresh_requested = 0;
  k_spin_unlock(&schedule_lock, refresh_key);
  if (refresh_token) {
    STRUCT_SECTION_FOREACH(kp_rgb_condition_registration, e) {
      if (e->live && !e->fault) atomic_set(&e->pending, 1);
    }
  }
  int64_t now = k_uptime_get();
  evaluate();
  bool changed = kp_rgb_overlay_refresh();
  kp_rgb_overlay_dispatch();
  if (changed)
    zmk_rgb_matrix_flush();
  kp_rgb_triggers_evaluate(now);

  in_control = false;
  k_spinlock_key_t key = k_spin_lock(&schedule_lock);
  bool refresh_complete = refresh_token && !pending() && !atomic_get(&external_pending);
  if (refresh_token && !refresh_complete && !refresh_requested) {
    refresh_requested = refresh_token;
  }
  if (refresh_requested || pending() || atomic_get(&external_pending)) {
    k_work_submit_to_queue(kp_rgb_work_q(), &control_work);
  }
  k_spin_unlock(&schedule_lock, key);
  if (refresh_complete) kp_rgb_conditions_refreshed(refresh_token);
}

void kp_rgb_conditions_start(void) {
  k_spinlock_key_t key = k_spin_lock(&schedule_lock);
  if (!started) {
    started = true;
    k_work_submit_to_queue(kp_rgb_work_q(), &control_work);
  }
  k_spin_unlock(&schedule_lock, key);
}
