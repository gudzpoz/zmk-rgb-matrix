/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#define DT_DRV_COMPAT test_rgb_static_condition_b
#include <zephyr/kernel.h>
#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
static atomic_t samples;
static bool sample(const struct device *dev) {
    ARG_UNUSED(dev);
    atomic_inc(&samples);
    return true;
}
unsigned kp_rgb_static_samples_b(void) { return atomic_get(&samples); }
static const struct kp_rgb_condition_api api = {
    .scope = KP_RGB_CONDITION_ANY_SIDE, .sample = sample,
};
#define DEFINE(inst) KP_RGB_CONDITION_DEFINE(inst, &api, NULL, NULL);
DT_INST_FOREACH_STATUS_OKAY(DEFINE)
#endif
