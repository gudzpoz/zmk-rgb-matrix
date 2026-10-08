/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>
#include <zephyr/ztest.h>
#include "mock_strip.h"

BUILD_ASSERT(IS_ENABLED(CONFIG_LED_STRIP_COMPOSITE));
BUILD_ASSERT(!IS_ENABLED(CONFIG_KEYPAW_RGB_MATRIX));

#define DEV(label) DEVICE_DT_GET(DT_NODELABEL(label))

static const struct device *const leaves[] = {DEV(a), DEV(b), DEV(c)};
static const size_t lengths[] = {2, 3, 1};

static void before(void *fixture)
{
    ARG_UNUSED(fixture);
    mock_reset();
}

static void colors(struct led_rgb *pixels, size_t count)
{
    memset(pixels, 0, count * sizeof(*pixels));
    for (size_t i = 0; i < count; i++) {
        pixels[i].r = i + 1;
        pixels[i].g = i + 31;
        pixels[i].b = i + 61;
    }
}

static void call_is(size_t index, const struct device *dev, struct led_rgb *ptr,
                    size_t count, const struct led_rgb *expected)
{
    zassert_true(index < mock_call_count);
    const struct mock_call *call = &mock_calls[index];

    zassert_equal(call->dev, dev, "wrong child at call %zu", index);
    zassert_equal(call->pixels, ptr, "driver must pass the caller's slice");
    zassert_equal(call->count, count);
    zassert_mem_equal(call->before, expected, count * sizeof(*expected));
}

static void check_prefix(const struct device *dev, size_t total)
{
    for (size_t prefix = 0; prefix <= total; prefix++) {
        struct led_rgb pixels[8], expected[8];
        size_t offset = 0, calls = 0;

        mock_reset();
        colors(pixels, ARRAY_SIZE(pixels));
        memcpy(expected, pixels, sizeof(pixels));
        for (size_t i = 0; i < ARRAY_SIZE(leaves); i++) {
            ((struct mock_strip_data *)leaves[i]->data)->mutate = true;
        }
        zassert_ok(led_strip_update_rgb(dev, pixels, prefix));
        for (size_t i = 0; i < ARRAY_SIZE(leaves) && offset < prefix; i++) {
            size_t count = MIN(lengths[i], prefix - offset);

            call_is(calls++, leaves[i], &pixels[offset], count, &expected[offset]);
            for (size_t j = offset; j < offset + count; j++) {
                zassert_equal(pixels[j].r, 0xa5);
                zassert_equal(pixels[j].g, 0xa5);
                zassert_equal(pixels[j].b, 0xa5);
            }
            offset += count;
        }
        zassert_equal(mock_call_count, calls);
        zassert_mem_equal(&pixels[prefix], &expected[prefix],
                          (ARRAY_SIZE(pixels) - prefix) * sizeof(pixels[0]));
    }
}

ZTEST(composite, test_lengths_and_dependency_initialization)
{
    const struct device *const devices[] = {
        DEV(one), DEV(many), DEV(reverse), DEV(nested), DEV(deeper), DEV(alias_c),
    };
    const size_t totals[] = {1, 5, 5, 6, 6, 1};

    for (size_t i = 0; i < ARRAY_SIZE(devices); i++) {
        zassert_true(device_is_ready(devices[i]), "%s not ready", devices[i]->name);
        zassert_equal(devices[i]->state->init_res, 0);
        zassert_equal(led_strip_length(devices[i]), totals[i]);
    }
}

ZTEST(composite, test_single_and_ordered_instances)
{
    struct led_rgb pixels[5], expected[5];

    colors(pixels, ARRAY_SIZE(pixels));
    memcpy(expected, pixels, sizeof(pixels));
    zassert_ok(led_strip_update_rgb(DEV(one), pixels, 1));
    call_is(0, DEV(c), pixels, 1, expected);
    zassert_equal(mock_call_count, 1);
    mock_reset();
    zassert_ok(led_strip_update_rgb(DEV(reverse), pixels, 5));
    call_is(0, DEV(b), pixels, 3, expected);
    call_is(1, DEV(a), pixels + 3, 2, expected + 3);
    zassert_equal(mock_call_count, 2);
}

ZTEST(composite, test_every_prefix_and_mutating_children)
{
    check_prefix(DEV(many), 5);
    check_prefix(DEV(nested), 6);
    check_prefix(DEV(deeper), 6);
}

ZTEST(composite, test_argument_errors_and_channels)
{
    const struct device *const devices[] = {DEV(one), DEV(many), DEV(nested)};
    struct led_rgb pixels[8];
    uint8_t channels[3] = {1, 2, 3};

    colors(pixels, ARRAY_SIZE(pixels));
    for (size_t i = 0; i < ARRAY_SIZE(devices); i++) {
        size_t length = led_strip_length(devices[i]);

        zassert_ok(led_strip_update_rgb(devices[i], NULL, 0));
        zassert_ok(led_strip_update_rgb(devices[i], pixels, 0));
        zassert_equal(led_strip_update_rgb(devices[i], NULL, 1), -EINVAL);
        zassert_equal(led_strip_update_rgb(devices[i], pixels, length + 1), -ERANGE);
        zassert_equal(led_strip_update_rgb(devices[i], NULL, length + 1), -ERANGE);
        zassert_equal(led_strip_update_channels(devices[i], channels, 3), -ENOSYS);
    }
    zassert_equal(mock_call_count, 0);
}

ZTEST(composite, test_first_failure_and_continued_dispatch)
{
    struct led_rgb pixels[6], expected[6];
    const int errors[] = {-EIO, -ETIMEDOUT, -EBUSY};

    for (size_t first = 0; first < ARRAY_SIZE(leaves); first++) {
        mock_reset();
        colors(pixels, ARRAY_SIZE(pixels));
        memcpy(expected, pixels, sizeof(pixels));
        for (size_t i = 0; i < ARRAY_SIZE(leaves); i++) {
            struct mock_strip_data *data = leaves[i]->data;

            data->result = i >= first ? errors[i] : 0;
            data->mutate = true;
        }
        zassert_equal(led_strip_update_rgb(DEV(deeper), pixels, 6), errors[first]);
        zassert_equal(mock_call_count, 3);
        call_is(0, DEV(a), pixels, 2, expected);
        call_is(1, DEV(b), pixels + 2, 3, expected + 2);
        call_is(2, DEV(c), pixels + 5, 1, expected + 5);
    }
}

ZTEST(composite, test_only_negative_results_are_errors)
{
    struct led_rgb pixels[6];

    colors(pixels, ARRAY_SIZE(pixels));
    ((struct mock_strip_data *)DEV(a)->data)->result = 1;
    ((struct mock_strip_data *)DEV(b)->data)->result = -EIO;
    zassert_equal(led_strip_update_rgb(DEV(nested), pixels, 6), -EIO);
    zassert_equal(mock_call_count, 3);
    mock_reset();
    ((struct mock_strip_data *)DEV(a)->data)->result = 1;
    zassert_ok(led_strip_update_rgb(DEV(nested), pixels, 6));
    zassert_equal(mock_call_count, 3);
}

ZTEST(composite, test_failure_at_partial_boundary)
{
    struct led_rgb pixels[6], expected[6];

    colors(pixels, ARRAY_SIZE(pixels));
    memcpy(expected, pixels, sizeof(pixels));
    ((struct mock_strip_data *)DEV(a)->data)->result = -EIO;
    ((struct mock_strip_data *)DEV(a)->data)->mutate = true;
    ((struct mock_strip_data *)DEV(b)->data)->result = -EBUSY;
    zassert_equal(led_strip_update_rgb(DEV(nested), pixels, 3), -EIO);
    zassert_equal(mock_call_count, 2);
    call_is(0, DEV(a), pixels, 2, expected);
    call_is(1, DEV(b), pixels + 2, 1, expected + 2);
    zassert_mem_equal(pixels + 3, expected + 3, 3 * sizeof(pixels[0]));
}

ZTEST(composite, test_black_and_fresh_retry_buffers)
{
    struct led_rgb pixels[6], expected[6];

    /* The caller reconstructs each retry: children may mutate failed submissions. */
    for (size_t attempt = 0; attempt < 4; attempt++) {
        mock_reset();
        if (attempt < 2) {
            memset(pixels, 0, sizeof(pixels));
        } else {
            colors(pixels, ARRAY_SIZE(pixels));
        }
        memcpy(expected, pixels, sizeof(pixels));
        for (size_t i = 0; i < ARRAY_SIZE(leaves); i++) {
            struct mock_strip_data *data = leaves[i]->data;

            data->mutate = true;
            data->result = attempt % 2 == 0 ? -EIO : 0;
        }
        zassert_equal(led_strip_update_rgb(DEV(nested), pixels, 6),
                      attempt % 2 == 0 ? -EIO : 0);
        zassert_equal(mock_call_count, 3);
        call_is(0, DEV(a), pixels, 2, expected);
        call_is(1, DEV(b), pixels + 2, 3, expected + 2);
        call_is(2, DEV(c), pixels + 5, 1, expected + 5);
    }
}

ZTEST(composite, test_runtime_init_errors)
{
    const struct device *const devices[] = {
        DEV(bad_ready), DEV(bad_rgb), DEV(bad_length), DEV(bad_runtime),
        DEV(bad_overlap), DEV(bad_siblings), DEV(bad_api),
    };
    const int errors[] = {ENODEV, ENOTSUP, ENOTSUP, EINVAL, EINVAL, EINVAL, ENOTSUP};

    zassert_false(device_is_ready(DEV(unready)));
    zassert_equal(DEV(unready)->state->init_res, EIO);
    zassert_true(device_is_ready(DEV(no_rgb)));
    zassert_true(device_is_ready(DEV(no_length)));
    zassert_true(device_is_ready(DEV(mismatch)));
    zassert_true(device_is_ready(DEV(no_api)));
    for (size_t i = 0; i < ARRAY_SIZE(devices); i++) {
        zassert_true(devices[i]->state->initialized);
        zassert_false(device_is_ready(devices[i]), "%s unexpectedly ready", devices[i]->name);
        zassert_equal(devices[i]->state->init_res, errors[i], "%s", devices[i]->name);
    }
    zassert_equal(mock_call_count, 0);
}

ZTEST_SUITE(composite, NULL, NULL, before, NULL, NULL);
