#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Host-test production enum helpers; mock only Zephyr's macro/DT primitives."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

HEADER = Path(__file__).resolve().parents[2] / "include/zmk/rgb_matrix.h"

MOCKS = r'''
#include <assert.h>
#include <stdio.h>
#include <sys/types.h>

/* These fixtures use five-token concatenation and three enum values. */
#define TEST_CONCAT(a, b, c, d, e) a##b##c##d##e
#define CONCAT(a, b, c, d, e) TEST_CONCAT(a, b, c, d, e)
#define TEST_UNWRAP(...) __VA_ARGS__
#define FOR_EACH_IDX_FIXED_ARG(fn, sep, arg, a, b, c) \
    fn(0, a, arg) TEST_UNWRAP sep \
    fn(1, b, arg) TEST_UNWRAP sep fn(2, c, arg)
#define DT_DRV_INST(inst) inst
#define TEST_DT_TOKEN(inst, prop) test_dt_##inst##_##prop
#define DT_STRING_TOKEN(inst, prop) TEST_DT_TOKEN(inst, prop)
#define test_dt_0_mode pulse
#define test_dt_1_mode idle
#define test_dt_2_mode sweep
'''

TESTS = r'''
#define DT_DRV_COMPAT test_first
DEFINE_DT_ENUM(kp_test_first_mode_t, mode, idle, pulse, sweep);

_Static_assert(test_first_mode_idle == 0, "first enum starts at zero");
_Static_assert(test_first_mode_pulse == 1, "first enum preserves order");
_Static_assert(test_first_mode_sweep == 2, "first enum preserves order");
_Static_assert(DT_ENUM_CONST(mode, pulse) == test_first_mode_pulse,
               "constant lookup uses the compatible and property");
static const kp_test_first_mode_t first_modes[] = {
    CONV_DT_ENUM(0, mode), CONV_DT_ENUM(1, mode), CONV_DT_ENUM(2, mode),
};

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT test_second
/* A typedef without _t must also be emitted exactly as supplied. */
DEFINE_DT_ENUM(kp_test_second_mode, mode, sweep, idle, pulse);

_Static_assert(test_second_mode_sweep == 0, "second enum starts at zero");
_Static_assert(test_second_mode_idle == 1, "second enum preserves order");
_Static_assert(test_second_mode_pulse == 2, "second enum preserves order");
_Static_assert(DT_ENUM_CONST(mode, pulse) == test_second_mode_pulse,
               "constant lookup follows the current compatible");
_Static_assert(!__builtin_types_compatible_p(kp_test_first_mode_t,
                                            kp_test_second_mode),
               "providers sharing a property have independent enum types");
static const kp_test_second_mode second_modes[] = {
    CONV_DT_ENUM(0, mode), CONV_DT_ENUM(1, mode), CONV_DT_ENUM(2, mode),
};

int main(void) {
    /* sys/types.h's mode_t remains available alongside both provider types. */
    mode_t permissions = 0644;
    assert(permissions == 0644);
    assert(first_modes[0] == test_first_mode_pulse);
    assert(first_modes[1] == test_first_mode_idle);
    assert(first_modes[2] == test_first_mode_sweep);
    assert(second_modes[0] == test_second_mode_pulse);
    assert(second_modes[1] == test_second_mode_idle);
    assert(second_modes[2] == test_second_mode_sweep);
    puts("production-extracted DT enum tests passed");
    return 0;
}
'''


def production_macros():
    source = HEADER.read_text()
    definitions = []
    for name in ("KP_ENUM_ENTRY", "DEFINE_DT_ENUM", "CONV_DT_ENUM", "DT_ENUM_CONST"):
        match = re.search(
            rf"^#define {name}\((?:[^\n]*\\\n)*[^\n]*",
            source, re.MULTILINE,
        )
        if match is None:
            raise AssertionError(f"Missing production macro: {name}")
        definitions.append(match.group(0))
    return "\n".join(definitions) + "\n"


def main():
    compiler = shlex.split(os.environ.get("CC", "cc"))
    flags = shlex.split(os.environ.get("CFLAGS", ""))
    with tempfile.TemporaryDirectory(prefix="rgb-dt-enum-") as directory:
        directory = Path(directory)
        source = directory / "dt_enum.c"
        executable = directory / "dt_enum"
        source.write_text(MOCKS + production_macros() + TESTS)
        subprocess.run(compiler + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", *flags,
            str(source), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
