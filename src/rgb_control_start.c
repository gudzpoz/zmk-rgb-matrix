/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include "rgb_matrix_internal.h"
#include <limits.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int control_start(void) {
  int err = kp_rgb_settings_commit();
  if (err == 0)
    err = kp_rgb_selector_overlays_start();
  if (err < 0) {
    LOG_ERR("RGB control startup failed (err %d)", err);
    return err;
  }
  kp_rgb_conditions_start();
  return 0;
}

#if IS_ENABLED(CONFIG_SETTINGS)

SETTINGS_STATIC_HANDLER_DEFINE_WITH_CPRIO(kp_rgb_control, "keypaw/rgb_control",
                                          NULL, NULL, control_start, NULL,
                                          INT_MAX);

#else

BUILD_ASSERT(CONFIG_APPLICATION_INIT_PRIORITY < 99,
             "RGB automatic control startup must follow matrix initialization");

SYS_INIT(control_start, APPLICATION, 99);

#endif
