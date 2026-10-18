/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#define DT_DRV_COMPAT keypaw_rgb_matrix_selector_probe

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zmk/physical_layouts.h>
#include <zmk/rgb_matrix.h>

#define CHECK(expr) do {                                                     \
    if (!(expr)) {                                                           \
        printk("FAIL geometry case %d line %u: %s\n",                         \
               KP_GEOMETRY_CASE, __LINE__, #expr);                            \
        exit(2);                                                             \
    }                                                                        \
} while (0)

/* Literal oracles: no DT attribute or center arithmetic is used here. */
#if KP_GEOMETRY_CASE <= 3 || KP_GEOMETRY_CASE == 9
static const struct kp_rgb_coord expected_coords[] = {
    {160, 240}, {60, 60}, {0, 0}, {160, 240}, {250, 110},
};
static const size_t expected_reverse[] = {1, 4, SIZE_MAX, 3, SIZE_MAX, SIZE_MAX};
#define EXPECTED_LENGTH 250
#define EXPECTED_HEIGHT 240
#elif KP_GEOMETRY_CASE == 4
static const struct kp_rgb_coord expected_coords[] = {
    {65484, 65484}, {50, 52}, {16384, 16382}, {32767, 32767},
    {49149, 16384}, {65534, 0}, {0, 65534}, {6, 9},
};
static const size_t expected_reverse[] = {0, 1, 2, 3, 4, 5, 6};
#define EXPECTED_LENGTH 65534
#define EXPECTED_HEIGHT 65534
#elif KP_GEOMETRY_CASE == 5
static const struct kp_rgb_coord expected_coords[] = {{0, 0}, {0, 0}, {0, 0}};
static const size_t expected_reverse[] = {0, 1, 2};
#define EXPECTED_LENGTH 100
#define EXPECTED_HEIGHT 100
#elif KP_GEOMETRY_CASE == 7
static const struct kp_rgb_coord expected_coords[] = {};
static const size_t expected_reverse[] = {
    SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX,
};
#define EXPECTED_LENGTH 100
#define EXPECTED_HEIGHT 100
#elif KP_GEOMETRY_CASE == 10
static const struct kp_rgb_coord expected_coords[] = {{0, 0}};
static const size_t expected_reverse[] = {SIZE_MAX, SIZE_MAX, SIZE_MAX, 0, SIZE_MAX, SIZE_MAX};
#define EXPECTED_LENGTH 100
#define EXPECTED_HEIGHT 100
#else
#error Unknown geometry fixture
#endif

BUILD_ASSERT(KP_LED_COUNT == ARRAY_SIZE(expected_coords));
K_SEM_DEFINE(render_seen, 0, 1);

static void check_geometry(void) {
    for (size_t i = 0; i < ARRAY_SIZE(expected_coords); i++) {
        const struct kp_rgb_coord *coord = kp_rgb_led_coord(i);
        CHECK(coord != NULL);
        CHECK(coord->x == expected_coords[i].x);
        CHECK(coord->y == expected_coords[i].y);
    }
    CHECK(kp_rgb_led_coord(ARRAY_SIZE(expected_coords)) == NULL);
    CHECK(kp_rgb_led_coord(SIZE_MAX) == NULL);
    for (size_t i = 0; i < ARRAY_SIZE(expected_reverse); i++) {
        CHECK(kp_rgb_led_for_position(i) == expected_reverse[i]);
    }
    CHECK(kp_rgb_led_for_position(ARRAY_SIZE(expected_reverse)) == SIZE_MAX);
    CHECK(kp_rgb_led_for_position(1000) == SIZE_MAX);
    CHECK(kp_rgb_led_for_position(UINT32_MAX) == SIZE_MAX);
}

static bool geometry_render(const struct device *dev, const struct kp_rgb_frame *frame) {
    ARG_UNUSED(dev);
    CHECK(frame->count == ARRAY_SIZE(expected_coords));
    CHECK(frame->count > 0);
    CHECK(frame->target_count == ARRAY_SIZE(expected_coords));
    CHECK(frame->targets != NULL);
    CHECK(frame->board_length == EXPECTED_LENGTH);
    CHECK(frame->board_height == EXPECTED_HEIGHT);
    check_geometry();
    for (size_t i = 0; i < frame->count; i++) {
        CHECK(frame->targets[i] == i);
        CHECK(frame->coords[i].x == expected_coords[i].x);
        CHECK(frame->coords[i].y == expected_coords[i].y);
        frame->pixels[i] = (struct led_rgb){.r = 1, .g = 2, .b = 3};
    }
    k_sem_give(&render_seen);
    return false;
}

static const struct kp_rgb_effect_callbacks geometry_callbacks = {.render = geometry_render};
static const struct { struct kp_rgb_effect_common_config common; } geometry_effect_cfg = {
    KP_RGB_EFFECT_COMMON_CONFIG(0),
};
static struct { struct kp_rgb_effect_common_data common; } geometry_effect_data;
KP_RGB_EFFECT_DEFINE(DT_NODELABEL(geometry_probe), &geometry_callbacks, geometry_effect);

static int strip_update(const struct device *dev, struct led_rgb *pixels, size_t count) {
    ARG_UNUSED(dev);
    ARG_UNUSED(pixels);
    CHECK(count == KP_LED_COUNT);
    return 0;
}

static size_t strip_length(const struct device *dev) {
    ARG_UNUSED(dev);
    return DT_PROP(DT_NODELABEL(geometry_strip), chain_length);
}

static const struct led_strip_driver_api strip_api = {
    .update_rgb = strip_update,
    .length = strip_length,
};
DEVICE_DT_DEFINE(DT_NODELABEL(geometry_strip), NULL, NULL, NULL, NULL,
                 POST_KERNEL, 90, &strip_api);

static void check_runtime_precondition(void) {
    const struct zmk_physical_layout *const *layouts;
#if KP_GEOMETRY_CASE == 2
    CHECK(zmk_physical_layouts_get_list(&layouts) == 2);
#else
    CHECK(zmk_physical_layouts_get_list(&layouts) == 1);
#endif
    CHECK(layouts != NULL && layouts[0] != NULL);
#if KP_GEOMETRY_CASE == 2 || KP_GEOMETRY_CASE == 9
    CHECK(!strcmp(layouts[0]->display_name, "Runtime decoy"));
    CHECK(layouts[0]->keys_len == 1);
    CHECK(layouts[0]->keys[0].x == 900 && layouts[0]->keys[0].y == 800);
    CHECK(layouts[0]->keys[0].width == 100 && layouts[0]->keys[0].height == 100);
#if KP_GEOMETRY_CASE == 9
    BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_NODELABEL(geometry_layout), disabled));
#endif
#if KP_GEOMETRY_CASE == 2
    CHECK(!strcmp(layouts[1]->display_name, "Declared geometry"));
    CHECK(layouts[1]->keys_len == ARRAY_SIZE(expected_reverse));
    BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_NODELABEL(geometry_layout), okay));
#endif
    BUILD_ASSERT(DT_SAME_NODE(DT_PHANDLE(KP_RGB_NODE, physical_layout),
                              DT_NODELABEL(geometry_layout)));
#elif KP_GEOMETRY_CASE == 3
    BUILD_ASSERT(DT_HAS_CHOSEN(zmk_matrix_transform));
    CHECK(!strcmp(layouts[0]->display_name, "Default"));
    CHECK(layouts[0]->keys_len == 0 && layouts[0]->keys == NULL);
#else
    CHECK(!strcmp(layouts[0]->display_name, "Declared geometry"));
    CHECK(layouts[0]->keys_len == ARRAY_SIZE(expected_reverse));
#if KP_GEOMETRY_CASE == 4
    static const struct { int16_t width, height, x, y; } attrs[] = {
        {101, 103, -101, -103}, {-101, -103, 101, 103},
        {-32767, 32767, -32768, -1}, {1, -1, -32768, 32767},
        {32767, -32768, 32767, -32768}, {-3, 3, 0, -1}, {3, -3, 0, -1},
    };
    for (size_t i = 0; i < ARRAY_SIZE(attrs); i++) {
        CHECK(layouts[0]->keys[i].width == attrs[i].width);
        CHECK(layouts[0]->keys[i].height == attrs[i].height);
        CHECK(layouts[0]->keys[i].x == attrs[i].x);
        CHECK(layouts[0]->keys[i].y == attrs[i].y);
    }
#endif
#endif
    printk("geometry runtime provider precondition passed case=%d\n", KP_GEOMETRY_CASE);
}

static void scenario(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    k_sleep(K_MSEC(100));
    CHECK(device_is_ready(DEVICE_DT_GET(DT_NODELABEL(rgb_matrix))));
    CHECK(device_is_ready(DEVICE_DT_GET(DT_NODELABEL(geometry_probe))));
    bool on = false;
    CHECK(zmk_rgb_matrix_get_state(&on) == 0 && on);
    check_runtime_precondition();
    check_geometry();
#if KP_LED_COUNT > 0
    CHECK(k_sem_take(&render_seen, K_SECONDS(2)) == 0);
#endif
    printk("PASS geometry case=%d LEDs=%u\n", KP_GEOMETRY_CASE, KP_LED_COUNT);
    exit(0);
}

K_THREAD_DEFINE(geometry_scenario, 2048, scenario, NULL, NULL, NULL, 5, 0, 0);
