/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include "rgb_matrix_internal.h"
#include <zephyr/init.h>

static bool kp_rgb_started;

bool kp_rgb_control_started_locked(void) { return kp_rgb_started; }

int kp_rgb_control_commit(void) {
  kp_rgb_matrix_lock();
  kp_rgb_settings_restore_locked();
  if (!kp_rgb_started) {
    for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++) {
      struct kp_rgb_selector *selector = kp_rgb_selector_registry_at(i);
      kp_rgb_selector_resolve_locked(selector, selector->effect_index);
    }
    kp_rgb_started = true;
    kp_rgb_conditions_start();
  }
  kp_rgb_matrix_unlock();
  return 0;
}

#if !IS_ENABLED(CONFIG_SETTINGS)
BUILD_ASSERT(CONFIG_APPLICATION_INIT_PRIORITY < 99,
             "RGB automatic control startup must follow matrix initialization");

SYS_INIT(kp_rgb_control_commit, APPLICATION, 99);
#endif
