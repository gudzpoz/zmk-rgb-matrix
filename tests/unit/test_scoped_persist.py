#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Host tests for the private positional RGB persistence codec."""
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
#include "rgb_persist_internal.h"

static void test_zero_and_many_roundtrip(void) {
    struct kp_rgb_persist_state state = {.user_on = true, .brightness = 73,
                                         .selection_count = 0};
    uint8_t bytes[KP_RGB_PERSIST_HEADER_SIZE] = {0};
    const uint8_t expected[] = {1, 1, 73, 0, 0, 0};
    assert(kp_rgb_persist_encode(&state, NULL, bytes, sizeof(bytes)) == 0);
    assert(memcmp(bytes, expected, sizeof(expected)) == 0);
    struct kp_rgb_persist_state decoded = {false, 0, 99};
    assert(kp_rgb_persist_decode(bytes, sizeof(bytes), sizeof(bytes), &decoded,
                                 NULL, 0) == 0);
    assert(decoded.user_on && decoded.brightness == 73 && decoded.selection_count == 0);

    const uint16_t selections[] = {0, 1, 0x1234, UINT16_MAX};
    state = (struct kp_rgb_persist_state){.user_on = false, .brightness = 0,
                                          .selection_count = 4};
    uint8_t record[KP_RGB_PERSIST_SIZE(4)] = {0};
    const uint8_t expected_many[] = {1, 0, 0, 0, 4, 0, 0, 0, 1, 0,
                                     0x34, 0x12, 0xff, 0xff};
    assert(kp_rgb_persist_encode(&state, selections, record, sizeof(record)) == 0);
    assert(sizeof(record) == sizeof(expected_many));
    assert(memcmp(record, expected_many, sizeof(record)) == 0);
    uint16_t restored[4] = {0};
    assert(kp_rgb_persist_decode(record, sizeof(record), sizeof(record), &decoded,
                                 restored, 4) == 0);
    assert(!decoded.user_on && decoded.brightness == 0 && decoded.selection_count == 4);
    assert(memcmp(restored, selections, sizeof(selections)) == 0);
}

static void test_prefix_and_capacity(void) {
    const uint8_t record[] = {1, 1, 100, 0, 3, 0, 0x11, 0x11, 0x22, 0x22, 0x33, 0x33};
    struct kp_rgb_persist_state state = {0};
    uint16_t slots[4] = {0xaaaa, 0xbbbb, 0xcccc, 0xdddd};
    assert(kp_rgb_persist_decode(record, 10, sizeof(record), &state, slots, 1) == 0);
    assert(state.selection_count == 3 && slots[0] == 0x1111 && slots[1] == 0xbbbb);
    assert(kp_rgb_persist_decode(record, 6, sizeof(record), &state, NULL, 0) == 0);
    assert(state.selection_count == 3);
    assert(kp_rgb_persist_decode(record, sizeof(record), sizeof(record), &state, slots, 4) == 0);
    assert(slots[2] == 0x3333 && slots[3] == 0xdddd);
}

#define REJECT_UNCHANGED(buf, prefix_len_, record_len_, state_, slots_, capacity_) do { \
    struct kp_rgb_persist_state state_before = (state_); \
    uint16_t slots_before[4]; memcpy(slots_before, (slots_), sizeof(slots_before)); \
    assert(kp_rgb_persist_decode((buf), (prefix_len_), (record_len_), &(state_), \
                                 (slots_), (capacity_)) == -EINVAL); \
    assert(memcmp(&(state_), &state_before, sizeof(state_before)) == 0); \
    assert(memcmp((slots_), slots_before, sizeof(slots_before)) == 0); \
} while (0)

static void test_invalid_framing_and_arguments(void) {
    const uint8_t valid[] = {1, 1, 55, 0, 1, 0, 0x34, 0x12};
    struct kp_rgb_persist_state state = {true, 44, 7};
    uint16_t slots[4] = {5, 6, 7, 8};
    uint8_t bad[sizeof(valid)];

    memcpy(bad, valid, sizeof(valid)); bad[0] = 2;
    REJECT_UNCHANGED(bad, sizeof(bad), sizeof(bad), state, slots, 4);
    memcpy(bad, valid, sizeof(valid)); bad[1] = 2;
    REJECT_UNCHANGED(bad, sizeof(bad), sizeof(bad), state, slots, 4);
    memcpy(bad, valid, sizeof(valid)); bad[2] = 101;
    REJECT_UNCHANGED(bad, sizeof(bad), sizeof(bad), state, slots, 4);
    memcpy(bad, valid, sizeof(valid)); bad[3] = 1;
    REJECT_UNCHANGED(bad, sizeof(bad), sizeof(bad), state, slots, 4);
    REJECT_UNCHANGED(valid, sizeof(valid), sizeof(valid) - 1, state, slots, 4);
    REJECT_UNCHANGED(valid, sizeof(valid), sizeof(valid) + 1, state, slots, 4);
    memcpy(bad, valid, sizeof(valid)); bad[4] = 2;
    REJECT_UNCHANGED(bad, sizeof(bad), sizeof(bad), state, slots, 4);
    REJECT_UNCHANGED(valid, 5, sizeof(valid), state, slots, 1);
    REJECT_UNCHANGED(valid, 7, sizeof(valid), state, slots, 1);
    REJECT_UNCHANGED(NULL, sizeof(valid), sizeof(valid), state, slots, 4);
    assert(kp_rgb_persist_decode(valid, sizeof(valid), sizeof(valid), NULL, slots, 4) == -EINVAL);
    assert(kp_rgb_persist_decode(valid, sizeof(valid), sizeof(valid), &state, NULL, 1) == -EINVAL);
    assert(kp_rgb_persist_decode(valid, 0, sizeof(valid), &state, slots, 4) == -EINVAL);

    uint8_t output[sizeof(valid)]; memset(output, 0xa5, sizeof(output));
    uint8_t before[sizeof(output)]; memcpy(before, output, sizeof(output));
    struct kp_rgb_persist_state encode_state = {true, 50, 1};
    assert(kp_rgb_persist_encode(&encode_state, NULL, output, sizeof(output)) == -EINVAL);
    assert(memcmp(output, before, sizeof(output)) == 0);
    assert(kp_rgb_persist_encode(&encode_state, (uint16_t[]){1}, output,
                                 sizeof(output) - 1) == -EINVAL);
    assert(memcmp(output, before, sizeof(output)) == 0);
    assert(kp_rgb_persist_encode(&encode_state, (uint16_t[]){1}, output,
                                 sizeof(output) + 1) == -EINVAL);
    assert(memcmp(output, before, sizeof(output)) == 0);
    encode_state.brightness = 101;
    assert(kp_rgb_persist_encode(&encode_state, (uint16_t[]){1}, output,
                                 sizeof(output)) == -EINVAL);
    assert(memcmp(output, before, sizeof(output)) == 0);
    assert(kp_rgb_persist_encode(&encode_state, (uint16_t[]){1}, NULL,
                                 sizeof(output)) == -EINVAL);
    assert(kp_rgb_persist_encode(NULL, (uint16_t[]){1}, output,
                                 sizeof(output)) == -EINVAL);
}

static void test_maximum_count_framing(void) {
    const uint8_t header[] = {1, 0, 0, 0, 0xff, 0xff};
    struct kp_rgb_persist_state state = {true, 20, 3};
    size_t full_len = KP_RGB_PERSIST_SIZE(UINT16_MAX);
    assert(kp_rgb_persist_decode(header, sizeof(header), full_len, &state, NULL, 0) == 0);
    assert(state.selection_count == UINT16_MAX && state.brightness == 0 && !state.user_on);
    struct kp_rgb_persist_state max_before = state;
    assert(kp_rgb_persist_decode(header, sizeof(header), full_len - 1, &state, NULL, 0) == -EINVAL);
    assert(memcmp(&state, &max_before, sizeof(state)) == 0);
    assert(kp_rgb_persist_decode(header, sizeof(header), SIZE_MAX, &state, NULL, 0) == -EINVAL);
    assert(memcmp(&state, &max_before, sizeof(state)) == 0);
}

static void test_upper_byte_count(void) {
    const uint8_t expected_header[] = {1, 1, 77, 0, 0, 1};
    struct kp_rgb_persist_state state = {.user_on = true, .brightness = 77,
                                         .selection_count = 256};
    uint16_t selections[256] = {0};
    uint8_t record[6 + 2 * 256] = {0};
    assert(kp_rgb_persist_encode(&state, selections, record, sizeof(record)) == 0);
    assert(memcmp(record, expected_header, sizeof(expected_header)) == 0);

    struct kp_rgb_persist_state decoded = {0};
    assert(kp_rgb_persist_decode(expected_header, sizeof(expected_header),
                                 sizeof(record), &decoded, NULL, 0) == 0);
    assert(decoded.user_on && decoded.brightness == 77 &&
           decoded.selection_count == 256);
}

int main(void) {
    test_zero_and_many_roundtrip();
    test_prefix_and_capacity();
    test_invalid_framing_and_arguments();
    test_upper_byte_count();
    test_maximum_count_framing();
    puts("positional persistence codec passed");
    return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        harness = tmp / "scoped_persist.c"
        harness.write_text('#include "' + str(ROOT / 'src/rgb_persist.c') + '"\n' + TEST)
        binary = tmp / "scoped_persist"
        run_logged(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "src"),
            str(harness), "-o", str(binary)], check=True,
            log_path=LOGS / "compile.log", text=True, stderr=subprocess.STDOUT)
        result = run_logged([str(binary)], check=True, timeout=HOST_TIMEOUT,
                            log_path=LOGS / "runtime.log", text=True,
                            stderr=subprocess.STDOUT)
        print(result.stdout, end="")


if __name__ == "__main__":
    main()
