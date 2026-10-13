#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Production global reconnect, frozen snapshot, retries and shared clock."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
from test_singleton import ROOT, function, structure

PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define ARG_UNUSED(x) ((void)(x))
#define LOG_WRN(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define KP_RGB_SYNC_SOURCES 2
#define KP_RGB_SYNC_SELECTOR_CAPACITY 2
#define KP_RGB_SYNC_STEP_DELAY_MS 50
#define KP_RGB_SYNC_MAX_BACKOFF_MS 1000
#define KP_RGB_SYNC_BACKOFF_SHIFT_MAX 5
#define CONFIG_KEYPAW_RGB_SPLIT_SYNC_SETTLE_MS 100
#define CONFIG_KEYPAW_RGB_SPLIT_SYNC_CLOCK_MS 30000
#define K_MSEC(x) (x)
#define K_NO_WAIT 0
#define RGB_BRT_SET_CMD 0x107
#define RGB_ON_CMD 4
#define RGB_OFF_CMD 5
#define RGB_OVL_STATE_CMD 6
#define RGB_CLOCK_CMD 7
#define RGB_OVL_STATE_VAL(w,v) (((uint32_t)(w)<<16)|(v))
struct device { const char *name; bool ready; };
struct kp_rgb_hsb { uint16_t h; uint8_t s,b; };
struct k_work { int unused; };
struct k_work_delayable { int unused; };
struct zmk_behavior_binding { const char *behavior_dev; uint32_t param1,param2; };
struct zmk_behavior_binding_event { uint8_t layer,position; int64_t timestamp; uint8_t source; };
struct zmk_split_transport_central_api { int (*get_available_source_ids)(uint8_t *); };
struct zmk_split_transport_central { const struct zmk_split_transport_central_api *api; };
static struct { bool user_on; uint8_t brightness; } kp_rgb_global;
static unsigned held;
static void kp_rgb_matrix_lock(void) { assert(!held); held = 1; }
static void kp_rgb_matrix_unlock(void) { assert(held); held = 0; }
static struct device endpoint = {"global", true};
static const struct device *const kp_rgb_sync_endpoint = &endpoint;
static const struct device owners[] = {{"selector-a",true},{"selector-b",true}};
static bool device_is_ready(const struct device *d) { return d->ready; }
static int64_t now_ms, scheduled_delay;
static struct k_work_delayable kp_rgb_sync_step, kp_rgb_sync_clock, kp_rgb_sync_selector_step;
static struct k_work_delayable *scheduled_work;
static size_t kp_rgb_sync_selector_cursor;
static int64_t k_uptime_get(void) { return now_ms; }
static void *kp_rgb_work_q(void) { return NULL; }
static int k_work_reschedule_for_queue(void *q, struct k_work_delayable *w, int64_t delay) {
 (void)q; scheduled_work = w; scheduled_delay = delay; return 0;
}
static unsigned sends, available_count = 2;
static uint8_t ids[] = {0,1};
static bool fail_source[2];
static int64_t send_cost;
static struct zmk_behavior_binding bindings[256];
static uint8_t destinations[256];
static int get_ids(uint8_t *out) { assert(!held); memcpy(out,ids,available_count); return available_count; }
static const struct zmk_split_transport_central_api api = {get_ids};
static const struct zmk_split_transport_central transports[] = {{&api},{&api}};
static const struct zmk_split_transport_central *selected_transport = &transports[0];
static const struct zmk_split_transport_central *kp_rgb_sync_pick_transport(void) {
 assert(!held); return selected_transport;
}
static int zmk_split_central_invoke_behavior(uint8_t source, struct zmk_behavior_binding *b,
 struct zmk_behavior_binding_event event, bool pressed) {
 assert(!held && source < 2 && sends < ARRAY_SIZE(bindings) && pressed);
 assert(event.source == source && event.timestamp == now_ms);
 bindings[sends] = *b; destinations[sends++] = source; now_ms += send_cost;
 return fail_source[source] ? -EIO : 0;
}
static uint16_t overlay_words[] = {0x1234,0xabcd};
static uint16_t kp_rgb_overlay_word_count(void) { return ARRAY_SIZE(overlay_words); }
static uint16_t kp_rgb_overlay_get_word(uint16_t w) { assert(w < 2); return overlay_words[w]; }
'''

SELECTOR_MOCKS = r'''
static uint16_t selected_slots[2];
static unsigned captures[2];
static int kp_rgb_selector_snapshot_locked(const struct device *owner, struct kp_rgb_selector_snapshot *s) {
 assert(held); size_t i = (size_t)(owner - owners); assert(i < 2); captures[i]++;
 *s = (struct kp_rgb_selector_snapshot){.control=owner,.slot=selected_slots[i],
 .color={123,45,67},.period_ms=890}; return 0;
}
static int kp_rgb_selector_snapshot_binding(const struct kp_rgb_selector_snapshot *s,
 uint8_t step, struct zmk_behavior_binding *b) {
 assert(step < 3); *b = (struct zmk_behavior_binding){s->control->name,
 0x108u + step, s->slot}; return 0;
}
'''

TESTS = r'''
int main(void) {
 kp_rgb_global.brightness = 61; kp_rgb_global.user_on = true;
 kp_rgb_sync_refresh();
 assert(!sends && kp_rgb_sync_sources[0].ready_at == 100);
 assert(scheduled_work == &kp_rgb_sync_clock && scheduled_delay == 100);
 kp_rgb_sync_step_handler(NULL); assert(!sends);
 now_ms = 100; fail_source[0] = true;
 kp_rgb_sync_step_handler(NULL);
 assert(sends == 2 && destinations[0] == 0 && destinations[1] == 1);
 assert(bindings[0].param1 == RGB_BRT_SET_CMD && bindings[0].param2 == 61);
 assert(!strcmp(bindings[0].behavior_dev,"global"));
 assert(kp_rgb_sync_sources[0].phase == KP_RGB_SYNC_BRIGHTNESS);
 assert(kp_rgb_sync_sources[0].ready_at == 150);
 kp_rgb_global.brightness = 9; kp_rgb_global.user_on = false;
 now_ms = 150; kp_rgb_sync_step_handler(NULL);
 assert(bindings[2].param2 == 61 && kp_rgb_sync_sources[0].ready_at == 250);
 unsigned before = sends;
 kp_rgb_sync_clock_handler(NULL);
 assert(sends == before + 2 && bindings[before].param1 == RGB_CLOCK_CMD);
 assert(bindings[before].param2 == 150 && scheduled_delay == 30000);
 now_ms = 250; fail_source[0] = false; kp_rgb_sync_step_handler(NULL);
 assert(bindings[sends-1].param1 == RGB_BRT_SET_CMD && bindings[sends-1].param2 == 61);
 now_ms += 50; kp_rgb_sync_step_handler(NULL);
 assert(bindings[sends-1].param1 == RGB_ON_CMD);
 now_ms += 50; kp_rgb_sync_step_handler(NULL);
 assert(bindings[sends-1].param2 == RGB_OVL_STATE_VAL(0,0x1234));
 now_ms += 50; fail_source[0] = true; kp_rgb_sync_step_handler(NULL);
 assert(kp_rgb_sync_sources[0].mask_word == 1);
 now_ms += 50; fail_source[0] = false; kp_rgb_sync_step_handler(NULL);
 assert(bindings[sends-1].param2 == RGB_OVL_STATE_VAL(1,0xabcd));
 for (unsigned n=0;n<8;n++) { now_ms += 50; kp_rgb_sync_step_handler(NULL); }
 assert(!kp_rgb_sync_sources[0].pending && !kp_rgb_sync_sources[1].pending);
 before = sends; kp_rgb_global.brightness = 77; kp_rgb_sync_refresh();
 assert(!kp_rgb_sync_sources[0].pending && sends == before);
 available_count = 1; ids[0] = 1; kp_rgb_sync_refresh();
 assert(!kp_rgb_sync_sources[0].seen);
 ids[0] = 0; kp_rgb_sync_refresh();
 assert(kp_rgb_sync_sources[0].pending && !kp_rgb_sync_sources[0].global_captured);
 now_ms += 100; kp_rgb_sync_step_handler(NULL);
 assert(bindings[sends-1].param2 == 77);
 selected_transport = &transports[1]; kp_rgb_sync_refresh();
 assert(!kp_rgb_sync_sources[0].global_captured && kp_rgb_sync_sources[0].ready_at == now_ms + 100);
 endpoint.ready = false; before = sends;
 now_ms += 100; kp_rgb_sync_step_handler(NULL); assert(sends == before);
 endpoint.ready = true; fail_source[0] = true;
 for (unsigned n=0;n<8;n++) {
  now_ms = kp_rgb_sync_sources[0].ready_at; kp_rgb_sync_step_handler(NULL);
 }
 assert(kp_rgb_sync_sources[0].ready_at - now_ms == 1000);
 assert(kp_rgb_sync_sources[0].phase == KP_RGB_SYNC_BRIGHTNESS);
 puts("global split reconnect, frozen state, retry isolation and clock passed");
 return 0;
}
'''


def fixture(selectors=True):
    source = (ROOT / "src/rgb_split_sync.c").read_text()
    for obsolete in ("RGB_LEGACY_", "synced_effect", "effect_index", "kp_rgb_controller",
                     "KP_RGB_SYNC_SELECT,", "KP_RGB_SYNC_COLOR", "KP_RGB_SYNC_DURATION"):
        assert obsolete not in source, obsolete
    assert "DEVICE_DT_GET(DT_INST(0, keypaw_behavior_rgb_matrix))" in source
    assert 'BUILD_ASSERT(DT_HAS_COMPAT_STATUS_OKAY(keypaw_behavior_rgb_matrix)' in source
    enums = "\n".join(re.search(r"enum " + name + r" \{.*?\};", source, re.S).group()
                      for name in ("kp_rgb_sync_phase", "kp_rgb_sync_emit"))
    header = (ROOT / "src/rgb_selector_sync_internal.h").read_text()
    code = PRELUDE + enums + structure(header, "kp_rgb_selector_snapshot")
    for name in ("kp_rgb_global_snapshot", "kp_rgb_sync_selector", "kp_rgb_sync_selector_progress", "kp_rgb_sync_source"):
        code += structure(source, name)
    code += r'''
static struct kp_rgb_sync_selector kp_rgb_sync_selectors[] = {{.owner=&owners[0]}, {.owner=&owners[1]}, {0}};
static struct kp_rgb_sync_source kp_rgb_sync_sources[KP_RGB_SYNC_SOURCES];
static const struct zmk_split_transport_central *kp_rgb_sync_transport;
'''
    if not selectors:
        code = code.replace('{{.owner=&owners[0]}, {.owner=&owners[1]}, {0}}', '{{0}}')
    code += SELECTOR_MOCKS
    for name in ("kp_rgb_global_snapshot_locked", "kp_rgb_sync_selector_count",
                 "kp_rgb_split_selector_dirty_locked", "kp_rgb_split_selectors_dirty_locked",
                 "kp_rgb_sync_selectors_reset", "kp_rgb_sync_selectors_refresh", "kp_rgb_sync_refresh",
                 "kp_rgb_sync_send_binding", "kp_rgb_split_send_selector", "kp_rgb_sync_send",
                 "kp_rgb_sync_send_clock", "kp_rgb_sync_clock_handler", "kp_rgb_sync_emit_global",
                 "kp_rgb_sync_step_handler", "kp_rgb_sync_selector_step_handler"):
        code += function(source, name) + "\n"
    return code


def run(tests, selectors=True):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "split.c"
        path.write_text(fixture(selectors) + tests)
        binary = Path(directory) / "split"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
            str(path), "-o", str(binary)], check=True, timeout=20)
        subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    run(TESTS)
    run(TESTS, selectors=False)
