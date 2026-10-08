/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COMPOSITE_MOCK_STRIP_H
#define COMPOSITE_MOCK_STRIP_H

#include <zephyr/drivers/led_strip.h>

#define MOCK_MAX_PIXELS 16
#define MOCK_MAX_CALLS 16

struct mock_strip_data {
    int result;
    bool mutate;
};

struct mock_call {
    const struct device *dev;
    struct led_rgb *pixels;
    size_t count;
    struct led_rgb before[MOCK_MAX_PIXELS];
};

extern struct mock_call mock_calls[MOCK_MAX_CALLS];
extern size_t mock_call_count;
void mock_reset(void);

#endif
