/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT keypaw_rgb_overlay_selector

#include <limits.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>
#include <zmk/rgb_matrix.h>
#include <zmk/rgb_persist.h>

#include "rgb_matrix_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_SETTINGS)

#define KP_RGB_PERSIST_KEY KP_RGB_PERSIST_SUBTREE "/state"

struct kp_selector_load {
  struct kp_rgb_persist_selector_record selection;
  bool selection_pending;
  bool dirty;
  uint32_t revision;
};

static struct kp_selector_load
    kp_selectors[MAX(1, DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT))];
static struct kp_rgb_persist_global_state kp_global_load;
static bool kp_global_pending, kp_global_dirty;
static uint32_t kp_global_revision;

static void kp_rgb_save_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(kp_rgb_save_work, kp_rgb_save_work_handler);

static void kp_path(char *out, const char *selector) {
  snprintk(out, SETTINGS_MAX_NAME_LEN + 1, "%s%s/state",
           KP_RGB_PERSIST_SELECTOR_PREFIX, selector);
}

static int kp_read(size_t len, size_t expected, settings_read_cb read_cb,
                   void *cb_arg, void *out) {
  if (len != expected)
    return -EINVAL;
  int rc = read_cb(cb_arg, out, expected);
  return rc < 0 ? rc : (rc == (int)expected ? 0 : -EINVAL);
}

static int kp_rgb_load_cb(const char *name, size_t len,
                          settings_read_cb read_cb, void *cb_arg) {
  if (strcmp(name, "state") == 0) {
    struct kp_rgb_persist_global_record bytes;
    struct kp_rgb_persist_global_state value;
    int err = kp_read(len, sizeof(bytes), read_cb, cb_arg, &bytes);
    if (err == 0)
      err = kp_rgb_persist_decode_global(&bytes, sizeof(bytes), &value);
    if (err < 0)
      return err;
    kp_rgb_matrix_lock();
    kp_global_load = value;
    kp_global_pending = true;
    kp_rgb_matrix_unlock();
    return 0;
  }

  char path[SETTINGS_MAX_NAME_LEN + 1];
  for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++) {
    const struct kp_rgb_selector *selector = kp_rgb_selector_registry_at(i);
    kp_path(path, selector->id);
    if (strcmp(name, path + sizeof(KP_RGB_PERSIST_SUBTREE)) == 0) {
      struct kp_rgb_persist_selector_record bytes, value;
      int err = kp_read(len, sizeof(bytes), read_cb, cb_arg, &bytes);
      if (err == 0)
        err = kp_rgb_persist_decode_selector(&bytes, sizeof(bytes), &value);
      if (err < 0)
        return err;
      kp_rgb_matrix_lock();
      kp_selectors[i].selection = value;
      kp_selectors[i].selection_pending = true;
      kp_rgb_matrix_unlock();
      return 0;
    }
  }
  return 0;
}

static void kp_dirty_selector_locked(size_t index) {
  kp_selectors[index].dirty = true;
  kp_selectors[index].revision++;
}

static void kp_dirty_global_locked(void) {
  kp_global_dirty = true;
  kp_global_revision++;
}

int kp_rgb_settings_commit(void) {
  kp_rgb_matrix_lock();
  /* Validate readiness before consuming any staged records. */
  for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++) {
    const struct kp_rgb_selector *selector = kp_rgb_selector_registry_at(i);
    if (!device_is_ready(selector->owner)) {
      kp_rgb_matrix_unlock();
      return -ENODEV;
    }
    for (size_t j = 0; j < selector->effect_count; j++) {
      const struct device *effect = selector->members[j].effect;
      if (effect != NULL && !device_is_ready(effect)) {
        kp_rgb_matrix_unlock();
        return -ENODEV;
      }
    }
  }
  if (kp_global_pending) {
    kp_rgb_global.user_on = kp_global_load.user_on;
    kp_rgb_global.brightness = kp_global_load.brightness;
    kp_rgb_reconcile_power_locked();
  }
  for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++) {
    struct kp_rgb_selector *selector = kp_rgb_selector_registry_at(i);
    struct kp_selector_load *load = &kp_selectors[i];
    if (load->selection_pending) {
      size_t slot = selector->initial_effect;
      if (kp_rgb_selector_at(selector, slot) == NULL)
        slot = kp_rgb_selector_calc_index(selector, 0, 0);
      for (size_t j = 0; j < selector->effect_count; j++) {
        const char *id = selector->members[j].id;
        if (id != NULL && strcmp(load->selection.selected_id, id) == 0) {
          slot = j;
          break;
        }
      }
      kp_rgb_selector_select_locked(selector, slot);
    }
    load->selection_pending = false;
  }
  kp_global_pending = false;
  zmk_rgb_matrix_flush();
  kp_rgb_matrix_unlock();
  return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE_WITH_CPRIO(kp_rgb_matrix, KP_RGB_PERSIST_SUBTREE,
                                          NULL, kp_rgb_load_cb,
                                          kp_rgb_settings_commit, NULL,
                                          INT_MAX - 1);

static int kp_save_selector(size_t index, uint32_t *revision) {
  const struct kp_rgb_selector *selector = kp_rgb_selector_registry_at(index);
  struct kp_rgb_persist_selector_record selection;
  kp_rgb_matrix_lock();
  *revision = kp_selectors[index].revision;
  int err = -ENODEV;
  if (device_is_ready(selector->owner) && selector->active_fx != NULL) {
    err = kp_rgb_persist_encode_selector(
        selector->members[selector->effect_index].id, &selection);
  }
  kp_rgb_matrix_unlock();
  if (err < 0)
    return err;
  char path[SETTINGS_MAX_NAME_LEN + 1];
  kp_path(path, selector->id);
  return settings_save_one(path, &selection, sizeof(selection));
}

static int kp_save_global(void) {
  struct kp_rgb_persist_global_record bytes;
  kp_rgb_matrix_lock();
  struct kp_rgb_persist_global_state state = {
      .user_on = kp_rgb_global.user_on, .brightness = kp_rgb_global.brightness};
  kp_rgb_persist_encode_global(&state, &bytes);
  kp_rgb_matrix_unlock();
  return settings_save_one(KP_RGB_PERSIST_KEY, &bytes, sizeof(bytes));
}

static void kp_rgb_save_work_handler(struct k_work *work) {
  ARG_UNUSED(work);
  int err = 0;
  for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++) {
    kp_rgb_matrix_lock();
    bool dirty = kp_selectors[i].dirty;
    kp_rgb_matrix_unlock();
    if (!dirty)
      continue;
    uint32_t revision;
    int rc = kp_save_selector(i, &revision);
    kp_rgb_matrix_lock();
    if (rc == 0 && revision == kp_selectors[i].revision)
      kp_selectors[i].dirty = false;
    if (kp_selectors[i].dirty && err == 0)
      err = rc < 0 ? rc : -EAGAIN;
    kp_rgb_matrix_unlock();
  }
  kp_rgb_matrix_lock();
  bool dirty = kp_global_dirty;
  uint32_t revision = kp_global_revision;
  kp_rgb_matrix_unlock();
  if (dirty) {
    int rc = kp_save_global();
    kp_rgb_matrix_lock();
    if (rc == 0 && revision == kp_global_revision)
      kp_global_dirty = false;
    if (kp_global_dirty && err == 0)
      err = rc < 0 ? rc : -EAGAIN;
    kp_rgb_matrix_unlock();
  }
  if (err < 0) {
    LOG_WRN("RGB settings save incomplete (err %d); retrying", err);
    k_work_reschedule(&kp_rgb_save_work, K_MSEC(1000));
  }
}

#else

int kp_rgb_settings_commit(void) { return 0; }

#endif

int kp_rgb_save_state(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
  kp_rgb_matrix_lock();
  kp_dirty_global_locked();
  kp_rgb_matrix_unlock();
  /* Flash I/O uses the larger system work queue stack. */
  return MIN(k_work_reschedule(&kp_rgb_save_work,
                               K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE)),
             0);
#else
  return 0;
#endif
}

int kp_rgb_save_selector_state(const struct device *selector, bool reset) {
#if IS_ENABLED(CONFIG_SETTINGS)
  bool found = false;
  kp_rgb_matrix_lock();
  for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++) {
    const struct kp_rgb_selector *state = kp_rgb_selector_registry_at(i);
    if (state->owner != selector)
      continue;
    kp_dirty_selector_locked(i);
    if (reset)
      kp_selectors[i].selection_pending = false;
    found = true;
    break;
  }
  kp_rgb_matrix_unlock();
  if (!found)
    return -ENODEV;
  return MIN(k_work_reschedule(&kp_rgb_save_work,
                               K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE)),
             0);
#else
  ARG_UNUSED(selector);
  ARG_UNUSED(reset);
  return 0;
#endif
}

void kp_rgb_reset_state(void) {
  kp_rgb_matrix_lock();
  (void)kp_rgb_apply_defaults();
  kp_rgb_selector_overlays_defaults_locked();
  kp_rgb_request_runtime_reset_locked();
#if IS_ENABLED(CONFIG_SETTINGS)
  kp_global_pending = false;
  for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++) {
    kp_selectors[i].selection_pending = false;
    kp_dirty_selector_locked(i);
  }
  kp_dirty_global_locked();
#endif
  kp_rgb_matrix_unlock();
#if IS_ENABLED(CONFIG_SETTINGS)
  k_work_reschedule(&kp_rgb_save_work, K_NO_WAIT);
#endif
}
