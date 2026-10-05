#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Execute production singleton selection/default helpers with host device data.

Devicetree ownership is covered separately by tests/sim/test_configuration.py.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r"^(?:static )?(?:inline )?[\w\s*]+\b" + name + r"\([^;]*?\) \{", source, re.M)
    assert match, name
    start = match.start()
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def structure(source, name):
    match = re.search(r"struct " + name + r" \{.*?^\};", source, re.M | re.S)
    assert match, name
    return match.group()


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#define IS_ENABLED(x) (x)
#define CONFIG_SETTINGS 0
#define CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS 100
#define CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS 10000
#define KP_RGB_BRT_MAX 100
#define KP_RGB_HUE_MAX 360
#define KP_RGB_SAT_MAX 100
#define CLAMP(x,l,h) ((x)<(l)?(l):((x)>(h)?(h):(x)))
struct device { void *data; const char *name; };
struct kp_rgb_hsb { uint16_t h; uint8_t s,b; };
struct kp_rgb_tuning { uint8_t max_brightness,idle_brightness; };
struct kp_rgb_effect_common_data { struct kp_rgb_hsb color; uint16_t duration_ms; };
static struct kp_rgb_effect_common_data *kp_rgb_effect_data(const struct device *dev) {
    return dev->data;
}
static unsigned held, lock_entries, flushes, locked_flushes;
static void zmk_rgb_matrix_flush(void) { flushes++; if(held) locked_flushes++; }
static void kp_rgb_matrix_lock(void) { if (!held) lock_entries++; held++; }
static void kp_rgb_matrix_unlock(void) { assert(held); held--; }
'''

TESTS = r'''
int main(void) {
    struct kp_rgb_effect_common_data data[4] = {0};
    const struct device devices[4] = {
        {&data[0], "base"}, {&data[1], "base-one"},
        {&data[2], "base-two"}, {&data[3], "private-overlay"}
    };
    const struct device *effects[] = {&devices[0], &devices[1], NULL, &devices[2]};
    const struct kp_rgb_effect_defaults defaults[] = {
        {{10, 20, 30}, 0}, {{40, 50, 60}, 700}, {{0}, 0}, {{70, 80, 90}, 0}
    };
    kp_rgb_controller.effects = effects;
    kp_rgb_controller.effect_count = 4;
    kp_rgb_controller.effect_defaults = defaults;
    kp_rgb_controller.initial_on = true;
    kp_rgb_controller.initial_brightness = 120;
    kp_rgb_controller.initial_duration_ms = 1;
    kp_rgb_controller.initial_effect = 0;
    data[3].color.h = 321;
    data[3].duration_ms = 456;
    assert(kp_rgb_apply_defaults() == 0);
    assert(kp_rgb_controller.state.user_on);
    assert(kp_rgb_controller.state.active_fx == &devices[0]);
    assert(data[0].color.h == 10 && data[0].color.b == 100);
    assert(data[0].duration_ms == 100 && data[1].duration_ms == 700);
    assert(data[3].color.h == 321 && data[3].duration_ms == 456);
    assert(kp_rgb_effect_count() == 4);
    assert(kp_rgb_effect_at(2) == NULL && kp_rgb_effect_at(4) == NULL);
    assert(zmk_rgb_matrix_select_effect(2) == -ENOENT);
    assert(zmk_rgb_matrix_select_effect(4) == -EINVAL);
    assert(kp_rgb_selected_effect() == 0);
    assert(zmk_rgb_matrix_cycle_effect(1) == 0 && kp_rgb_selected_effect() == 1);
    assert(zmk_rgb_matrix_cycle_effect(1) == 0 && kp_rgb_selected_effect() == 3); /* Skip disabled. */
    assert(zmk_rgb_matrix_cycle_effect(1) == 0 && kp_rgb_selected_effect() == 0); /* Wrap. */
    assert(zmk_rgb_matrix_cycle_effect(-1) == 0 && kp_rgb_selected_effect() == 3); /* Wrap. */
    assert(zmk_rgb_matrix_cycle_effect(-1) == 0 && kp_rgb_selected_effect() == 1); /* Skip disabled. */
    assert(zmk_rgb_matrix_cycle_effect(-1) == 0 && kp_rgb_selected_effect() == 0);
    assert(zmk_rgb_matrix_select_effect(1) == 0 && kp_rgb_selected_effect() == 1);
    unsigned before=flushes, locked_before=locked_flushes;
    assert(kp_rgb_set_hsb((struct kp_rgb_hsb){222,50,80}) == 0);
    assert(flushes==before+1 && locked_flushes==locked_before+1);
    assert(kp_rgb_set_hsb((struct kp_rgb_hsb){222,50,80}) == 0);
    assert(flushes==before+1);
    assert(kp_rgb_set_hsb((struct kp_rgb_hsb){223,50,80}) == 0);
    assert(kp_rgb_set_hsb((struct kp_rgb_hsb){223,51,80}) == 0);
    assert(kp_rgb_set_hsb((struct kp_rgb_hsb){223,51,81}) == 0);
    assert(flushes==before+4 && locked_flushes==locked_before+4);
    assert(kp_rgb_set_hsb((struct kp_rgb_hsb){222,50,80}) == 0);
    before=flushes;locked_before=locked_flushes;
    assert(kp_rgb_set_duration(1234) == 0);
    assert(flushes==before+1 && locked_flushes==locked_before+1);
    assert(kp_rgb_set_duration(1234) == 0 && flushes==before+1);
    assert(kp_rgb_set_duration(0) == 0);
    assert(data[1].duration_ms==100 && flushes==before+2);
    assert(kp_rgb_set_duration(1) == 0 && flushes==before+2);
    assert(kp_rgb_set_duration(20000) == 0);
    assert(data[1].duration_ms==10000 && flushes==before+3);
    assert(kp_rgb_set_duration(10001) == 0 && flushes==before+3);
    assert(kp_rgb_set_duration(1234) == 0);
    assert(flushes==before+4 && locked_flushes==locked_before+4);
    before=flushes;
    assert(kp_rgb_set_hsb((struct kp_rgb_hsb){361,50,80}) == -EINVAL);
    assert(kp_rgb_set_hsb((struct kp_rgb_hsb){222,101,80}) == -EINVAL);
    assert(kp_rgb_set_hsb((struct kp_rgb_hsb){222,50,101}) == -EINVAL);
    assert(flushes==before);
    kp_rgb_controller.state.active_fx=NULL;
    assert(kp_rgb_set_hsb((struct kp_rgb_hsb){222,50,80}) == -ENODEV);
    assert(kp_rgb_set_duration(500) == -ENODEV && flushes==before);
    kp_rgb_controller.state.active_fx=&devices[1];
    assert(zmk_rgb_matrix_select_effect(0) == 0);
    assert(data[1].color.h == 222 && data[0].color.h == 10 && data[3].color.h == 321);
    assert(data[1].duration_ms == 1234 && data[0].duration_ms == 100 && data[3].duration_ms == 456);
    kp_rgb_controller.initial_effect = 99;
    kp_rgb_controller.initial_on = false;
    assert(kp_rgb_apply_defaults() == 0);
    assert(kp_rgb_selected_effect() == 0 && !kp_rgb_controller.state.user_on);
    assert(data[1].color.h == 40 && data[3].color.h == 321);
    assert(held == 0);
    puts("singleton selection/defaults/private parameter isolation passed");
    return 0;
}
'''


def fixture_code():
    internal = (ROOT / "src/rgb_matrix_internal.h").read_text()
    source = (ROOT / "src/behavior_rgb_matrix.c").read_text()
    code = PRELUDE + "\n".join(structure(internal, name) for name in (
        "kp_rgb_state", "kp_rgb_effect_defaults", "kp_rgb_controller"))
    code += r'''
static struct kp_rgb_controller kp_rgb_controller;
static unsigned reconciles;
static bool reconciled_user_on;
static void kp_rgb_reconcile_power_locked(void) {
    assert(held >= 1);
    reconciles++;
    reconciled_user_on = kp_rgb_controller.state.user_on;
}
'''
    code += "\n".join(function(source, name) for name in (
        "kp_rgb_effect_count", "kp_rgb_effect_at", "kp_rgb_selected_effect",
        "kp_rgb_calc_effect_index", "kp_rgb_resolve_active",
        "kp_rgb_apply_defaults", "kp_rgb_select_effect", "zmk_rgb_matrix_select_effect",
        "zmk_rgb_matrix_cycle_effect", "kp_rgb_set_hsb", "kp_rgb_set_duration"))
    return code


def conversion_fixture_code():
    header = (ROOT / "include/zmk/rgb_matrix.h").read_text()
    source = (ROOT / "src/behavior_rgb_matrix.c").read_text()
    code = fixture_code().replace(
        "struct device { void *data; const char *name; };",
        "struct device { void *data; const char *name; const void *config; };")
    code += "\n" + structure(header, "kp_rgb_effect_common_config")
    code += "\n" + function(header, "kp_rgb_effect_cfg")
    code += r'''
#define ARG_UNUSED(x) (void)(x)
#define RGB_EFS_CMD 13
struct zmk_behavior_binding { const char *behavior_dev; uint32_t param1, param2; };
struct zmk_behavior_binding_event { int64_t timestamp; };
static const struct device *bound_device;
static const struct device *zmk_behavior_get_binding(const char *name) {
    ARG_UNUSED(name);
    return bound_device;
}
'''
    code += function(source, "kp_rgb_effect_convert_central_state_dependent_params")
    code += r'''
static void check_conversion(const struct device *dev, int expected, uint16_t index) {
    bound_device = dev;
    const char *original_name = "effect";
    struct zmk_behavior_binding binding = {original_name, 123, 456};
    struct zmk_behavior_binding_event event = {0};
    assert(kp_rgb_effect_convert_central_state_dependent_params(&binding, event) == expected);
    if (expected == 0) {
        assert(binding.behavior_dev == kp_rgb_controller.dev->name);
        assert(binding.param1 == RGB_EFS_CMD && binding.param2 == index);
    } else {
        assert(binding.behavior_dev == original_name);
        assert(binding.param1 == 123 && binding.param2 == 456);
    }
}
int main(void) {
    const struct device controller = {.name = "rgb"};
    struct kp_rgb_effect_common_config configs[] = {{0}, {2}, {0}, {3}, {1}};
    const struct device devices[] = {
        {.config = &configs[0]}, {.config = &configs[1]},
        {.config = &configs[2]}, {.config = &configs[3]},
        {.config = &configs[4]}, {0}
    };
    const struct device *effects[] = {&devices[0], NULL, &devices[1]};
    kp_rgb_controller.dev = &controller;
    kp_rgb_controller.effects = effects;
    kp_rgb_controller.effect_count = 3;
    check_conversion(&devices[0], 0, 0);
    check_conversion(&devices[1], 0, 2);
    check_conversion(&devices[2], -ENODEV, 0); /* Private effect with a colliding index. */
    check_conversion(&devices[3], -ENODEV, 0); /* Out of range. */
    check_conversion(&devices[4], -ENODEV, 0); /* Disabled slot. */
    check_conversion(&devices[5], -ENODEV, 0); /* Missing config. */
    check_conversion(NULL, -ENODEV, 0);
    configs[1].index = 0;
    check_conversion(&devices[1], -ENODEV, 0); /* Do not fall back to an array scan. */
    puts("singleton direct-index binding conversion passed");
    return 0;
}
'''
    return code


def main():
    fixtures = {"singleton": fixture_code() + TESTS,
                "conversion": conversion_fixture_code()}
    with tempfile.TemporaryDirectory() as directory:
        for name, code in fixtures.items():
            path = Path(directory) / f"{name}.c"
            path.write_text(code)
            binary = Path(directory) / name
            subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
                "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(path), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
