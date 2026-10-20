/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#define DT_DRV_COMPAT test_rgb_static_overlay
#include <stdlib.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zmk/rgb_matrix.h>
#include "rgb_matrix_internal.h"

#define CHECK(expr) do {                                                     \
    if (!(expr)) {                                                           \
        printk("FAIL static_metadata large line %u: %s\n", __LINE__, #expr);  \
        exit(2);                                                             \
    }                                                                        \
} while (0)

static const size_t expected[120] = {
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
    60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71,
    72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83,
    84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95,
    47, 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36,
    35, 34, 33, 32, 31, 30, 29, 28, 27, 26, 25, 24,
    23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12,
    11, 10,  9,  8,  7,  6,  5,  4,  3,  2,  1,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
};

KP_RGB_OVERLAY_TARGET_ARRAYS(0, large);
BUILD_ASSERT(KP_LED_COUNT == 96);
BUILD_ASSERT(KP_RGB_OVERLAY_COUNT == 1);
BUILD_ASSERT(KP_RGB_OVERLAY_WORDS == 1);
BUILD_ASSERT(KP_RGB_OVERLAY_INDEX(DT_NODELABEL(sm_large)) == 0);
BUILD_ASSERT(large_target_count == 96);
BUILD_ASSERT(large_target_candidates == 120);
BUILD_ASSERT(ARRAY_SIZE(large_targets) == 120);
BUILD_ASSERT(ARRAY_SIZE(large_keys) == 72);
BUILD_ASSERT(ARRAY_SIZE(large_leds) == 48);

K_SEM_DEFINE(render_seen, 0, 1);
static bool render(const struct device *dev, const struct kp_rgb_frame *frame) {
    CHECK(dev == DEVICE_DT_GET(DT_NODELABEL(sm_large)));
    CHECK(frame->count == 96 && frame->target_count == 96);
    CHECK(frame->targets != NULL);
    for (size_t i = 0; i < 96; i++) CHECK(frame->targets[i] == expected[i]);
    k_sem_give(&render_seen);
    return false;
}
static const struct kp_rgb_effect_callbacks callbacks = {.render = render};
static const struct { struct kp_rgb_overlay_common_config common; } large_cfg = {
    .common = KP_RGB_OVERLAY_COMMON(DT_DRV_INST(0), large),
};
static struct { struct kp_rgb_overlay_common_data common; } large_data = {
    KP_RGB_OVERLAY_COMMON_DATA(0, large),
};
KP_RGB_OVERLAY_DEFINE(0, &callbacks, false, large);

static bool early_ok;
static int inspect_before_init(void) {
    const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(sm_large));
    const struct kp_rgb_overlay_common_data *data = dev->data;
    const struct kp_rgb_overlay_common_config *cfg = dev->config;
    bool ok = data->index == 0 && !data->local && data->led_count == 96;
    ok &= data->leds == large_targets && cfg->keys_len == 72 && cfg->leds_len == 48;
    for (size_t i = 0; i < 120; i++) ok &= data->leds[i] == expected[i];
    size_t conditions = 0;
    STRUCT_SECTION_FOREACH(kp_rgb_condition_registration, record) {
        (void)record;
        conditions++;
    }
    early_ok = ok && conditions == 0;
    return 0;
}
SYS_INIT(inspect_before_init, PRE_KERNEL_1, 0);

static void scenario(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    CHECK(k_sem_take(&render_seen, K_SECONDS(2)) == 0);
    CHECK(early_ok);
    CHECK(device_is_ready(DEVICE_DT_GET(DT_NODELABEL(rgb_matrix))));
    const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(sm_large));
    CHECK(device_is_ready(dev));
    CHECK(kp_rgb_overlay_count() == 1 && kp_rgb_overlay_list()[0] == dev);
    CHECK(kp_rgb_overlay_word_count() == 1 && kp_rgb_overlay_get_word(0) == 1);
    CHECK(kp_rgb_overlay_target_count(dev) == 96);
    const size_t *targets = kp_rgb_overlay_targets(dev);
    CHECK(targets != NULL);
    for (size_t i = 0; i < 96; i++) CHECK(targets[i] == expected[i]);
    size_t conditions;
    STRUCT_SECTION_COUNT(kp_rgb_condition_registration, &conditions);
    CHECK(conditions == 0);
    printk("PASS static_metadata large overlays=1 LEDs=96 targets=96 backing=120 conditions=0\n");
    exit(0);
}
K_THREAD_DEFINE(static_metadata_large_scenario, 3072, scenario, NULL, NULL, NULL, 5, 0, 0);
