#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Host tests for the current scoped RGB persistence codecs."""
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
}

#define REJECT_UNCHANGED(fn, bytes, length, output) do { \
    __typeof__(output) before = (output); \
    assert((fn)((bytes), (length), &(output)) == -EINVAL); \
    assert(memcmp(&(output), &before, sizeof(output)) == 0); \
} while (0)

static void test_selector_codec_accepts_bounded_bytes(void) {
    const char punctuation[] = "punctuation/+ =@.-_\\";
    const char nonascii[] = {(char)0xc3, (char)0xa9, 0};
    const char maximum[32] = "1234567890123456789012345678901";
    const char *accepted[] = {punctuation, nonascii, maximum};
    for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); i++) {
        struct kp_rgb_persist_selector_record encoded = {0}, decoded = {0};
        assert(kp_rgb_persist_encode_selector(accepted[i], &encoded) == 0);
        assert(kp_rgb_persist_decode_selector(&encoded, sizeof(encoded), &decoded) == 0);
        assert(strcmp(decoded.selected_id, accepted[i]) == 0);
    }
}

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
    bads[0] = 1; bads[1] = 0; REJECT_UNCHANGED(kp_rgb_persist_decode_selector, bads, 33, s);
    memset(bads + 1, 'a', 32); REJECT_UNCHANGED(kp_rgb_persist_decode_selector, bads, 33, s);
    REJECT_UNCHANGED(kp_rgb_persist_decode_selector, NULL, 33, s);
    assert(kp_rgb_persist_encode_selector("ok", NULL) == -EINVAL);
    assert(kp_rgb_persist_decode_selector(bads, sizeof(bads), NULL) == -EINVAL);

    struct kp_rgb_persist_selector_record encoded = {.version = 55};
    strcpy(encoded.selected_id, "keep");
    struct kp_rgb_persist_selector_record before = encoded;
    assert(kp_rgb_persist_encode_selector(NULL, &encoded) == -EINVAL);
    assert(memcmp(&encoded, &before, sizeof(encoded)) == 0);
    assert(kp_rgb_persist_encode_selector("", &encoded) == -EINVAL);
    assert(memcmp(&encoded, &before, sizeof(encoded)) == 0);
    char too_long[33]; memset(too_long, 'x', 32); too_long[32] = 0;
    assert(kp_rgb_persist_encode_selector(too_long, &encoded) == -EINVAL);
    assert(memcmp(&encoded, &before, sizeof(encoded)) == 0);
    char unterminated[32]; memset(unterminated, 'x', sizeof(unterminated));
    assert(kp_rgb_persist_encode_selector(unterminated, &encoded) == -EINVAL);
    assert(memcmp(&encoded, &before, sizeof(encoded)) == 0);

    /* The former combined legacy blob is not a valid v3 global record. */
    uint8_t raw_legacy_global[36] = {2, 1, 50, 1};
    REJECT_UNCHANGED(kp_rgb_persist_decode_global, raw_legacy_global, sizeof(raw_legacy_global), g);
    /* Unsupported selector versions are rejected. */
    uint8_t raw_legacy_selector[33] = {2};
    REJECT_UNCHANGED(kp_rgb_persist_decode_selector, raw_legacy_selector, sizeof(raw_legacy_selector), s);
}

int main(void) {
    test_wire_codecs();
    test_selector_codec_accepts_bounded_bytes();
    test_invalid_records_and_legacy();
    puts("scoped persistence codecs passed");
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
