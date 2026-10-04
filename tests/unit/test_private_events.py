#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Test production event routing for independently owned base/overlay effects."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
from test_singleton import ROOT, function

MOCKS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
struct device { const void *api; void *data; const void *config; };
struct kp_rgb_key_event { uint32_t position; bool pressed; int64_t timestamp_ms; };
struct kp_rgb_effect_runtime { bool active; };
struct kp_rgb_effect_callbacks {
    bool (*on_event)(const struct device *, const struct kp_rgb_key_event *);
};
struct kp_rgb_effect_api {
    const struct kp_rgb_effect_callbacks *callbacks;
    struct kp_rgb_effect_runtime *runtime;
    const struct device *const *overlays;
    size_t overlays_len;
};
struct kp_rgb_overlay_api { const struct device *(*event_target)(const struct device *); };
static struct { struct { const struct device *active_fx; } state; } kp_rgb_controller;
static bool kp_rgb_output_allowed;
#define atomic_get(p) (*(p))
static bool gates[2] = {true,true};
static bool mapped = true;
static unsigned counts[3];
static bool on_event(const struct device *dev, const struct kp_rgb_key_event *ev) {
    assert(ev->position == 17 && ev->timestamp_ms == 123); (*(unsigned *)dev->data)++; return true;
}
static const struct device *event_target(const struct device *dev) { return dev->config; }
static const struct kp_rgb_effect_callbacks callbacks = {.on_event = on_event};
static struct kp_rgb_effect_runtime states[3] = {{true},{true},{true}};
static const struct kp_rgb_effect_api child_api[] = {
    {.callbacks=&callbacks,.runtime=&states[1]}, {.callbacks=&callbacks,.runtime=&states[2]}
};
static const struct device children[] = {{&child_api[0],&counts[1],NULL},{&child_api[1],&counts[2],NULL}};
static const struct kp_rgb_overlay_api overlay_api = {.event_target = event_target};
static const struct device overlays[] = {
    {&overlay_api,&gates[0],&children[0]}, {&overlay_api,&gates[1],&children[1]}
};
static const struct device *const overlay_list[] = {&overlays[0],&overlays[1]};
static const struct device *const *kp_rgb_overlay_list(void) { return overlay_list; }
static size_t kp_rgb_overlay_count(void) { return 2; }
static bool kp_rgb_overlay_gate(const struct device *dev) { return *(bool *)dev->data; }
static size_t kp_rgb_led_for_position(uint32_t position) { (void)position; return mapped ? 0 : SIZE_MAX; }
'''

TESTS = r'''
int main(void) {
    struct kp_rgb_effect_api base_api = {.callbacks=&callbacks,.runtime=&states[0]};
    const struct device base = {&base_api,&counts[0],NULL};
    kp_rgb_output_allowed = true;
    kp_rgb_controller.state.active_fx = &base;
    const struct kp_rgb_key_event event = {17,true,123};
    kp_rgb_deliver_position(&event);
    assert(counts[0] == 1 && counts[1] == 1 && counts[2] == 1);
    gates[0] = false;
    kp_rgb_deliver_position(&event);
    assert(counts[0] == 2 && counts[1] == 1 && counts[2] == 2);
    gates[0] = true;
    mapped = false;
    kp_rgb_deliver_position(&event);
    assert(counts[0] == 2 && counts[1] == 1 && counts[2] == 2);
    mapped = true;
    const struct device *list[] = {&overlays[0]};
    base_api.overlays = list;
    base_api.overlays_len = 1;
    kp_rgb_deliver_position(&event);
    assert(counts[0] == 3 && counts[1] == 2 && counts[2] == 2);
    base_api.overlays_len = 0;
    kp_rgb_deliver_position(&event);
    assert(counts[0] == 4 && counts[1] == 2 && counts[2] == 2);
    kp_rgb_output_allowed = false;
    kp_rgb_deliver_position(&event);
    assert(counts[0] == 4 && counts[1] == 2 && counts[2] == 2);
    puts("private effect event routing passed");
    return 0;
}
'''


def main():
    source = (ROOT / "src/rgb_matrix.c").read_text()
    code = MOCKS + "\n".join(function(source, name) for name in (
        "kp_rgb_deliver_event", "kp_rgb_deliver_position")) + TESTS
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "events.c"
        path.write_text(code)
        binary = Path(directory) / "events"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(path), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
