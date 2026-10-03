#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Compile the complete production coordinator with deterministic host boundary mocks."""
import os
import pathlib
import re
import shlex
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
UNIT = ROOT / "tests/unit"
CASES = ("startup", "dag", "deadline", "deadline-reset", "same-value", "isr", "cycle", "missing",
         "undeclared", "invalid-deadline", "scope", "unused-scope", "self-feedback",
         "multi-feedback", "unready", "metadata", "short-circuit")


def run_tests():
    source = (ROOT / "src/rgb_conditions.c").read_text()
    # No output state exists in this fixture: a new coupling must fail compilation.
    source += "\n" + (ROOT / "src/rgb_control_start.c").read_text()
    source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
    public = (ROOT / "include/zmk/rgb_matrix.h").read_text()
    api = public[public.index("enum kp_rgb_condition_scope {"):public.index("#include <zmk/rgb_matrix_condition_internal.h>")]
    private = (ROOT / "include/zmk/rgb_matrix_condition_internal.h").read_text()
    private = re.sub(r'^#(?:include|pragma)[^\n]*\n', '', private, flags=re.M)
    fixture = (UNIT / "conditions_harness.c").read_text()
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory)
        c = path / "conditions.c"
        c.write_text(fixture.replace("/* PRODUCTION_API */", api + private)
                     .replace("/* PRODUCTION_COORDINATOR */", source))
        for central in (0, 1):
            for auto, settings in ((0, 0), (1, 0), (1, 1)):
                binary = path / "conditions"
                subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
                    "-std=c11", "-Wall", "-Wextra", "-Werror",
                    f"-DCONFIG_ZMK_SPLIT_ROLE_CENTRAL={central}",
                    f"-DCONFIG_KEYPAW_RGB_CONTROL_AUTO_START={auto}",
                    f"-DCONFIG_SETTINGS={settings}", str(c), "-o", str(binary)], check=True)
                for case in CASES:
                    subprocess.run([str(binary), case], check=True, timeout=10)


if __name__ == "__main__":
    run_tests()
