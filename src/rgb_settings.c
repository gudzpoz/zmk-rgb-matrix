/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <limits.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>
#include <zmk/rgb_matrix.h>

#include "rgb_matrix_internal.h"
#include "rgb_persist_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_SETTINGS)

#define KP_RGB_SETTINGS_SIZE KP_RGB_PERSIST_SIZE(KP_RGB_SELECTOR_COUNT)

BUILD_ASSERT(KP_RGB_SELECTOR_COUNT <= UINT16_MAX,
             "RGB selector count exceeds settings count field");
BUILD_ASSERT(KP_RGB_SETTINGS_SIZE <= SETTINGS_MAX_VAL_LEN,
             "RGB snapshot exceeds SETTINGS_MAX_VAL_LEN");

static struct kp_rgb_persist_state kp_load;
static uint16_t kp_load_selections[MAX(1, KP_RGB_SELECTOR_COUNT)];
static bool kp_load_pending;
static bool kp_save_pending;
static int64_t kp_save_deadline;

static void kp_rgb_save_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(kp_rgb_save_work, kp_rgb_save_work_handler);

static uint16_t kp_initial_slot(const struct kp_rgb_selector *selector) {
  return selector->initial_effect < selector->effect_count
             ? selector->initial_effect
             : 0;
}

static int kp_rgb_load_cb(const char *name, size_t len,
                          settings_read_cb read_cb, void *cb_arg) {
  if (strcmp(name, "snapshot") != 0)
    return 0;
  if (len < KP_RGB_PERSIST_HEADER_SIZE ||
      (len - KP_RGB_PERSIST_HEADER_SIZE) % 2 != 0 ||
      (len - KP_RGB_PERSIST_HEADER_SIZE) / 2 > UINT16_MAX)
    return -EINVAL;

  uint8_t bytes[KP_RGB_SETTINGS_SIZE];
  uint16_t selections[MAX(1, KP_RGB_SELECTOR_COUNT)];
  struct kp_rgb_persist_state value;
  size_t prefix_len = MIN(len, sizeof(bytes));
  int rc = read_cb(cb_arg, bytes, prefix_len);
  if (rc < 0)
    return rc;
  if (rc != (int)prefix_len)
    return -EINVAL;
  rc = kp_rgb_persist_decode(bytes, prefix_len, len, &value, selections,
                             KP_RGB_SELECTOR_COUNT);
  if (rc < 0)
    return rc;

  kp_rgb_matrix_lock();
  kp_load = value;
  for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++)
    kp_load_selections[i] = i < value.selection_count
                                ? selections[i]
                                : kp_initial_slot(kp_rgb_selector_registry_at(i));
  kp_load_pending = true;
  kp_rgb_matrix_unlock();
  return 0;
}

void kp_rgb_settings_restore_locked(void) {
  if (kp_load_pending) {
    kp_rgb_global.user_on = kp_load.user_on;
    kp_rgb_global.brightness = kp_load.brightness;
    kp_rgb_reconcile_power_locked();
    for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++) {
      struct kp_rgb_selector *selector = kp_rgb_selector_registry_at(i);
      uint16_t slot = kp_load_selections[i];
      if (slot >= selector->effect_count)
        slot = kp_initial_slot(selector);
      kp_rgb_selector_select_locked(selector, slot);
    }
    kp_load_pending = false;
  }
  zmk_rgb_matrix_flush();
}

SETTINGS_STATIC_HANDLER_DEFINE_WITH_CPRIO(kp_rgb_matrix, KP_RGB_PERSIST_SUBTREE,
                                          NULL, kp_rgb_load_cb,
                                          kp_rgb_control_commit, NULL,
                                          INT_MAX);

static int kp_request_save_locked(int64_t delay_ms) {
  kp_save_pending = true;
  kp_save_deadline = k_uptime_get() + delay_ms;
  return MIN(k_work_reschedule(&kp_rgb_save_work, K_MSEC(delay_ms)), 0);
}

static void kp_rgb_save_work_handler(struct k_work *work) {
  ARG_UNUSED(work);
  uint8_t bytes[KP_RGB_SETTINGS_SIZE];
  uint16_t selections[MAX(1, KP_RGB_SELECTOR_COUNT)];

  kp_rgb_matrix_lock();
  if (!kp_save_pending) {
    kp_rgb_matrix_unlock();
    return;
  }
  int64_t remaining = kp_save_deadline - k_uptime_get();
  if (remaining > 0) {
    k_work_reschedule(&kp_rgb_save_work, K_MSEC(remaining));
    kp_rgb_matrix_unlock();
    return;
  }
  kp_save_pending = false;
  struct kp_rgb_persist_state state = {
      .user_on = kp_rgb_global.user_on,
      .brightness = kp_rgb_global.brightness,
      .selection_count = KP_RGB_SELECTOR_COUNT,
  };
  for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++)
    selections[i] = kp_rgb_selector_registry_at(i)->effect_index;
  int err = kp_rgb_persist_encode(&state, selections, bytes, sizeof(bytes));
  kp_rgb_matrix_unlock();

  /* Flash I/O uses the larger system work queue stack. */
  if (err == 0)
    err = settings_save_one(KP_RGB_PERSIST_KEY, bytes, sizeof(bytes));
  if (err < 0)
    LOG_WRN("RGB settings save failed (err %d)", err);
}

#else

void kp_rgb_settings_restore_locked(void) {}

#endif

int kp_rgb_save_state(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
  kp_rgb_matrix_lock();
  int err = kp_request_save_locked(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE);
  kp_rgb_matrix_unlock();
  return err;
#else
  return 0;
#endif
}

int kp_rgb_save_selector_state(const struct device *selector, bool reset) {
#if IS_ENABLED(CONFIG_SETTINGS)
  int err = -ENODEV;
  kp_rgb_matrix_lock();
  for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++) {
    const struct kp_rgb_selector *state = kp_rgb_selector_registry_at(i);
    if (state->owner != selector)
      continue;
    if (reset && kp_load_pending)
      kp_load_selections[i] = state->effect_index;
    err = kp_request_save_locked(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE);
    break;
  }
  kp_rgb_matrix_unlock();
  return err;
#else
  ARG_UNUSED(selector);
  ARG_UNUSED(reset);
  return 0;
#endif
}

void kp_rgb_reset_state(void) {
  kp_rgb_matrix_lock();
  kp_rgb_apply_defaults();
  kp_rgb_selector_overlays_defaults_locked();
  kp_rgb_request_runtime_reset_locked();
#if IS_ENABLED(CONFIG_SETTINGS)
  kp_load_pending = false;
  kp_request_save_locked(0);
#endif
  kp_rgb_matrix_unlock();
}
