/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>

extern const char *kp_test_scenario;
#define KP_CHECK(expr) do { if (!(expr)) { \
    printk("FAIL %s %s:%u: %s\n", kp_test_scenario, __FILE__, __LINE__, #expr); \
    exit(2); \
} } while (0)
static inline bool kp_test_case(const char *name) {
    return strcmp(kp_test_scenario, name) == 0;
}
static inline void kp_test_complete(void) {
    printk("PASS %s\n", kp_test_scenario);
    exit(0);
}
void kp_test_engine_run(void);
void kp_test_conditions_run(void);
void kp_test_controls_run(void);
void kp_test_effects_run(void);
