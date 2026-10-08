/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT test_composite_mock_strip

#include <string.h>
#include <zephyr/ztest.h>
#include "mock_strip.h"

struct mock_strip_config {
    size_t length;
    int init_error;
};

struct mock_call mock_calls[MOCK_MAX_CALLS];
size_t mock_call_count;

static int mock_update(const struct device *dev, struct led_rgb *pixels, size_t count)
{
    struct mock_strip_data *data = dev->data;

    zassert_true(mock_call_count < MOCK_MAX_CALLS);
    zassert_true(count <= MOCK_MAX_PIXELS);
    struct mock_call *call = &mock_calls[mock_call_count++];

    call->dev = dev;
    call->pixels = pixels;
    call->count = count;
    if (count) {
        memcpy(call->before, pixels, count * sizeof(*pixels));
        if (data->mutate) {
            memset(pixels, 0xa5, count * sizeof(*pixels));
        }
    }
    return data->result;
}

static size_t mock_length(const struct device *dev)
{
    const struct mock_strip_config *config = dev->config;

    return config->length;
}

static int mock_init(const struct device *dev)
{
    const struct mock_strip_config *config = dev->config;

    return -config->init_error;
}

static const struct led_strip_driver_api mock_api_0 = {
    .update_rgb = mock_update,
    .length = mock_length,
};
static const struct led_strip_driver_api mock_api_1 = {
    .length = mock_length,
};
static const struct led_strip_driver_api mock_api_2 = {
    .update_rgb = mock_update,
};

#define MOCK_API_0 (&mock_api_0)
#define MOCK_API_1 (&mock_api_1)
#define MOCK_API_2 (&mock_api_2)
#define MOCK_API_3 NULL

#define MOCK_DEFINE(inst)                                                        \
    static struct mock_strip_data mock_data_##inst;                               \
    static const struct mock_strip_config mock_config_##inst = {                  \
        .length = DT_INST_PROP(inst, runtime_length) ?                            \
                      DT_INST_PROP(inst, runtime_length) :                       \
                      DT_INST_PROP(inst, chain_length),                          \
        .init_error = DT_INST_PROP(inst, init_error),                             \
    };                                                                           \
    DEVICE_DT_INST_DEFINE(inst, mock_init, NULL, &mock_data_##inst,                \
                          &mock_config_##inst, POST_KERNEL, 50,                   \
                          UTIL_CAT(MOCK_API_, DT_INST_PROP(inst, api_kind)));

DT_INST_FOREACH_STATUS_OKAY(MOCK_DEFINE)

#define MOCK_RESET(inst) memset(&mock_data_##inst, 0, sizeof(mock_data_##inst));

void mock_reset(void)
{
    mock_call_count = 0;
    memset(mock_calls, 0, sizeof(mock_calls));
    DT_INST_FOREACH_STATUS_OKAY(MOCK_RESET)
}
