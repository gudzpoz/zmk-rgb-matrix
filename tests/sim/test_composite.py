#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Run the real RGB engine against nested, destructive, failing strip probes."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from native_build import MODULE, SIM, WEST, WORKSPACE
from native_process import run_logged


def main():
    root = Path(tempfile.mkdtemp(prefix="rgb-composite-engine-")).resolve()
    print(f"ARTIFACTS {root}", flush=True)
    build = root / "build"
    command = shlex.split(WEST) + [
        "build", "-s", "app", "-d", str(build),
        "-b", "native_sim//zmk_test_mock", "-p", "--",
        f"-DZMK_CONFIG={SIM / 'config-composite'}",
        f"-DZMK_EXTRA_MODULES={MODULE};{SIM / 'module'}",
    ]
    result = run_logged(
        command, cwd=WORKSPACE,
        env={**os.environ, "ZEPHYR_TOOLCHAIN_VARIANT": "host"},
        text=True, stderr=subprocess.STDOUT, timeout=600,
        log_path=root / "build.log",
    )
    if result.returncode:
        raise AssertionError(f"Build failed: {root / 'build.log'}\n{result.stdout[-16000:]}")
    result = run_logged(
        [str(build / "zephyr/zmk.exe"), "--stop_at=5", "-no-rt"],
        text=True, stderr=subprocess.STDOUT, timeout=30,
        log_path=root / "runtime.log",
    )
    if result.returncode or "PASS composite engine integration" not in result.stdout:
        raise AssertionError(f"Runtime failed: {root / 'runtime.log'}\n{result.stdout[-16000:]}")
    print(result.stdout, end="")


if __name__ == "__main__":
    main()
