/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <zmk/rgb_matrix.h>
#include <zmk/rgb_persist.h>
#include <zmk/workqueue.h>

#include "rgb_matrix_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_SETTINGS)

#define KP_RGB_PERSIST_SUBTREE "keypaw/rgb_matrix"
#define KP_RGB_PERSIST_KEY KP_RGB_PERSIST_SUBTREE "/state"

static void kp_rgb_encode(struct kp_rgb_persist_blob *blob) {
  struct kp_rgb_persist_effect effects[KP_RGB_PERSIST_MAX_EFFECTS] = {0};
  size_t count;
  uint16_t selected;
  bool user_on;

  kp_rgb_matrix_lock();
  user_on = kp_rgb_controller.state.user_on;
  selected = (uint16_t)kp_rgb_selected_effect();
  count = MIN(kp_rgb_effect_count(), (size_t)KP_RGB_PERSIST_MAX_EFFECTS);
  for (size_t i = 0; i < count; i++) {
    const struct device *dev = kp_rgb_effect_at(i);
    if (dev == NULL) {
      continue;
    }
    const struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(dev);
    effects[i] = (struct kp_rgb_persist_effect){
        .duration_ms = data->duration_ms,
        .h = data->color.h,
        .s = data->color.s,
        .b = data->color.b,
    };
  }
  kp_rgb_matrix_unlock();

  kp_rgb_persist_pack(selected, user_on, effects, count, blob);
}

static void kp_rgb_save_work_handler(struct k_work *work) {
  ARG_UNUSED(work);
  struct kp_rgb_persist_blob blob;
  kp_rgb_encode(&blob);
  int err = settings_save_one(KP_RGB_PERSIST_KEY, &blob, sizeof(blob));
  if (err < 0) {
    LOG_WRN("Failed to persist RGB state (err %d)", err);
  }
}

K_WORK_DELAYABLE_DEFINE(kp_rgb_save_work, kp_rgb_save_work_handler);

static int kp_rgb_load_cb(const char *name, size_t len,
                          settings_read_cb read_cb, void *cb_arg) {
  if (strcmp(name, "state") != 0) {
    return -ENOENT;
  }
  if (!kp_rgb_persist_size_ok(len)) {
    LOG_INF("Discarding RGB state of unexpected size %u", (uint32_t)len);
    return -EINVAL;
  }

  struct kp_rgb_persist_blob blob;
  int rc = read_cb(cb_arg, &blob, sizeof(blob));
  if (rc < 0) {
    return rc;
  }
  if (rc != sizeof(blob) || !kp_rgb_persist_version_ok(blob.version)) {
    return -EINVAL;
  }

  size_t count =
      MIN((size_t)blob.effect_count, (size_t)KP_RGB_PERSIST_MAX_EFFECTS);
  count = MIN(count, kp_rgb_effect_count());
  kp_rgb_matrix_lock();
  for (size_t i = 0; i < count; i++) {
    const struct device *dev = kp_rgb_effect_at(i);
    if (dev == NULL) {
      continue;
    }
    const struct kp_rgb_persist_effect *stored = &blob.effects[i];
    if (!kp_rgb_persist_effect_valid(stored)) {
      LOG_WRN("Skipping out-of-range persisted effect %u", (uint32_t)i);
      continue;
    }
    struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(dev);
    data->color = (struct kp_rgb_hsb){
        .h = stored->h == KP_RGB_HUE_MAX ? 0 : stored->h,
        .s = stored->s,
        .b = stored->b,
    };
    data->duration_ms = kp_rgb_persist_clamp_duration(
        stored->duration_ms, CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS,
        CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
  }

  size_t previous_index = kp_rgb_controller.effect_index;
  const struct device *previous_fx = kp_rgb_controller.state.active_fx;
  bool unavailable = kp_rgb_select_effect(blob.selected_index) < 0;
  if (unavailable) {
    LOG_WRN("Persisted effect %u is unavailable",
            (uint32_t)blob.selected_index);
    kp_rgb_resolve_active();
  }
  kp_rgb_controller.state.user_on = blob.user_on;
  kp_rgb_reconcile_power_locked();
  if (unavailable || (previous_index == kp_rgb_controller.effect_index &&
                      previous_fx == kp_rgb_controller.state.active_fx)) {
    zmk_rgb_matrix_flush();
  }
  kp_rgb_matrix_unlock();

  return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(kp_rgb_matrix, KP_RGB_PERSIST_SUBTREE, NULL,
                               kp_rgb_load_cb, NULL, NULL);

#endif

int kp_rgb_save_state(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
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

static void kp_rgb_reset_work_handler(struct k_work *work) {
  ARG_UNUSED(work);
  struct k_work_sync sync;
  /* The save handler takes the matrix lock; cancel before acquiring it. */
  k_work_cancel_delayable_sync(&kp_rgb_save_work, &sync);
  int err = settings_delete(KP_RGB_PERSIST_KEY);
  if (err < 0 && err != -ENOENT) {
    LOG_WRN("Failed to clear RGB state (err %d)", err);
  }
  kp_rgb_restore_defaults();
}

K_WORK_DEFINE(kp_rgb_reset_work, kp_rgb_reset_work_handler);

#endif

void kp_rgb_reset_state(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
  k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &kp_rgb_reset_work);
#else
  kp_rgb_restore_defaults();
#endif
}
