#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Host-test production trigger functions; only DT data and external APIs are mocked.

The coordinator serializes calls without locks, registers roots before sampling,
then evaluates only after source/default/settings readiness and cache sampling.
These tests do not implement or validate that coordinator, DT schema generation,
provider sampling, or ZMK conversion/transport. Dispatch is observed at the single
zmk_behavior_invoke_binding boundary, including its pressed flag and timestamp.
"""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

SOURCE = Path(__file__).resolve().parents[2] / "src/rgb_triggers.c"

MOCKS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define ARG_UNUSED(a) ((void)(a))
#define IS_ENABLED(a) (a)
#define DT_HAS_COMPAT_STATUS_OKAY(a) HAVE_TABLE
#define LOG_MODULE_DECLARE(...)
static void test_log(const char *format, ...) { (void)format; }
#define LOG_ERR(...) test_log(__VA_ARGS__)
static unsigned warnings;
static char last_warning[128];
static void test_warning(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(last_warning, sizeof(last_warning), format, args);
    va_end(args);
    warnings++;
}
#define LOG_WRN(...) test_warning(__VA_ARGS__)
struct zmk_behavior_binding { const char *behavior_dev; uint32_t param1, param2; };
struct zmk_behavior_binding_event { int64_t timestamp; };
struct device { int unused; };
static struct device conditions[4];
static bool sampled, values[4], valid[4], required_ok[4];
static unsigned requires[4], reads, validity_reads;
static bool kp_rgb_condition_require(const struct device *dev) {
    assert(!sampled && dev != NULL);
    size_t i = (size_t)(dev - conditions); assert(i < 4);
    requires[i]++; return required_ok[i];
}
static bool kp_rgb_condition_valid(const struct device *dev) {
    assert(sampled); validity_reads++;
    return dev == NULL || valid[dev - conditions];
}
static bool kp_rgb_condition_value(const struct device *dev) {
    assert(sampled && dev != NULL && valid[dev - conditions]);
    reads++; return values[dev - conditions];
}
static unsigned actions[128], action_count;
static int64_t expected_time;
static int dispatch_error;
static void (*action_hook)(unsigned);
static int zmk_behavior_invoke_binding(const struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event,
                                     bool pressed) {
    assert(sampled && pressed && event.timestamp == expected_time);
    assert(action_count < ARRAY_SIZE(actions));
    if (binding->param1 == 25 || binding->param1 == 26) {
        assert(!strcmp(binding->behavior_dev, "third-party"));
        assert(binding->param2 == 77);
    }
    actions[action_count++] = binding->param1;
    if (action_hook) action_hook(binding->param1);
    return dispatch_error;
}
'''

FIXTURE = r'''
#define ACTION(name, code) \
    { .binding = {name, code, 0} }
static const struct kp_trig_action enter0[] = { ACTION("rgb", 10) };
static const struct kp_trig_action exit0[] = { ACTION("rgb", 11) };
static const struct kp_trig_action enter1[] = { ACTION("fx", 20) };
static const struct kp_trig_action exit1[] = { ACTION("fx", 21) };
static const struct kp_trig_action enter2[] = { { .binding = {"third-party", 25, 77} } };
static const struct kp_trig_action exit2[] = { { .binding = {"third-party", 26, 77} } };
static const struct kp_trig_action constant[] = { ACTION("rgb", 30) };
static const struct kp_trig_action shared[] = { ACTION("rgb", 40) };
static const struct kp_trig_action shared_exit[] = { ACTION("rgb", 41) };
static const struct kp_trig_action exit_only[] = { ACTION("rgb", 50) };
static const struct kp_trig kp_triggers[] = {
    {"enter", &conditions[0], enter0, 1, exit0, 1, false},
    {"baseline", &conditions[1], enter1, 1, exit1, 1, true},
    {"third-party", &conditions[2], enter2, 1, exit2, 1, false},
    {"constant", NULL, constant, 1, NULL, 0, false},
    {"shared", &conditions[0], shared, 1, shared_exit, 1, false},
    {"exit-only", &conditions[3], NULL, 0, exit_only, 1, false},
};
'''

TESTS = r'''
static bool evaluate(void) {
    expected_time += 17;
    return kp_rgb_triggers_evaluate(expected_time);
}
#if HAVE_TABLE && (!CONFIG_ZMK_SPLIT || CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static void reset(void) {
    memset(kp_trigger_state, 0, sizeof(kp_trigger_state));
    kp_triggers_ready = false;
    memset(values, 0, sizeof(values));
    memset(requires, 0, sizeof(requires));
    for (size_t i = 0; i < 4; i++) valid[i] = required_ok[i] = true;
    sampled = false; reads = validity_reads = action_count = 0;
    expected_time = 1000; action_hook = NULL; dispatch_error = 0;
    warnings = 0; last_warning[0] = '\0';
}
static void initialize(void) {
    assert(!evaluate()); /* No premature source access or startup dispatch. */
    kp_rgb_triggers_init();
    assert(action_count == 0 && reads == 0 && validity_reads == 0);
    assert(requires[0] == 2 && requires[1] == 1 && requires[2] == 1 && requires[3] == 1);
    kp_rgb_triggers_init(); /* Idempotent; still no sampled cache is available. */
    assert(requires[0] == 2);
    sampled = true;
    kp_rgb_triggers_begin();
}
static void neutral_start(void) {
    reset(); initialize();
    assert(evaluate() && action_count == 1 && actions[0] == 30);
    assert(!evaluate());
    action_count = 0;
    kp_rgb_triggers_begin();
}
static void startup(void) {
    reset(); values[0] = values[1] = values[3] = true;
    initialize();
    assert(evaluate());
    assert(action_count == 3 && actions[0] == 10 && actions[1] == 30 && actions[2] == 40);
    assert(!kp_trigger_state[1].participant && !kp_trigger_state[5].participant);
    assert(!evaluate() && action_count == 3); /* Constant and active roots are one-shot. */
    values[0] = values[1] = values[3] = false;
    assert(evaluate());
    assert(action_count == 7 && actions[3] == 11 && actions[4] == 21 &&
           actions[5] == 41 && actions[6] == 50);
    values[1] = true;
    assert(evaluate() && actions[7] == 20); /* Baseline suppresses startup only. */

    neutral_start(); /* Initially false roots, including baseline, emit no exit. */
    values[2] = true;
    assert(evaluate() && action_count == 1 && actions[0] == 25);
    values[0] = true;
    assert(evaluate() && action_count == 3 && actions[1] == 10 && actions[2] == 40);
    values[2] = false;
    assert(evaluate() && action_count == 4 && actions[3] == 26);
    assert(!evaluate());
}
static void mutate_after_capture(unsigned code) {
    if (code != 10) return;
    /* Every baseline must be committed before the first action is called. */
    assert(kp_trigger_state[0].previous && kp_trigger_state[1].previous &&
           kp_trigger_state[2].previous && kp_trigger_state[4].previous);
    values[1] = values[2] = false;
}
static void capture_before_dispatch(void) {
    neutral_start();
    values[0] = values[1] = values[2] = true;
    action_hook = mutate_after_capture;
    assert(evaluate());
    assert(action_count == 4 && actions[0] == 10 && actions[1] == 20 &&
           actions[2] == 25 && actions[3] == 40);
    action_hook = NULL;
    assert(evaluate());
    assert(action_count == 6 && actions[4] == 21 && actions[5] == 26);
    assert(!evaluate());
}
static void faulty_roots(void) {
    neutral_start(); values[0] = true;
    assert(evaluate()); action_count = 0;
    valid[0] = false;
    assert(!evaluate() && action_count == 0); /* No synthetic falling edge. */
    valid[0] = true; values[0] = false;
    assert(!evaluate());
    values[0] = true;
    assert(!evaluate()); /* Fault disables both consumers until reboot. */
    values[2] = true;
    assert(evaluate() && action_count == 1 && actions[0] == 25);

    reset(); required_ok[2] = false; values[2] = true;
    initialize(); assert(evaluate());
    assert(action_count == 1 && actions[0] == 30);
    required_ok[2] = true; values[2] = false;
    assert(!evaluate()); /* Root registration rejection is permanent too. */
}
static void self_toggle(unsigned code) {
    if (code == 10 || code == 11) values[0] = !values[0];
}
static void quarantine(void) {
    neutral_start();
    values[2] = true; assert(evaluate()); /* Previous transaction participant. */
    action_count = 0; kp_rgb_triggers_begin();
    values[0] = true; action_hook = self_toggle;
    for (unsigned i = 0; i < 8; i++) assert(evaluate());
    assert(action_count == 16); /* Both independent consumers participated. */
    kp_rgb_triggers_quarantine(); action_hook = NULL;
    assert(kp_trigger_state[0].disabled && kp_trigger_state[4].disabled);
    assert(!kp_trigger_state[1].disabled && !kp_trigger_state[2].disabled &&
           !kp_trigger_state[3].disabled && !kp_trigger_state[5].disabled);
    action_count = 0; kp_rgb_triggers_begin();
    assert(!evaluate());
    values[2] = false;
    assert(evaluate() && action_count == 1 && actions[0] == 26);
    values[0] = !values[0]; assert(!evaluate());

    neutral_start(); values[0] = true; dispatch_error = -1;
    assert(evaluate()); /* Attempted dispatch still participates if it fails. */
    kp_rgb_triggers_quarantine();
    assert(kp_trigger_state[0].disabled && kp_trigger_state[4].disabled);
}
static void third_party_actions(void) {
    neutral_start();
    values[2] = true; dispatch_error = -5;
    assert(evaluate() && action_count == 1 && actions[0] == 25);
    assert(warnings == 1 && strstr(last_warning, "err -5"));
    assert(!kp_trigger_state[2].disabled && kp_trigger_state[2].participant);
    assert(!evaluate() && action_count == 1 && warnings == 1); /* No retry/release. */
    values[2] = false; dispatch_error = 1;
    assert(evaluate() && action_count == 2 && actions[1] == 26);
    assert(warnings == 1); /* Nonnegative behavior returns are not errors. */
    assert(!evaluate() && action_count == 2);

    const struct kp_trig_action actions_to_run[] = {
        ACTION("third-party-a", 60), ACTION("third-party-b", 61)
    };
    dispatch_error = -7;
    kp_triggers_run(&kp_triggers[2], actions_to_run, ARRAY_SIZE(actions_to_run), expected_time);
    assert(action_count == 4 && actions[2] == 60 && actions[3] == 61);
    assert(warnings == 3 && strstr(last_warning, "binding 1") &&
           strstr(last_warning, "err -7")); /* Continue after a failed binding. */
}
int main(void) {
    startup(); capture_before_dispatch(); faulty_roots(); quarantine(); third_party_actions();
    puts("production-extracted central/standalone trigger tests passed");
    return 0;
}
#else
int main(void) {
    /* Even unavailable/unsampled sources must remain untouched by stubs. */
    sampled = false;
    kp_rgb_triggers_init(); kp_rgb_triggers_begin();
    for (unsigned i = 0; i < 10; i++) assert(!evaluate());
    kp_rgb_triggers_quarantine(); kp_rgb_triggers_init();
    assert(!evaluate());
    assert(action_count == 0 && reads == 0 && validity_reads == 0);
    for (size_t i = 0; i < 4; i++) assert(requires[i] == 0);
    puts("production-extracted peripheral/no-table trigger stubs passed");
    return 0;
}
#endif
'''


def production_source():
    source = SOURCE.read_text()
    source = re.sub(r"^#include[^\n]*\n", "", source, flags=re.MULTILINE)
    start = source.index("#define KP_TRIG_ACTION(")
    end_marker = "DT_INST_FOREACH_STATUS_OKAY(KP_TRIG_TABLE)};"
    end = source.index(end_marker, start) + len(end_marker)
    # Keep production structs, platform guard, state, and every function verbatim.
    return source[:start] + FIXTURE + source[end:]


def main():
    compiler = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="rgb-triggers-") as directory:
        directory = Path(directory)
        source = directory / "triggers.c"
        source.write_text(MOCKS + production_source() + TESTS)
        for name, split, central, table in (
            ("central", 1, 1, 1),
            ("standalone", 0, 0, 1),
            ("peripheral", 1, 0, 1),
            ("no-table", 0, 0, 0),
        ):
            executable = directory / name
            subprocess.run(compiler + [
                "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-function", "-Wno-unused-variable",
                f"-DCONFIG_ZMK_SPLIT={split}",
                f"-DCONFIG_ZMK_SPLIT_ROLE_CENTRAL={central}",
                f"-DHAVE_TABLE={table}", str(source), "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
