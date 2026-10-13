#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Host tests for the current scoped RGB persistence codecs and ID rules."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]

sys.path.insert(0, str(ROOT / "tests/sim"))
from native_process import HOST_TIMEOUT, run_logged
LOGS = ROOT / "tests/build/process/scoped-persist"
TEST = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zmk/rgb_persist.h>

static void test_wire_codecs(void) {
    struct kp_rgb_persist_global_state global = {.user_on = true, .brightness = 73};
    struct kp_rgb_persist_global_record gr = {0};
    assert(kp_rgb_persist_encode_global(&global, &gr) == 0);
    const uint8_t global_bytes[] = {3, 1, 73, 0};
    assert(sizeof(gr) == sizeof(global_bytes) && memcmp(&gr, global_bytes, sizeof(gr)) == 0);
    struct kp_rgb_persist_global_state gd = {false, 12};
    assert(kp_rgb_persist_decode_global(global_bytes, sizeof(global_bytes), &gd) == 0);
    assert(gd.user_on && gd.brightness == 73);

    struct kp_rgb_persist_selector_record sr = {0};
    assert(kp_rgb_persist_encode_selector("solid", &sr) == 0);
    assert(sr.version == 1 && !strcmp(sr.selected_id, "solid"));
    assert(sizeof(sr) == 33 && ((const uint8_t *)&sr)[0] == 1);
    assert(!strcmp((const char *)&sr + 1, "solid"));
    struct kp_rgb_persist_selector_record sd = {0};
    assert(kp_rgb_persist_decode_selector(&sr, sizeof(sr), &sd) == 0);
    assert(!strcmp(sd.selected_id, "solid"));

    struct kp_rgb_persist_effect effect = {.duration_ms = 0x1234, .h = 0x0123, .s = 67};
    struct kp_rgb_persist_parameters_record pr = {0};
    assert(kp_rgb_persist_encode_parameters(&effect, &pr) == 0);
    const uint8_t parameter_bytes[] = {1, 0x34, 0x12, 0x23, 0x01, 67};
    assert(sizeof(pr) == sizeof(parameter_bytes) && memcmp(&pr, parameter_bytes, sizeof(pr)) == 0);
    struct kp_rgb_persist_effect pd = {0};
    assert(kp_rgb_persist_decode_parameters(parameter_bytes, sizeof(parameter_bytes), &pd) == 0);
    assert(pd.duration_ms == 0x1234 && pd.h == 0x0123 && pd.s == 67);

    effect.h = 360;
    assert(kp_rgb_persist_encode_parameters(&effect, &pr) == 0);
    assert(pr.hue_le[0] == 0 && pr.hue_le[1] == 0);
    const uint8_t hue360[] = {1, 9, 0, 104, 1, 22};
    assert(kp_rgb_persist_decode_parameters(hue360, sizeof(hue360), &pd) == 0 && pd.h == 0);
}

#define REJECT_UNCHANGED(fn, bytes, length, output) do { \
    __typeof__(output) before = (output); \
    assert((fn)((bytes), (length), &(output)) == -EINVAL); \
    assert(memcmp(&(output), &before, sizeof(output)) == 0); \
} while (0)

static void test_invalid_records_and_legacy(void) {
    struct kp_rgb_persist_global_state g = {true, 55};
    uint8_t badg[] = {3, 1, 20, 0};
    REJECT_UNCHANGED(kp_rgb_persist_decode_global, badg, 3, g);
    badg[0] = 99; REJECT_UNCHANGED(kp_rgb_persist_decode_global, badg, 4, g);
    badg[0] = 3; badg[1] = 2; REJECT_UNCHANGED(kp_rgb_persist_decode_global, badg, 4, g);
    badg[1] = 1; badg[2] = 101; REJECT_UNCHANGED(kp_rgb_persist_decode_global, badg, 4, g);
    badg[2] = 20; badg[3] = 1; REJECT_UNCHANGED(kp_rgb_persist_decode_global, badg, 4, g);
    REJECT_UNCHANGED(kp_rgb_persist_decode_global, NULL, 4, g);

    uint8_t bads[33] = {1}; strcpy((char *)bads + 1, "ok");
    struct kp_rgb_persist_selector_record s = {.version = 77}; strcpy(s.selected_id, "keep");
    REJECT_UNCHANGED(kp_rgb_persist_decode_selector, bads, 32, s);
    bads[0] = 9; REJECT_UNCHANGED(kp_rgb_persist_decode_selector, bads, 33, s);
    bads[0] = 1; strcpy((char *)bads + 1, "bad/id"); REJECT_UNCHANGED(kp_rgb_persist_decode_selector, bads, 33, s);
    memset(bads + 1, 'a', 32); REJECT_UNCHANGED(kp_rgb_persist_decode_selector, bads, 33, s);
    REJECT_UNCHANGED(kp_rgb_persist_decode_selector, NULL, 33, s);

    struct kp_rgb_persist_effect p = {.duration_ms = 123, .h = 45, .s = 67};
    uint8_t badp[] = {0, 1, 0, 2, 0, 3};
    REJECT_UNCHANGED(kp_rgb_persist_decode_parameters, badp, 6, p);
    badp[0] = 1; badp[3] = 105; badp[4] = 1;
    REJECT_UNCHANGED(kp_rgb_persist_decode_parameters, badp, 6, p);
    badp[3] = 2; badp[4] = 0; badp[5] = 101;
    REJECT_UNCHANGED(kp_rgb_persist_decode_parameters, badp, 6, p);
    REJECT_UNCHANGED(kp_rgb_persist_decode_parameters, badp, 5, p);
    REJECT_UNCHANGED(kp_rgb_persist_decode_parameters, NULL, 6, p);

    /* The former combined legacy blob is not a valid v3 global record. */
    uint8_t raw_legacy_global[36] = {2, 1, 50, 1};
    REJECT_UNCHANGED(kp_rgb_persist_decode_global, raw_legacy_global, sizeof(raw_legacy_global), g);
    /* Unsupported selector versions and non-v1 parameter bytes are rejected. */
    uint8_t raw_legacy_selector[33] = {2};
    REJECT_UNCHANGED(kp_rgb_persist_decode_selector, raw_legacy_selector, sizeof(raw_legacy_selector), s);
    uint8_t raw_legacy_parameters[] = {0x90, 0x01, 0x68, 0x01, 80, 0xa5};
    REJECT_UNCHANGED(kp_rgb_persist_decode_parameters, raw_legacy_parameters,
                     sizeof(raw_legacy_parameters), p);
}

static void test_ids(void) {
    char id31[32]; memset(id31, 'x', 31); id31[31] = 0;
    char id32[33]; memset(id32, 'x', 32); id32[32] = 0;
    assert(kp_rgb_persist_id_valid(id31) && !kp_rgb_persist_id_valid(id32));
    assert(!kp_rgb_persist_id_valid(NULL) && !kp_rgb_persist_id_valid(""));
    assert(!kp_rgb_persist_id_valid("a/b") && !kp_rgb_persist_id_valid("a="));
    assert(!kp_rgb_persist_id_valid("a b") && !kp_rgb_persist_id_valid("a\t"));
    const char nonascii[] = {(char)0xc3, (char)0xa9, 0};
    assert(!kp_rgb_persist_id_valid(nonascii));

    const struct kp_rgb_persist_effect_id a[] = {{"same", true}, {NULL, true}, {"other", false}};
    const struct kp_rgb_persist_effect_id b[] = {{"same", false}};
    const struct kp_rgb_persist_selector_ids selectors[] = {{"selA", a, 3}, {"selB", b, 1}};
    assert(kp_rgb_persist_validate_ids(selectors, 2) == 0);
    const struct kp_rgb_persist_selector_ids duplicate_selector[] = {{"x", NULL, 0}, {"x", NULL, 0}};
    assert(kp_rgb_persist_validate_ids(duplicate_selector, 2) == -EEXIST);
    const struct kp_rgb_persist_effect_id duplicate_members[] = {{"x", false}, {"x", false}};
    const struct kp_rgb_persist_selector_ids duplicate_member_selector[] = {{"sel", duplicate_members, 2}};
    assert(kp_rgb_persist_validate_ids(duplicate_member_selector, 1) == -EEXIST);
    const struct kp_rgb_persist_effect_id invalid_enabled[] = {{"bad/id", false}};
    const struct kp_rgb_persist_selector_ids invalid_selector[] = {{"sel", invalid_enabled, 1}};
    assert(kp_rgb_persist_validate_ids(invalid_selector, 1) == -EINVAL);
    assert(kp_rgb_persist_validate_ids(NULL, 1) == -EINVAL);

    char selector[32], effect[32]; memset(selector, 's', 13); selector[13] = 0;
    memset(effect, 'e', 14); effect[14] = 0;
    const struct kp_rgb_persist_effect_id budget_ok[] = {{effect, true}};
    const struct kp_rgb_persist_selector_ids budget_selector[] = {{selector, budget_ok, 1}};
    assert(KP_RGB_PERSIST_EFFECT_ID_BUDGET == 27);
    assert(kp_rgb_persist_validate_ids(budget_selector, 1) == 0);
    selector[13] = 's'; selector[14] = 0;
    assert(kp_rgb_persist_validate_ids(budget_selector, 1) == -ENAMETOOLONG);
    char id30[31]; memset(id30, 's', 30); id30[30] = 0;
    const struct kp_rgb_persist_effect_id optout[] = {{id31, false}};
    const struct kp_rgb_persist_selector_ids long_optout[] = {{id30, optout, 1}};
    assert(kp_rgb_persist_validate_ids(long_optout, 1) == 0);
    const struct kp_rgb_persist_selector_ids oversized_selector[] = {{id31, optout, 1}};
    assert(kp_rgb_persist_validate_ids(oversized_selector, 1) == -ENAMETOOLONG);
}

int main(void) {
    test_wire_codecs();
    test_invalid_records_and_legacy();
    test_ids();
    puts("scoped persistence codecs and IDs passed");
    return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        stub = tmp / "stub/zephyr"
        (stub / "settings").mkdir(parents=True)
        (stub / "sys").mkdir(parents=True)
        (stub / "drivers").mkdir(parents=True)
        (stub / "settings/settings.h").write_text(
            "#pragma once\n#define SETTINGS_MAX_NAME_LEN 64\n#define SETTINGS_MAX_VAL_LEN 256\n")
        (stub / "sys/util.h").write_text(
            "#pragma once\n#define BUILD_ASSERT(c, ...) _Static_assert(c, #c)\n"
            "#define MIN(a,b) ((a) < (b) ? (a) : (b))\n"
            "#define CLAMP(v,lo,hi) MIN(MAX(v,lo),hi)\n"
            "#define MAX(a,b) ((a) > (b) ? (a) : (b))\n")
        (stub / "drivers/led_strip.h").write_text(
            "#pragma once\n#include <stdint.h>\nstruct led_rgb { uint8_t r, g, b; };\n")
        harness = tmp / "scoped_persist.c"
        harness.write_text('#include "' + str(ROOT / 'src/rgb_persist.c') + '"\n' + TEST)
        binary = tmp / "scoped_persist"
        run_logged(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-I", str(tmp / "stub"), "-I", str(ROOT / "include"),
            str(harness), "-o", str(binary)], check=True,
            log_path=LOGS / "compile.log", text=True, stderr=subprocess.STDOUT)
        result = run_logged([str(binary)], check=True, timeout=HOST_TIMEOUT,
                            log_path=LOGS / "runtime.log", text=True,
                            stderr=subprocess.STDOUT)
        print(result.stdout, end="")


if __name__ == "__main__":
    main()
