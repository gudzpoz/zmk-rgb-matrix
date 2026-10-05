#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Compile and exercise the production effect-instance callback helpers."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src/rgb_instance.c").read_text()
SOURCE = SOURCE.replace('#include "rgb_matrix_internal.h"', "")

MOCKS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct kp_rgb_key_event { uint32_t position; bool pressed; int64_t timestamp_ms; };
struct kp_rgb_frame { int64_t now_ms; uint32_t elapsed_ms; int marker; };
struct device { void *data; const void *config; const void *api; };
struct kp_rgb_effect_callbacks {
    bool (*render)(const struct device *, const struct kp_rgb_frame *);
    bool (*on_event)(const struct device *, const struct kp_rgb_key_event *);
    void (*set_active)(const struct device *, bool, int64_t);
    void (*reset)(const struct device *, int64_t);
};
struct kp_rgb_effect_api { struct kp_rgb_effect_callbacks behavior; const struct kp_rgb_effect_callbacks *callbacks; };
struct kp_rgb_callback_state {
    int64_t last_render_ms;
    bool initialized, active, animating;
};
struct kp_rgb_effect_instance {
    const struct device *effect;
    struct kp_rgb_callback_state state;
};
#define KP_RGB_EFFECT_INSTANCE_INIT(effect_dev) { .effect = (effect_dev) }

void kp_rgb_callbacks_set_active(const struct device *, const struct kp_rgb_effect_callbacks *,
                                 struct kp_rgb_callback_state *, bool, int64_t);
void kp_rgb_callbacks_reset(const struct device *, const struct kp_rgb_effect_callbacks *,
                            struct kp_rgb_callback_state *, int64_t);
bool kp_rgb_callbacks_on_event(const struct device *, const struct kp_rgb_effect_callbacks *,
                               const struct kp_rgb_callback_state *, const struct kp_rgb_key_event *);
bool kp_rgb_callbacks_render(const struct device *, const struct kp_rgb_effect_callbacks *,
                             struct kp_rgb_callback_state *, const struct kp_rgb_frame *);
void kp_rgb_effect_instance_set_active(struct kp_rgb_effect_instance *, bool, int64_t);
void kp_rgb_effect_instance_reset(struct kp_rgb_effect_instance *, int64_t);
bool kp_rgb_effect_instance_on_event(struct kp_rgb_effect_instance *, const struct kp_rgb_key_event *);
bool kp_rgb_effect_instance_render(struct kp_rgb_effect_instance *, const struct kp_rgb_frame *);
void kp_rgb_effect_instance_restart_clock(struct kp_rgb_effect_instance *);
bool kp_rgb_effect_render(const struct device *, const struct kp_rgb_frame *);

static int reset_count, active_count, event_count, render_count, callback_order;
static int reset_order, active_order;
static bool animation_result, event_result;
static uint32_t elapsed_seen;
static int64_t reset_time, active_time;
static bool active_value;
static int marker_seen;
static const struct device *expected_dev;

static void reset_cb(const struct device *dev, int64_t now) {
    assert(dev == expected_dev); reset_count++; reset_time = now; reset_order = ++callback_order;
}
static void active_cb(const struct device *dev, bool active, int64_t now) {
    assert(dev == expected_dev); active_count++; active_value = active; active_time = now;
    active_order = ++callback_order;
}
static bool event_cb(const struct device *dev, const struct kp_rgb_key_event *event) {
    assert(dev == expected_dev); assert(event->position == 17); event_count++; return event_result;
}
static bool render_cb(const struct device *dev, const struct kp_rgb_frame *frame) {
    assert(dev == expected_dev); render_count++; elapsed_seen = frame->elapsed_ms;
    marker_seen = frame->marker; return animation_result;
}
static const struct kp_rgb_effect_callbacks callbacks = {
    .render = render_cb, .on_event = event_cb, .set_active = active_cb, .reset = reset_cb,
};
static const struct kp_rgb_effect_callbacks optional_callbacks = {.render = render_cb};
static struct kp_rgb_effect_api api = {.callbacks = &callbacks};
static struct kp_rgb_effect_api optional_api = {.callbacks = &optional_callbacks};
static struct device device_a = {.api = &api};
static struct device device_b = {.api = &api};
static struct device device_optional = {.api = &optional_api};

int main(void) {
    struct kp_rgb_effect_instance a = KP_RGB_EFFECT_INSTANCE_INIT(&device_a);
    struct kp_rgb_effect_instance b = KP_RGB_EFFECT_INSTANCE_INIT(&device_b);
    struct kp_rgb_effect_instance optional = KP_RGB_EFFECT_INSTANCE_INIT(&device_optional);
    struct kp_rgb_key_event event = {.position = 17, .pressed = true};
    struct kp_rgb_frame frame = {.now_ms = 100, .marker = 42};
    expected_dev = &device_a;

    /* An inactive false sample is inert; first activation resets before activate. */
    kp_rgb_effect_instance_set_active(&a, false, 9);
    assert(!a.state.initialized && reset_count == 0 && active_count == 0);
    kp_rgb_effect_instance_set_active(&a, true, 10);
    assert(a.state.initialized && a.state.active && !a.state.animating);
    assert(reset_count == 1 && reset_time == 10 && active_count == 1);
    assert(reset_order < active_order);
    assert(active_value && active_time == 10);
    kp_rgb_effect_instance_set_active(&a, true, 11);
    assert(reset_count == 1 && active_count == 1);

    event_result = true;
    assert(kp_rgb_effect_instance_on_event(&a, &event));
    assert(event_count == 1);
    kp_rgb_effect_instance_set_active(&a, false, 12);
    assert(!a.state.active && !a.state.animating && active_count == 2);
    assert(!kp_rgb_effect_instance_on_event(&a, &event) && event_count == 1);
    kp_rgb_effect_instance_set_active(&a, false, 13);
    assert(active_count == 2);

    /* Explicit reset preserves activity and timestamp/state-independent params. */
    kp_rgb_effect_instance_set_active(&a, true, 20);
    animation_result = true;
    frame.now_ms = 100;
    assert(kp_rgb_effect_instance_render(&a, &frame));
    assert(elapsed_seen == 0 && a.state.last_render_ms == 100);
    frame.now_ms = 140;
    assert(kp_rgb_effect_instance_render(&a, &frame));
    assert(elapsed_seen == 40);
    kp_rgb_effect_instance_reset(&a, 141);
    assert(a.state.active && a.state.initialized && !a.state.animating);
    assert(reset_count == 2 && reset_time == 141);
    frame.now_ms = 200;
    assert(kp_rgb_effect_instance_render(&a, &frame) && elapsed_seen == 0);
    animation_result = false;
    frame.now_ms = 220;
    assert(!kp_rgb_effect_instance_render(&a, &frame));
    animation_result = true;
    frame.now_ms = 1000;
    assert(kp_rgb_effect_instance_render(&a, &frame) && elapsed_seen == 0);
    frame.now_ms = 990;
    assert(kp_rgb_effect_instance_render(&a, &frame) && elapsed_seen == 0);

    /* Duration subtraction saturates across the full signed timestamp range. */
    frame.now_ms = INT64_MIN;
    kp_rgb_effect_instance_reset(&a, INT64_MIN);
    assert(kp_rgb_effect_instance_render(&a, &frame) && elapsed_seen == 0);
    frame.now_ms = INT64_MAX;
    assert(kp_rgb_effect_instance_render(&a, &frame) && elapsed_seen == UINT32_MAX);

    /* Restart after a coalesced owner pause affects an unrendered child too. */
    frame.now_ms = 5;
    kp_rgb_effect_instance_render(&a, &frame);
    assert(elapsed_seen == 0);
    frame.now_ms = 15;
    kp_rgb_effect_instance_render(&a, &frame);
    assert(elapsed_seen == 10);
    kp_rgb_effect_instance_restart_clock(&a);
    frame.now_ms = 500;
    kp_rgb_effect_instance_render(&a, &frame);
    assert(elapsed_seen == 0);

    /* Child clocks are independent even when both use the same callback table. */
    expected_dev = &device_b;
    kp_rgb_effect_instance_set_active(&b, true, 0);
    frame.now_ms = 20;
    kp_rgb_effect_instance_render(&b, &frame);
    assert(elapsed_seen == 0);
    frame.now_ms = 30;
    kp_rgb_effect_instance_render(&b, &frame);
    assert(elapsed_seen == 10);
    kp_rgb_effect_instance_restart_clock(&b); /* Child b was skipped during the pause. */
    frame.now_ms = 500;
    kp_rgb_effect_instance_render(&b, &frame);
    assert(elapsed_seen == 0);
    expected_dev = &device_a;
    frame.now_ms = 510;
    kp_rgb_effect_instance_render(&a, &frame);
    assert(elapsed_seen == 10);

    /* Raw dispatch does not manage activity or alter callback timing input. */
    frame.elapsed_ms = 77;
    frame.marker = 123;
    int prior_renders = render_count;
    assert(kp_rgb_effect_render(&device_a, &frame));
    assert(render_count == prior_renders + 1 && elapsed_seen == 77 && marker_seen == 123);
    expected_dev = &device_optional;
    assert(kp_rgb_effect_render(&device_optional, &frame));
    expected_dev = &device_a;

    /* Optional callbacks may be absent when the required render callback exists. */
    expected_dev = &device_optional;
    assert(!kp_rgb_effect_instance_on_event(&optional, &event));
    kp_rgb_effect_instance_set_active(&optional, true, 8);
    assert(optional.state.active && optional.state.initialized);
    assert(kp_rgb_effect_instance_render(&optional, &frame));
    kp_rgb_effect_instance_reset(&optional, 9);
    kp_rgb_effect_instance_set_active(&optional, false, 10);
    assert(!optional.state.active && optional.state.initialized);
    return 0;
}
'''


def main():
    compiler = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="rgb-effect-instances-") as tmp:
        source = Path(tmp) / "test.c"
        binary = Path(tmp) / "test"
        source.write_text(MOCKS + SOURCE + "\n")
        subprocess.run(compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror",
                                   str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("effect instances: production callbacks and independent timing passed")


if __name__ == "__main__":
    main()
