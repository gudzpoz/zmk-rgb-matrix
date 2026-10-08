#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Run the real RGB engine against nested, destructive, failing strip probes."""

import os
from pathlib import Path
import subprocess
import tempfile

SIM = Path(__file__).resolve().parent
MODULE = SIM.parent.parent
REPO = MODULE.parent.parent
WORKSPACE = Path(os.environ.get("ZMK_WS", REPO / "zmk")).resolve()
LOCAL_WEST = REPO / ".venv/bin/west"
WEST = os.environ.get("WEST", str(LOCAL_WEST) if LOCAL_WEST.is_file() else "west")


def main():
    with tempfile.TemporaryDirectory(prefix="rgb-composite-engine-") as directory:
        build = Path(directory) / "build"
        command = [WEST, "build", "-s", "app", "-d", str(build),
                   "-b", "native_sim//zmk_test_mock", "-p", "--",
                   f"-DZMK_CONFIG={SIM / 'config-composite'}",
                   f"-DZMK_EXTRA_MODULES={MODULE};{SIM / 'module'}"]
        result = subprocess.run(command, cwd=WORKSPACE, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode:
            raise AssertionError(result.stdout[-16000:])
        result = subprocess.run([str(build / "zephyr/zmk.exe"), "--stop_at=5", "-no-rt"],
                                text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=30)
        if result.returncode or "PASS composite engine integration" not in result.stdout:
            raise AssertionError(result.stdout[-16000:])
        print(result.stdout, end="")


if __name__ == "__main__":
    main()
