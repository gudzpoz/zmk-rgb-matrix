#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Pristine native geometry regressions with retained configs, builds and logs."""
import argparse
from pathlib import Path
import os
import shlex
import shutil
import subprocess

from native_build import MODULE, SIM, WEST, WORKSPACE, native_build_root
from native_process import run_logged

FIXTURE = SIM / "config-geometry"
POSITIVE = {
    "normal": 1,
    "different": 2,
    "synthetic": 3,
    "signed": 4,
    "coincident": 5,
    "zero-keys": 7,
    "disabled": 9,
    "single": 10,
}
NEGATIVE = {
    "wrong-compat": "RGB physical-layout must reference zmk,physical-layout",
    "missing-keys": "RGB physical-layout must provide keys",
    "index-equal-count": "invalid mapping entry",
}


def run_case(root, name):
    directory = root / name
    config = directory / "config"
    config.mkdir(parents=True, exist_ok=True)
    for filename in ("native_sim.conf", "native_sim.keymap"):
        shutil.copyfile(FIXTURE / filename, config / filename)
    overlay = f'#include "{FIXTURE / "native_sim.overlay"}"\n'
    if name != "normal":
        overlay += f'#include "{FIXTURE / (name + ".overlay")}"\n'
    (config / "native_sim.overlay").write_text(overlay)
    command = shlex.split(WEST) + [
        "build", "-s", "app", "-d", str(directory / "build"),
        "-b", "native_sim//zmk_test_mock", "-p", "--",
        f"-DZMK_CONFIG={config}",
        f"-DZMK_EXTRA_MODULES={MODULE};{SIM / 'module'}",
        f"-DEXTRA_CFLAGS=-DKP_GEOMETRY_CASE={POSITIVE.get(name, 1)}",
    ]
    print("COMMAND", shlex.join(command), flush=True)
    # A timeout raises; it must never count as an expected build rejection.
    result = run_logged(
        command, cwd=WORKSPACE,
        env={**os.environ, "ZEPHYR_TOOLCHAIN_VARIANT": "host"},
        text=True, stderr=subprocess.STDOUT, timeout=600,
        log_path=directory / "build.log",
    )
    if name in NEGATIVE:
        if result.returncode == 0 or NEGATIVE[name] not in result.stdout:
            raise AssertionError(f"Expected rejection {name}: {directory / 'build.log'}\n"
                                 + result.stdout[-16000:])
        print(f"PASS geometry rejection {name}: {NEGATIVE[name]}", flush=True)
        return
    if result.returncode:
        raise AssertionError(f"Build failed: {directory / 'build.log'}\n"
                             + result.stdout[-16000:])
    result = run_logged(
        [str(directory / "build/zephyr/zmk.exe"), "-no-rt"],
        text=True, stderr=subprocess.STDOUT, timeout=30,
        log_path=directory / "runtime.log",
    )
    marker = f"PASS geometry case={POSITIVE[name]} LEDs="
    precondition = f"geometry runtime provider precondition passed case={POSITIVE[name]}"
    if result.returncode or marker not in result.stdout or precondition not in result.stdout:
        raise AssertionError(f"Runtime failed: {directory / 'runtime.log'}\n" + result.stdout)
    print(f"PASS geometry {name}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", action="append", choices=[*POSITIVE, *NEGATIVE],
                        help="Run only this case; repeat to select multiple cases")
    parser.add_argument("--build-dir", type=Path,
                        help="Retained artifact root (default: unique native-artifacts directory)")
    args = parser.parse_args()
    root = native_build_root(args.build_dir, "geometry", prefix="geometry-",
                             parent=MODULE / "build/native-artifacts")
    print(f"Geometry artifacts: {root}", flush=True)
    cases = list(dict.fromkeys(args.case or [*POSITIVE, *NEGATIVE]))
    for name in cases:
        run_case(root, name)
    print(f"PASS geometry suite: {len(cases)} cases; artifacts: {root}", flush=True)


if __name__ == "__main__":
    main()
