#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Production split-sync connection lifecycle and retry behavior."""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

from test_singleton import ROOT, fixture_code, function, structure


MOCKS = r'''
#include <string.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define ARG_UNUSED(x) ((void)(x))
#define LOG_WRN(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define CONFIG_KEYPAW_RGB_SPLIT_SYNC_SETTLE_MS 100
#define KP_RGB_SYNC_SOURCES 2
#define KP_RGB_SYNC_NO_EFFECT UINT16_MAX
#define KP_RGB_SYNC_STEP_DELAY_MS 50
#define KP_RGB_SYNC_MAX_BACKOFF_MS 1000
#define KP_RGB_SYNC_BACKOFF_SHIFT_MAX 5
#define RGB_LEGACY_EFS_CMD 0x104
#define RGB_LEGACY_COLOR_CMD 0x105
#define RGB_LEGACY_PERIOD_CMD 0x106
#define RGB_BRT_SET_CMD 0x107
#define RGB_ON_CMD 4
#define RGB_OFF_CMD 5
#define RGB_OVL_STATE_CMD 6
#define RGB_SELECTOR_COLOR_CMD 0x108u
#define RGB_SELECTOR_PERIOD_CMD 0x109u
#define KP_RGB_SELECTOR_COMMAND_MASK 0xffffu
#define RGB_EFS_CMD 13u
#define RGB_COLOR_HSB_VAL(h,s,b) (((uint32_t)(h)<<16)|((uint32_t)(s)<<8)|(b))
#define RGB_OVL_STATE_VAL(w,v) (((uint32_t)(w)<<16)|(v))
#define K_MSEC(x) (x)
#define K_NO_WAIT 0
struct k_work { int unused; };
struct k_work_delayable { int unused; };
struct zmk_behavior_binding { const char *behavior_dev; uint32_t param1,param2; };
struct zmk_behavior_binding_event { uint8_t layer,position; int64_t timestamp; uint8_t source; };
struct zmk_split_transport_central_api {
    int (*get_available_source_ids)(uint8_t *ids);
};
struct zmk_split_transport_central {
    const struct zmk_split_transport_central_api *api;
};
static const struct zmk_split_transport_central *selected_transport;
static int available_count = 2;
static uint8_t available_ids[] = {0, 1};
static int64_t now_ms;
static const struct k_work_delayable *scheduled_work;
static int64_t scheduled_delay;
static unsigned transport_queries;
static unsigned sends;
static uint32_t commands[128], arguments[128];
static uint8_t destinations[128];
static bool fail_source[2];
static int behavior_result;
static const struct device *sent_behavior;
static struct zmk_behavior_binding sent_binding;
static struct zmk_behavior_binding_event sent_event;
static int get_ids(uint8_t *ids) {
    assert(!held);
    transport_queries++;
    memcpy(ids, available_ids, (size_t)available_count);
    return available_count;
}
static const struct zmk_split_transport_central *kp_rgb_sync_pick_transport(void) {
    assert(!held);
    return selected_transport;
}
static bool kp_rgb_sync_send(uint8_t source, uint32_t cmd, uint32_t arg) {
    assert(!held && source < 2 && sends < ARRAY_SIZE(commands));
    destinations[sends] = source;
    commands[sends] = cmd;
    arguments[sends++] = arg;
    return !fail_source[source];
}
static int zmk_split_central_invoke_behavior(uint8_t source, struct zmk_behavior_binding *binding,
                                             struct zmk_behavior_binding_event event, bool pressed) {
    assert(!held && source < 2 && pressed);
    sent_behavior = NULL; sent_binding = *binding; sent_event = event;
    return behavior_result;
}
static uint16_t kp_rgb_overlay_word_count(void) { return 1; }
static uint16_t kp_rgb_overlay_get_word(uint16_t word) {
    assert(word == 0);
    return 0x1234;
}
static int64_t k_uptime_get(void) { return now_ms; }
static void *kp_rgb_work_q(void) { return NULL; }
static int k_work_reschedule_for_queue(void *queue, struct k_work_delayable *work,
                                       int64_t delay) {
    ARG_UNUSED(queue);
    scheduled_work = work;
    scheduled_delay = delay;
    return 0;
}
static struct k_work_delayable kp_rgb_sync_clock;
static struct k_work_delayable kp_rgb_sync_step;
'''

TESTS = r'''
static const struct zmk_split_transport_central_api transport_api = {
    .get_available_source_ids = get_ids,
};
static const struct zmk_split_transport_central transport_a = {&transport_api};
static const struct zmk_split_transport_central transport_b = {&transport_api};

static void set_scene(const struct device *device, uint16_t index, bool user_on) {
    kp_rgb_matrix_lock();
    kp_rgb_controller.selector.active_fx = device;
    kp_rgb_global.user_on = user_on;
    kp_rgb_controller.selector.effect_index = index;
    kp_rgb_matrix_unlock();
}

static void reset_test(void) {
    memset(kp_rgb_sync_sources, 0, sizeof(kp_rgb_sync_sources));
    memset(commands, 0, sizeof(commands));
    memset(arguments, 0, sizeof(arguments));
    memset(destinations, 0, sizeof(destinations));
    memset(fail_source, 0, sizeof(fail_source));
    selected_transport = &transport_a;
    kp_rgb_sync_transport = NULL;
    available_count = 2;
    available_ids[0] = 0;
    available_ids[1] = 1;
    now_ms = 0;
    scheduled_work = NULL;
    scheduled_delay = -1;
    sends = 0;
    transport_queries = 0;
    behavior_result = 0; sent_behavior = NULL;
    assert(!held);
}

static void finish_source(unsigned source) {
    unsigned guard = 0;
    while (kp_rgb_sync_sources[source].pending && guard++ < 16) {
        if (now_ms < kp_rgb_sync_sources[source].ready_at) {
            now_ms = kp_rgb_sync_sources[source].ready_at;
        }
        kp_rgb_sync_step_handler(NULL);
        if (kp_rgb_sync_sources[source].pending) {
            now_ms += KP_RGB_SYNC_STEP_DELAY_MS;
        }
    }
    assert(!kp_rgb_sync_sources[source].pending);
}

static void test_initial_connection_and_off_snapshot(void) {
    reset_test();
    struct kp_rgb_effect_common_data data = {{123, 45, 67}, 890};
    const struct device device = {&data, "rgb", NULL};
    set_scene(&device, 3, false);
    kp_rgb_matrix_lock();
    kp_rgb_global.brightness = 73;
    kp_rgb_matrix_unlock();

    kp_rgb_sync_refresh();
    assert(kp_rgb_sync_sources[0].pending && kp_rgb_sync_sources[1].pending);
    assert(kp_rgb_sync_sources[0].ready_at == 100);
    assert(sends == 0 && scheduled_work == &kp_rgb_sync_clock);
    assert(scheduled_delay == CONFIG_KEYPAW_RGB_SPLIT_SYNC_SETTLE_MS);
    kp_rgb_sync_step_handler(NULL);
    assert(sends == 0);
    assert(scheduled_work == &kp_rgb_sync_step);
    assert(scheduled_delay == CONFIG_KEYPAW_RGB_SPLIT_SYNC_SETTLE_MS);

    unsigned locks_before_snapshot = lock_entries;
    now_ms = 100;
    kp_rgb_sync_step_handler(NULL);
    /* Connection refresh and selector refresh lock separately from the flat snapshot. */
    assert(lock_entries == locks_before_snapshot + 3);
    assert(sends == 1 && commands[0] == RGB_LEGACY_EFS_CMD && arguments[0] == 3);
    assert(destinations[0] == 0 && held == 0);
    finish_source(0);
    assert(kp_rgb_sync_sources[0].synced_effect == 3);
    assert(!kp_rgb_sync_sources[0].pending);
    assert(kp_rgb_sync_sources[1].pending);
    assert(commands[1] == RGB_LEGACY_COLOR_CMD);
    assert(arguments[1] == RGB_COLOR_HSB_VAL(123, 45, 67));
    assert(commands[2] == RGB_LEGACY_PERIOD_CMD && arguments[2] == 890);
    assert(commands[3] == RGB_BRT_SET_CMD && arguments[3] == 73);
    assert(commands[4] == RGB_OFF_CMD);
    unsigned before = sends;
    kp_rgb_sync_refresh();
    assert(sends == before && !kp_rgb_sync_sources[0].pending);
}

static void test_disconnect_reconnect_and_transport_replacement(void) {
    reset_test();
    struct kp_rgb_effect_common_data data = {{10, 20, 30}, 700};
    const struct device device = {&data, "rgb", NULL};
    set_scene(&device, 1, true);
    available_count = 1;
    available_ids[0] = 0;
    kp_rgb_sync_refresh();
    now_ms = 100;
    kp_rgb_sync_step_handler(NULL);
    assert(kp_rgb_sync_sources[0].phase == KP_RGB_SYNC_COLOR);
    kp_rgb_sync_sources[0].retry_count = 3;
    available_count = 1;
    available_ids[0] = 1;
    kp_rgb_sync_refresh();
    assert(!kp_rgb_sync_sources[0].seen && !kp_rgb_sync_sources[0].pending);
    assert(kp_rgb_sync_sources[0].synced_effect == KP_RGB_SYNC_NO_EFFECT);

    kp_rgb_matrix_lock();
    data.color = (struct kp_rgb_hsb){75, 80, 90};
    data.duration_ms = 1200;
    kp_rgb_matrix_unlock();
    set_scene(&device, 0, false);
    kp_rgb_matrix_lock();
    kp_rgb_global.brightness = 55;
    kp_rgb_matrix_unlock();

    available_count = 1;
    available_ids[0] = 0;
    now_ms = 200;
    kp_rgb_sync_refresh();
    assert(kp_rgb_sync_sources[0].pending);
    assert(kp_rgb_sync_sources[0].phase == KP_RGB_SYNC_SELECT);
    assert(kp_rgb_sync_sources[0].retry_count == 0);
    assert(kp_rgb_sync_sources[0].ready_at == 300);
    unsigned before = sends;
    kp_rgb_sync_step_handler(NULL);
    assert(sends == before);
    now_ms = 300;
    kp_rgb_sync_step_handler(NULL);
    assert(sends == before + 1 && commands[before] == RGB_LEGACY_EFS_CMD);
    assert(arguments[before] == 0);
    finish_source(0);
    assert(!kp_rgb_sync_sources[0].pending && kp_rgb_sync_sources[0].synced_effect == 0);
    assert(commands[before + 1] == RGB_LEGACY_COLOR_CMD);
    assert(arguments[before + 1] == RGB_COLOR_HSB_VAL(75, 80, 90));
    assert(commands[before + 2] == RGB_LEGACY_PERIOD_CMD && arguments[before + 2] == 1200);
    assert(commands[before + 3] == RGB_BRT_SET_CMD && arguments[before + 3] == 55);
    assert(commands[before + 4] == RGB_OFF_CMD);

    selected_transport = &transport_b;
    now_ms += 50;
    int64_t replacement_ready_at = now_ms + CONFIG_KEYPAW_RGB_SPLIT_SYNC_SETTLE_MS;
    kp_rgb_sync_refresh();
    assert(kp_rgb_sync_sources[0].pending);
    assert(kp_rgb_sync_sources[0].phase == KP_RGB_SYNC_SELECT);
    assert(kp_rgb_sync_sources[0].synced_effect == KP_RGB_SYNC_NO_EFFECT);
    assert(kp_rgb_sync_sources[0].ready_at == replacement_ready_at);
    before = sends;
    kp_rgb_sync_step_handler(NULL);
    assert(sends == before);
}

static void test_failure_backoff_does_not_block_other_source(void) {
    reset_test();
    struct kp_rgb_effect_common_data data = {{1, 2, 3}, 400};
    const struct device device = {&data, "rgb", NULL};
    set_scene(&device, 2, true);
    fail_source[0] = true;
    kp_rgb_sync_refresh();
    now_ms = 100;
    kp_rgb_sync_step_handler(NULL);
    assert(sends == 2 && destinations[0] == 0 && destinations[1] == 1);
    assert(commands[0] == RGB_LEGACY_EFS_CMD && commands[1] == RGB_LEGACY_EFS_CMD);
    assert(kp_rgb_sync_sources[0].phase == KP_RGB_SYNC_SELECT);
    assert(kp_rgb_sync_sources[0].retry_count == 1);
    assert(kp_rgb_sync_sources[0].ready_at == 150);
    assert(kp_rgb_sync_sources[1].phase == KP_RGB_SYNC_COLOR);
    assert(kp_rgb_sync_sources[1].pending);

    fail_source[0] = false;
    now_ms = 150;
    kp_rgb_sync_step_handler(NULL);
    assert(sends == 3 && destinations[2] == 0);
    assert(commands[2] == commands[0] && arguments[2] == arguments[0]);
    assert(kp_rgb_sync_sources[0].phase == KP_RGB_SYNC_COLOR);
    now_ms = 200;
    kp_rgb_sync_step_handler(NULL);
    assert(sends == 4 && destinations[3] == 0);
    assert(commands[3] == RGB_LEGACY_COLOR_CMD);
}

static void test_no_effect_snapshot_keeps_brightness_and_power(void) {
    reset_test();
    set_scene(NULL, 9, false);
    kp_rgb_matrix_lock();
    kp_rgb_global.brightness = 37;
    kp_rgb_matrix_unlock();
    struct kp_rgb_sync_source state = {.phase = KP_RGB_SYNC_SELECT};
    assert(kp_rgb_sync_emit_one(0, &state) == KP_RGB_SYNC_EMIT_SENT);
    assert(sends == 0 && state.phase == KP_RGB_SYNC_BRIGHTNESS);
    assert(kp_rgb_sync_emit_one(0, &state) == KP_RGB_SYNC_EMIT_SENT);
    assert(commands[0] == RGB_BRT_SET_CMD && arguments[0] == 37);
    assert(kp_rgb_sync_emit_one(0, &state) == KP_RGB_SYNC_EMIT_SENT);
    assert(commands[1] == RGB_OFF_CMD && arguments[1] == 0);
    assert(kp_rgb_sync_emit_one(0, &state) == KP_RGB_SYNC_EMIT_SENT);
    assert(commands[2] == RGB_OVL_STATE_CMD);
    assert(kp_rgb_sync_emit_one(0, &state) == KP_RGB_SYNC_EMIT_DONE && sends == 3);
}
static void test_selector_sender_and_global_snapshot(void) {
    reset_test();
    struct kp_rgb_selector_snapshot snapshot = {
        .control = &(const struct device){NULL, "selector-control", NULL},
        .slot = 1, .color = {222, 77, 100}, .period_ms = 3456};
    struct kp_rgb_selector_snapshot original = snapshot;
    now_ms = 765;
    behavior_result = -EIO;
    assert(kp_rgb_split_send_selector(1, &snapshot, 0) == -EIO);
    assert(!held && sent_binding.behavior_dev == snapshot.control->name);
    assert(sent_binding.param1 == KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, 1));
    assert(sent_binding.param2 == RGB_COLOR_HSB_VAL(222, 77, 100));
    assert(sent_event.layer == 0 && sent_event.position == 0 && sent_event.timestamp == 765 && sent_event.source == 1);
    assert(!memcmp(&snapshot, &original, sizeof(snapshot)));
    behavior_result = 0;
    assert(kp_rgb_split_send_selector(0, &snapshot, 1) == 0);
    assert(sent_binding.behavior_dev == snapshot.control->name);
    assert(sent_binding.param1 == KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, 1));
    assert(sent_binding.param2 == 3456 && sent_event.source == 0 && !held);

    struct kp_rgb_sync_source global = {.phase = KP_RGB_SYNC_BRIGHTNESS};
    kp_rgb_matrix_lock(); kp_rgb_global.brightness = 61; kp_rgb_global.user_on = false;
    kp_rgb_global_snapshot_locked(&global.global); kp_rgb_matrix_unlock();
    kp_rgb_global.brightness = 9; kp_rgb_global.user_on = true;
    assert(global.global.brightness == 61 && !global.global.user_on);
    assert(kp_rgb_sync_emit_global(0, &global) == KP_RGB_SYNC_EMIT_SENT);
    assert(commands[0] == RGB_BRT_SET_CMD && arguments[0] == 61 && global.phase == KP_RGB_SYNC_POWER);
    assert(kp_rgb_sync_emit_global(0, &global) == KP_RGB_SYNC_EMIT_SENT);
    assert(commands[1] == RGB_OFF_CMD && arguments[1] == 0);
}


int main(void) {
    test_initial_connection_and_off_snapshot();
    test_disconnect_reconnect_and_transport_replacement();
    test_failure_backoff_does_not_block_other_source();
    test_no_effect_snapshot_keeps_brightness_and_power();
    test_selector_sender_and_global_snapshot();
    puts("split reconnect lifecycle, snapshots, transport replacement and retries passed");
    return 0;
}
'''


def main():
    source = (ROOT / "src/rgb_split_sync.c").read_text()
    enums = "\n".join(
        re.search(r"enum " + name + r" \{.*?\};", source, re.S).group()
        for name in ("kp_rgb_sync_phase", "kp_rgb_sync_emit")
    )
    sync_header = (ROOT / "src/rgb_selector_sync_internal.h").read_text()
    sync_source = (ROOT / "src/rgb_selector_sync.c").read_text()
    sync_api = sync_header[sync_header.index("#define RGB_SELECTOR_COLOR_CMD"):sync_header.index("/* Caller holds")]
    code = fixture_code() + MOCKS + "\n" + sync_api + enums
    code += "\n#define KP_RGB_SYNC_SELECTOR_CAPACITY 1\n"
    code += structure(source, "kp_rgb_global_snapshot")
    code += structure(source, "kp_rgb_sync_selector_progress") + structure(source, "kp_rgb_sync_source")
    code += function(sync_source, "kp_rgb_selector_snapshot_binding") + "\n"
    code += "\n".join(function(source, name) for name in ("kp_rgb_global_snapshot_locked", "kp_rgb_sync_send_binding", "kp_rgb_split_send_selector", "kp_rgb_sync_emit_global")) + "\n"
    code += "\nstatic struct kp_rgb_sync_source kp_rgb_sync_sources[KP_RGB_SYNC_SOURCES];\n"
    code += "static const struct zmk_split_transport_central *kp_rgb_sync_transport;\n"
    # This fixture remains the no-selector legacy reconnect configuration.
    code += structure(source, "kp_rgb_sync_selector")
    code += "static struct kp_rgb_sync_selector kp_rgb_sync_selectors[] = {{0}};\n"
    code += "\n".join(function(source, name) for name in (
        "kp_rgb_sync_selector_count", "kp_rgb_sync_selectors_reset", "kp_rgb_sync_selectors_refresh"))
    code += "\n".join(
        function(source, name)
        for name in ("kp_rgb_sync_refresh", "kp_rgb_sync_emit_one", "kp_rgb_sync_step_handler")
    )
    code += TESTS

    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "split_reconnect.c"
        path.write_text(code)
        binary = Path(directory) / "split_reconnect"
        subprocess.run(
            shlex.split(os.environ.get("CC", "cc"))
            + ["-std=gnu11", "-Wall", "-Wextra", "-Werror", str(path), "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
