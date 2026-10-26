/* SPDX-License-Identifier: MIT */
#pragma once

#include <errno.h>
#include <zephyr/kernel.h>
#include "../../../../src/rgb_matrix_internal.h"
#include "../../../../src/rgb_selector_internal.h"

/* TEST ONLY: fixture state setup through private services, without saving or sending. */
static inline int kp_test_select(const struct device *owner, const struct device *effect) {
    kp_rgb_assert_thread_context();
    kp_rgb_matrix_lock();
    struct kp_rgb_selector *selector = kp_rgb_selector_overlay_state_locked(owner);
    int result = selector == NULL ? -ENODEV : -EINVAL;
    if (selector != NULL && effect != NULL) {
        for (size_t slot = 0; slot < selector->effect_count; slot++) {
            if (kp_rgb_selector_at(selector, slot) == effect) {
                result = kp_rgb_selector_overlay_select_locked(owner, (uint16_t)slot);
                if (result >= 0) result = 0;
                break;
            }
        }
    }
    kp_rgb_matrix_unlock();
    return result;
}

static inline int kp_test_reset(const struct device *owner) {
    kp_rgb_assert_thread_context();
    kp_rgb_matrix_lock();
    int result = kp_rgb_selector_overlay_reset_locked(owner);
    kp_rgb_matrix_unlock();
    return result;
}

static inline int kp_test_set_color(const struct device *effect, const struct kp_rgb_hsb *color) {
    kp_rgb_assert_thread_context();
    kp_rgb_matrix_lock();
    int result = kp_rgb_set_color_locked(effect, color);
    kp_rgb_matrix_unlock();
    return result;
}

static inline int kp_test_set_period(const struct device *effect, uint32_t period_ms) {
    kp_rgb_assert_thread_context();
    kp_rgb_matrix_lock();
    int result = kp_rgb_set_period_locked(effect, period_ms);
    kp_rgb_matrix_unlock();
    return result;
}
