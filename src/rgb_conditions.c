/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include "rgb_matrix_internal.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zmk/workqueue.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct kp_rgb_condition_registration *registry;
static struct kp_rgb_condition_registration *sampling;
static struct k_spinlock schedule_lock;
static atomic_t external_pending;
static bool started, initialized, in_control;
static size_t condition_count;

static uint64_t refresh_requested;
static void control_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(control_work, control_handler);

static struct kp_rgb_condition_registration *lookup(const struct device *dev) {
  for (struct kp_rgb_condition_registration *e = registry; e; e = e->next) {
    if (e->dev == dev)
      return e;
  }
  return NULL;
}

static bool on_worker(void) {
  return !k_is_in_isr() &&
         k_current_get() ==
             k_work_queue_thread_get(zmk_workqueue_lowprio_work_q()) &&
         in_control;
}

static void fault(struct kp_rgb_condition_registration *e, const char *reason) {
  if (!e->fault)
    LOG_ERR("RGB condition %s: %s", e->dev->name, reason);
  e->fault = true;
  e->deadline = KP_RGB_CONDITION_NEVER;
}

void kp_rgb_condition_register(const struct device *dev,
                               struct kp_rgb_condition_registration *entry) {
  k_spinlock_key_t key = k_spin_lock(&schedule_lock);
  __ASSERT(!started, "condition registered after startup");
  entry->dev = dev;
  entry->deadline = KP_RGB_CONDITION_NEVER;
  entry->next = registry;
  registry = entry;
  condition_count++;
  k_spin_unlock(&schedule_lock, key);
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

  if (on_worker() && sampling) {
    fault(sampling, "notification from sample callback");
    k_spin_unlock(&schedule_lock, key);
    return;
  }

  atomic_set(&e->pending, 1);
  if (!on_worker())
    atomic_set(&external_pending, 1);
  if (started && !on_worker()) {
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &control_work,
                                K_NO_WAIT);
  }
  k_spin_unlock(&schedule_lock, key);
}

void kp_rgb_conditions_request_refresh(uint64_t token) {
  k_spinlock_key_t key = k_spin_lock(&schedule_lock);
  refresh_requested = token;
  atomic_set(&external_pending, 1);
  if (started) {
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &control_work,
                                K_NO_WAIT);
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

  if (sampling) {
    bool declared = false;
    for (size_t i = 0; i < sampling->dep_count; i++) {
      declared |= sampling->deps[i] == dev;
    }
    if (!declared) {
      fault(sampling, "undeclared dependency read");
      return false;
    }
  }

  struct kp_rgb_condition_registration *e = lookup(dev);
  return e && e->sampled && !e->fault && e->value;
}

static void build_graph(void) {
  for (struct kp_rgb_condition_registration *e = registry; e; e = e->next) {
    const struct kp_rgb_condition_api *api = e->dev->api;
    if (!device_is_ready(e->dev) || !api || !api->sample ||
        api->scope > KP_RGB_CONDITION_CENTRAL_ONLY) {
      fault(e, "invalid or unready provider");
      continue;
    }
    if (api->dependencies)
      e->deps = api->dependencies(e->dev, &e->dep_count);
    if (e->dep_count && !e->deps)
      fault(e, "missing dependency metadata");
  }

  kp_rgb_overlay_conditions_init();
  kp_rgb_triggers_init();

  for (size_t pass = 0; pass < condition_count; pass++) {
    for (struct kp_rgb_condition_registration *e = registry; e; e = e->next) {
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
        struct kp_rgb_condition_registration *dep = lookup(e->deps[i]);
        if (!dep)
          fault(e, "unregistered dependency");
        else
          dep->live = true;
      }
    }
  }

  for (size_t pass = 0; pass < condition_count; pass++) {
    for (struct kp_rgb_condition_registration *e = registry; e; e = e->next) {
      if (!e->live || e->resolved)
        continue;
      bool ready = true;
      if (!e->fault) {
        for (size_t i = 0; i < e->dep_count; i++) {
          struct kp_rgb_condition_registration *dep = lookup(e->deps[i]);
          if (!dep || dep->fault) {
            fault(e, "invalid dependency");
            break;
          }
          ready &= dep->resolved;
          e->rank = MAX(e->rank, dep->rank + 1);
        }
      }
      if (ready || e->fault)
        e->resolved = true;
    }
  }

  for (struct kp_rgb_condition_registration *e = registry; e; e = e->next) {
    if (e->live && !e->resolved)
      fault(e, "dependency cycle");
  }
}

static void evaluate(int64_t now) {
  for (struct kp_rgb_condition_registration *e = registry; e; e = e->next) {
    e->dirty = atomic_set(&e->pending, 0) || !e->sampled || e->deadline <= now;
  }

  for (size_t rank = 0; rank < condition_count; rank++) {
    for (struct kp_rgb_condition_registration *e = registry; e; e = e->next) {
      if (!e->live || e->fault || e->rank != rank)
        continue;
      for (size_t i = 0; i < e->dep_count; i++) {
        struct kp_rgb_condition_registration *dep = lookup(e->deps[i]);
        if (!dep || dep->fault) {
          fault(e, "faulted dependency");
          break;
        }
        e->dirty |= dep->dirty;
      }
      if (e->fault || !e->dirty)
        continue;

      int64_t next_wakeup_ms = KP_RGB_CONDITION_NEVER;
      const struct kp_rgb_condition_api *api = e->dev->api;
      sampling = e;
      bool active = api->sample(e->dev, now, &next_wakeup_ms);
      sampling = NULL;

      if (next_wakeup_ms <= now)
        fault(e, "non-future deadline");
      if (e->fault)
        continue;

      e->value = active;
      e->deadline = next_wakeup_ms;
      e->sampled = true;
    }
  }
}

static bool pending(void) {
  for (struct kp_rgb_condition_registration *e = registry; e; e = e->next) {
    if (e->live && !e->fault && atomic_get(&e->pending))
      return true;
  }
  return false;
}

static void control_handler(struct k_work *work) {
  ARG_UNUSED(work);
  in_control = true;
  if (!initialized) {
    build_graph();
    initialized = true;
  }

  atomic_set(&external_pending, 0);
  k_spinlock_key_t refresh_key = k_spin_lock(&schedule_lock);
  uint64_t refresh_token = refresh_requested;
  refresh_requested = 0;
  k_spin_unlock(&schedule_lock, refresh_key);
  if (refresh_token) {
    for (struct kp_rgb_condition_registration *e = registry; e; e = e->next) {
      if (e->live && !e->fault) atomic_set(&e->pending, 1);
    }
  }
  kp_rgb_triggers_begin();

  for (unsigned pass = 0; pass < 8; pass++) {
    int64_t now = k_uptime_get();
    evaluate(now);
    bool changed = kp_rgb_overlay_refresh();
    kp_rgb_overlay_dispatch();
    if (changed)
      zmk_rgb_matrix_flush();
    kp_rgb_triggers_evaluate(now);
    if (!pending() || atomic_get(&external_pending))
      break;
    if (pass == 7)
      kp_rgb_triggers_quarantine();
  }

  in_control = false;
  k_spinlock_key_t key = k_spin_lock(&schedule_lock);
  bool refresh_complete = refresh_token && !pending();
  if (refresh_token && !refresh_complete && !refresh_requested) {
    refresh_requested = refresh_token;
  }
  int64_t next = KP_RGB_CONDITION_NEVER;
  for (struct kp_rgb_condition_registration *e = registry; e; e = e->next) {
    if (e->live && !e->fault)
      next = MIN(next, e->deadline);
  }

  if (refresh_requested || pending() || atomic_get(&external_pending)) {
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &control_work,
                                K_NO_WAIT);
  } else if (next != KP_RGB_CONDITION_NEVER) {
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &control_work,
                                K_MSEC(MAX(INT64_C(0), next - k_uptime_get())));
  }
  k_spin_unlock(&schedule_lock, key);
  if (refresh_complete) kp_rgb_conditions_refreshed(refresh_token);
}

void kp_rgb_conditions_start(void) {
  k_spinlock_key_t key = k_spin_lock(&schedule_lock);
  if (!started) {
    started = true;
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &control_work,
                                K_NO_WAIT);
  }
  k_spin_unlock(&schedule_lock, key);
}
