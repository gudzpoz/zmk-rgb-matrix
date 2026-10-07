/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

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

#define KP_RGB_PERSIST_SUBTREE "keypaw/rgb_matrix"
#define KP_RGB_PERSIST_KEY KP_RGB_PERSIST_SUBTREE "/state"
#define KP_RGB_PERSIST_EFFECT_PREFIX "state/effects/"
#define KP_RGB_PERSIST_EFFECT_PATH KP_RGB_PERSIST_SUBTREE "/state/effects/"

static const char *kp_rgb_selected_id(void) {
  const struct device *fx = kp_rgb_controller.state.active_fx;
  if (fx == NULL) {
    return NULL;
  }
  return kp_rgb_effect_cfg(fx)->persist_id;
}

static int kp_rgb_effect_path(char *out, size_t out_len, const char *id) {
  int len = snprintk(out, out_len, "%s%s", KP_RGB_PERSIST_EFFECT_PATH, id);
  return len < 0 || (size_t)len >= out_len ? -ENAMETOOLONG : 0;
}

static void kp_rgb_save_effects(void);
static void kp_rgb_encode(struct kp_rgb_persist_blob *blob) {
  bool user_on;
  uint8_t brightness;
  const char *selected_id;

  kp_rgb_matrix_lock();
  user_on = kp_rgb_controller.state.user_on;
  brightness = kp_rgb_controller.brightness;
  selected_id = kp_rgb_selected_id();
  kp_rgb_persist_pack(user_on, brightness, selected_id, blob);
  kp_rgb_matrix_unlock();
}

static void kp_rgb_save_work_handler(struct k_work *work) {
  ARG_UNUSED(work);
  struct kp_rgb_persist_blob blob;
  kp_rgb_encode(&blob);
  int err = settings_save_one(KP_RGB_PERSIST_KEY, &blob, sizeof(blob));
  if (err < 0) {
    LOG_WRN("Failed to persist RGB state (err %d)", err);
  }
  kp_rgb_save_effects();
}
static void kp_rgb_save_effects(void) {
  for (size_t i = 0; i < kp_rgb_effect_count(); i++) {
    const struct device *fx = kp_rgb_effect_at(i);
    const char *id = fx == NULL ? NULL : kp_rgb_effect_cfg(fx)->persist_id;
    char path[sizeof(KP_RGB_PERSIST_EFFECT_PATH) + KP_RGB_PERSIST_MAX_ID_LENGTH];
    if (fx == NULL || !kp_rgb_effect_cfg(fx)->persist_parameters ||
        kp_rgb_effect_path(path, sizeof(path), id) < 0) {
      continue;
    }
    struct kp_rgb_persist_effect record;
    kp_rgb_matrix_lock();
    const struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(fx);
    record = (struct kp_rgb_persist_effect){
        .duration_ms = data->duration_ms, .h = data->color.h, .s = data->color.s};
    kp_rgb_matrix_unlock();
    int err = settings_save_one(path, &record, sizeof(record));
    if (err < 0) {
      LOG_WRN("Failed to persist RGB effect %s (err %d)", id, err);
    }
  }
}

K_WORK_DELAYABLE_DEFINE(kp_rgb_save_work, kp_rgb_save_work_handler);

static int kp_rgb_find_id(const char *id) {
  for (size_t i = 0; i < kp_rgb_effect_count(); i++) {
    const struct device *fx = kp_rgb_effect_at(i);
    if (fx != NULL && kp_rgb_effect_cfg(fx)->persist_id != NULL &&
        strcmp(kp_rgb_effect_cfg(fx)->persist_id, id) == 0) {
      return (int)i;
    }
  }
  return -ENOENT;
}

static int kp_rgb_load_cb(const char *name, size_t len,
                          settings_read_cb read_cb, void *cb_arg) {
  if (strncmp(name, KP_RGB_PERSIST_EFFECT_PREFIX,
              sizeof(KP_RGB_PERSIST_EFFECT_PREFIX) - 1) == 0) {
    const char *id = name + sizeof(KP_RGB_PERSIST_EFFECT_PREFIX) - 1;
    if (len != sizeof(struct kp_rgb_persist_effect)) {
      return -EINVAL;
    }
    struct kp_rgb_persist_effect record;
    int rc = read_cb(cb_arg, &record, sizeof(record));
    if (rc != sizeof(record) || !kp_rgb_persist_effect_valid(&record)) {
      return rc < 0 ? rc : -EINVAL;
    }
    int index = kp_rgb_find_id(id);
    if (index < 0) {
      return 0;
    }
    const struct device *fx = kp_rgb_effect_at((size_t)index);
    kp_rgb_matrix_lock();
    struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(fx);
    data->color.h = record.h == KP_RGB_HUE_MAX ? 0 : record.h;
    data->color.s = record.s;
    data->duration_ms = kp_rgb_persist_clamp_duration(
        record.duration_ms, CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS,
        CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
    zmk_rgb_matrix_flush();
    kp_rgb_matrix_unlock();
    return 0;
  }

  if (strcmp(name, "state") != 0 || !kp_rgb_persist_size_ok(len)) {
    return -ENOENT;
  }

  struct kp_rgb_persist_blob blob;
  int rc = read_cb(cb_arg, &blob, sizeof(blob));
  if (rc < 0) {
    return rc;
  }
  if (rc != sizeof(blob) || !kp_rgb_persist_version_ok(blob.version) ||
      blob.has_selected > 1 || blob.selected_id[sizeof(blob.selected_id) - 1] != '\0' ||
      blob.brightness > KP_RGB_BRT_MAX) {
    return -EINVAL;
  }

  kp_rgb_matrix_lock();
  kp_rgb_controller.brightness = blob.brightness;
  kp_rgb_controller.state.user_on = blob.user_on != 0;
  if (blob.has_selected) {
    int index = kp_rgb_find_id(blob.selected_id);
    if (index >= 0) {
      (void)kp_rgb_select_effect_locked((uint16_t)index);
    }
  }
  kp_rgb_reconcile_power_locked();
  zmk_rgb_matrix_flush();
  kp_rgb_matrix_unlock();
  return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(kp_rgb_matrix, KP_RGB_PERSIST_SUBTREE, NULL,
                               kp_rgb_load_cb, NULL, NULL);

#endif

int kp_rgb_save_state(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
  /* Deliberate exception to kp_rgb_work_q(): this only does flash I/O and never
   * invokes a behavior, so it keeps the larger system work queue stack. */
  int ret = k_work_reschedule(&kp_rgb_save_work,
                              K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
  return MIN(ret, 0);
#else
  return 0;
#endif
}

static void kp_rgb_restore_defaults(void) {
  kp_rgb_matrix_lock();
  (void)kp_rgb_apply_defaults();
  kp_rgb_request_runtime_reset_locked();
  kp_rgb_matrix_unlock();
}

#if IS_ENABLED(CONFIG_SETTINGS)

static int kp_rgb_reset_delete(const char *path) {
  int err = settings_delete(path);
  if (err < 0 && err != -ENOENT) {
    LOG_WRN("Failed to clear RGB state (err %d)", err);
  }
  return err;
}

static void kp_rgb_reset_work_handler(struct k_work *work) {
  ARG_UNUSED(work);
  struct k_work_sync sync;
  k_work_cancel_delayable_sync(&kp_rgb_save_work, &sync);
  kp_rgb_reset_delete(KP_RGB_PERSIST_KEY);
  for (size_t i = 0; i < kp_rgb_effect_count(); i++) {
    const struct device *fx = kp_rgb_effect_at(i);
    const char *id = fx == NULL ? NULL : kp_rgb_effect_cfg(fx)->persist_id;
    char path[sizeof(KP_RGB_PERSIST_EFFECT_PATH) + KP_RGB_PERSIST_MAX_ID_LENGTH];
    if (kp_rgb_effect_path(path, sizeof(path), id) == 0) {
      kp_rgb_reset_delete(path);
    }
  }
  kp_rgb_restore_defaults();
}

K_WORK_DEFINE(kp_rgb_reset_work, kp_rgb_reset_work_handler);

#endif

void kp_rgb_reset_state(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
  k_work_submit_to_queue(kp_rgb_work_q(), &kp_rgb_reset_work);
#else
  kp_rgb_restore_defaults();
#endif
}
