/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#define DT_DRV_COMPAT test_rgb_static_overlay
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zmk/rgb_matrix.h>
#include "rgb_matrix_internal.h"

#define CHECK(expr) do {                                                     \
    if (!(expr)) {                                                           \
        printk("FAIL static_metadata line %u: %s\n", __LINE__, #expr);        \
        exit(2);                                                             \
    }                                                                        \
} while (0)

struct static_config {
    struct kp_rgb_overlay_common_config common;
};
struct static_data { struct kp_rgb_overlay_common_data common; };
static bool render(const struct device *dev, const struct kp_rgb_frame *frame);
static const struct kp_rgb_effect_callbacks callbacks __unused = {.render = render};
#define DEFINE(inst)                                                         \
    KP_RGB_OVERLAY_TARGET_ARRAYS(inst, sm_##inst);                             \
    static const struct static_config sm_##inst##_cfg = {                     \
        .common = KP_RGB_OVERLAY_COMMON(DT_DRV_INST(inst), &callbacks, false), \
    };                                                                       \
    static struct static_data sm_##inst##_data = {                            \
        KP_RGB_OVERLAY_COMMON_DATA(inst, sm_##inst),                           \
    };                                                                       \
    KP_RGB_OVERLAY_DEFINE(inst, sm_##inst);
DT_INST_FOREACH_STATUS_OKAY(DEFINE)

#if DT_NODE_EXISTS(DT_NODELABEL(sm00))
#define D(label) DEVICE_DT_GET(DT_NODELABEL(label))
#if DT_PROP(DT_NODELABEL(sm00), zero_local_leds)
#define EXPECTED_LEDS 0
#else
#define EXPECTED_LEDS 12
#endif
BUILD_ASSERT(KP_LED_COUNT == EXPECTED_LEDS);
struct oracle {
    const struct device *dev;
    size_t count;
    size_t targets[12];
};
/* Physical indices and lengths are literal, independent of production macros. */
static const struct oracle expected[] = {
    {D(sm00), 10, {11, 3, 1, 4, 5, 6, 7, 8, 9, 10}},
    {D(sm01), 4, {3, 1, 2, 0}},
    {D(sm02), 0, {0}},
    {D(sm03), 0, {0}},
    {D(sm04), 0, {0}},
    {D(sm05), 0, {0}},
    {D(sm06), 0, {0}},
    {D(sm07), 1, {2}},
    {D(sm08), 3, {4, 1, 5}},
    {D(sm09), 12, {11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0}},
    {D(sm10), 1, {3}},
    {D(sm11), 1, {0}},
    {D(sm12), 1, {11}},
    {D(sm13), 1, {5}},
    {D(sm14), 1, {4}},
    {D(sm15), 1, {10}},
    {D(sm16), 1, {7}},
};
BUILD_ASSERT(KP_RGB_OVERLAY_COUNT == 17);
BUILD_ASSERT(KP_RGB_OVERLAY_WORDS == 2);
BUILD_ASSERT(KP_RGB_OVERLAY_INDEX(DT_NODELABEL(sm00)) == 0);
BUILD_ASSERT(KP_RGB_OVERLAY_INDEX(DT_NODELABEL(sm01)) == 1);
BUILD_ASSERT(KP_RGB_OVERLAY_INDEX(DT_NODELABEL(sm02)) == 2);
BUILD_ASSERT(KP_RGB_OVERLAY_INDEX(DT_NODELABEL(sm03)) == 3);
BUILD_ASSERT(KP_RGB_OVERLAY_INDEX(DT_NODELABEL(sm04)) == 4);
BUILD_ASSERT(KP_RGB_OVERLAY_INDEX(DT_NODELABEL(sm05)) == 5);
BUILD_ASSERT(KP_RGB_OVERLAY_INDEX(DT_NODELABEL(sm06)) == 6);
BUILD_ASSERT(KP_RGB_OVERLAY_INDEX(DT_NODELABEL(sm07)) == 7);
BUILD_ASSERT(KP_RGB_OVERLAY_INDEX(DT_NODELABEL(sm08)) == 8);
BUILD_ASSERT(KP_RGB_OVERLAY_INDEX(DT_NODELABEL(sm09)) == 9);
BUILD_ASSERT(KP_RGB_OVERLAY_INDEX(DT_NODELABEL(sm10)) == 10);
BUILD_ASSERT(KP_RGB_OVERLAY_INDEX(DT_NODELABEL(sm11)) == 11);
BUILD_ASSERT(KP_RGB_OVERLAY_INDEX(DT_NODELABEL(sm12)) == 12);
BUILD_ASSERT(KP_RGB_OVERLAY_INDEX(DT_NODELABEL(sm13)) == 13);
BUILD_ASSERT(KP_RGB_OVERLAY_INDEX(DT_NODELABEL(sm14)) == 14);
BUILD_ASSERT(KP_RGB_OVERLAY_INDEX(DT_NODELABEL(sm15)) == 15);
BUILD_ASSERT(KP_RGB_OVERLAY_INDEX(DT_NODELABEL(sm16)) == 16);
#else
#define EXPECTED_LEDS 12
BUILD_ASSERT(KP_LED_COUNT == 12);
static const struct { const struct device *dev; } expected[] = {};
BUILD_ASSERT(KP_RGB_OVERLAY_COUNT == 0);
BUILD_ASSERT(KP_RGB_OVERLAY_WORDS == 1);
#endif

#if DT_NODE_EXISTS(DT_NODELABEL(static_cond_b))
extern struct kp_rgb_condition_registration
    KP_RGB_CONDITION_REGISTRATION(DT_NODELABEL(static_cond_b));
#endif
#define CONDITION_ORDINAL(node_id) {DEVICE_DT_GET(node_id), DT_DEP_ORD(node_id)},
static const struct { const struct device *dev; unsigned ordinal; } condition_ordinals[] = {
    DT_FOREACH_STATUS_OKAY(test_rgb_static_condition_a, CONDITION_ORDINAL)
    DT_FOREACH_STATUS_OKAY(test_rgb_static_condition_b, CONDITION_ORDINAL)
};
#if DT_NODE_EXISTS(DT_NODELABEL(ordinal_boundary))
BUILD_ASSERT(ARRAY_SIZE(condition_ordinals) == 130);
#elif DT_NODE_EXISTS(DT_NODELABEL(static_cond_a))
BUILD_ASSERT(ARRAY_SIZE(condition_ordinals) == 2);
#else
BUILD_ASSERT(ARRAY_SIZE(condition_ordinals) == 0);
#endif

static bool pre_kernel_ok;
static size_t early_condition_count;
static int inspect_before_init(void) {
    bool ok = true;
#if DT_NODE_EXISTS(DT_NODELABEL(sm00))
    for (size_t i = 0; i < ARRAY_SIZE(expected); i++) {
        const struct static_data *data = expected[i].dev->data;
        ok &= data->common.index == i && !data->common.local;
        ok &= data->common.leds != NULL && data->common.led_count == 0;
    }
#endif
    unsigned previous_ordinal = 0;
    bool unpadded_order_wrong = false;
    char previous_text[16] = "";
    STRUCT_SECTION_FOREACH(kp_rgb_condition_registration, record) {
#if DT_NODE_EXISTS(DT_NODELABEL(static_cond_a))
        unsigned ordinal = 0;
        for (size_t i = 0; i < ARRAY_SIZE(condition_ordinals); i++)
            if (condition_ordinals[i].dev == record->dev) ordinal = condition_ordinals[i].ordinal;
        ok &= ordinal > previous_ordinal;
        char text[16];
        snprintf(text, sizeof(text), "%u", ordinal);
        unpadded_order_wrong |= previous_ordinal && strcmp(previous_text, text) > 0;
        strcpy(previous_text, text);
        previous_ordinal = ordinal;
        if (record->dev == DEVICE_DT_GET(DT_NODELABEL(static_cond_a))) {
            ok &= record->dep_count == 1 && record->deps != NULL;
            if (record->deps) ok &= record->deps[0] ==
                &KP_RGB_CONDITION_REGISTRATION(DT_NODELABEL(static_cond_b));
            if (record->deps) ok &= record->deps[0]->dev == DEVICE_DT_GET(DT_NODELABEL(static_cond_b));
        } else {
            ok &= record->dep_count == 0 && record->deps == NULL;
        }
#else
        ok = false;
#endif
        ok &= !record->live && !record->fault && !record->value;
        early_condition_count++;
    }
#if DT_NODE_EXISTS(DT_NODELABEL(ordinal_boundary))
    ok &= unpadded_order_wrong;
#else
    ARG_UNUSED(unpadded_order_wrong);
    ARG_UNUSED(previous_ordinal);
    ARG_UNUSED(previous_text);
#endif
    pre_kernel_ok = ok;
    return 0;
}
SYS_INIT(inspect_before_init, PRE_KERNEL_1, 0);

static bool early_invalidation_ok;
static int invalidate_before_provider_init(void) {
    bool ok = true;
    size_t count = 0;
    STRUCT_SECTION_FOREACH(kp_rgb_condition_registration, record) {
        ok &= !device_is_ready(record->dev);
        kp_rgb_condition_invalidate();
        ok &= !record->live && !record->fault && !record->value;
        count++;
    }
    early_invalidation_ok = ok && count == early_condition_count;
    return 0;
}
SYS_INIT(invalidate_before_provider_init, POST_KERNEL, 0);

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT test_rgb_static_condition_a
#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
static atomic_t samples_a;
static bool sample_a(const struct device *dev) {
    ARG_UNUSED(dev);
    atomic_inc(&samples_a);
    CHECK(kp_rgb_condition_value(DEVICE_DT_GET(DT_NODELABEL(static_cond_b))));
    return true;
}
static const struct kp_rgb_condition_api condition_api = {
    .scope = KP_RGB_CONDITION_ANY_SIDE, .sample = sample_a,
};
KP_RGB_CONDITION_DEFINE(0, &condition_api, NULL, NULL);
extern unsigned kp_rgb_static_samples_b(void);
#endif

static atomic_t all_rendered __unused;
static bool render(const struct device *dev, const struct kp_rgb_frame *frame) {
#if DT_NODE_EXISTS(DT_NODELABEL(sm00))
    const struct static_data *data = dev->data;
    size_t index = data->common.index;
    CHECK(index < 17 && dev == expected[index].dev);
    CHECK(frame->count == 12 && frame->targets != NULL);
    if (index == 6) {
        CHECK(frame->target_count == 12);
        static const size_t all[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
        for (size_t i = 0; i < 12; i++) CHECK(frame->targets[i] == all[i]);
        atomic_set(&all_rendered, 1);
    } else {
        CHECK(frame->target_count == expected[index].count);
        for (size_t i = 0; i < frame->target_count; i++)
            CHECK(frame->targets[i] == expected[index].targets[i]);
    }
#else
    ARG_UNUSED(dev); ARG_UNUSED(frame);
    CHECK(false);
#endif
    return false;
}

static void scenario(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    k_sleep(K_MSEC(100));
    CHECK(pre_kernel_ok && early_invalidation_ok);
    CHECK(device_is_ready(DEVICE_DT_GET(DT_NODELABEL(rgb_matrix))));
    CHECK(KP_RGB_OVERLAY_COUNT == ARRAY_SIZE(expected));
#if DT_NODE_EXISTS(DT_NODELABEL(sm00))
    CHECK(early_condition_count == ARRAY_SIZE(condition_ordinals));
    CHECK(atomic_get(&samples_a) > 0 && kp_rgb_static_samples_b() > 0);
    CHECK(KP_RGB_OVERLAY_WORDS == 2);
    CHECK(kp_rgb_overlay_get_word(0) == 0xffff);
    CHECK(kp_rgb_overlay_get_word(1) == 1);
    CHECK(kp_rgb_overlay_get_word(2) == 0);
    const struct device *const *list = kp_rgb_overlay_list();
    for (size_t i = 0; i < 17; i++) {
        CHECK(list[i] == expected[i].dev && device_is_ready(list[i]));
        size_t count = EXPECTED_LEDS == 0 ? 0 : (i == 6 ? 12 : expected[i].count);
        CHECK(kp_rgb_overlay_target_count(list[i]) == count);
        const size_t *targets = kp_rgb_overlay_targets(list[i]);
        CHECK(targets != NULL);
        static const size_t all[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
        for (size_t j = 0; j < count; j++)
            CHECK(targets[j] == (i == 6 ? all[j] : expected[i].targets[j]));
    }
    if (EXPECTED_LEDS > 0) {
        for (unsigned i = 0; i < 100 && !atomic_get(&all_rendered); i++)
            k_sleep(K_MSEC(10));
        CHECK(atomic_get(&all_rendered));
    }
#else
    CHECK(early_condition_count == 0);
    size_t linked_count;
    STRUCT_SECTION_COUNT(kp_rgb_condition_registration, &linked_count);
    CHECK(linked_count == 0);
    CHECK(KP_RGB_OVERLAY_WORDS == 1);
    CHECK(kp_rgb_overlay_get_word(0) == 0);
#endif
    printk("PASS static_metadata overlays=%u LEDs=%u conditions=%u\n",
           (unsigned)ARRAY_SIZE(expected), KP_LED_COUNT, (unsigned)early_condition_count);
    exit(0);
}
K_THREAD_DEFINE(static_metadata_scenario, 3072, scenario, NULL, NULL, NULL, 5, 0, 0);
