#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Build and execute the standalone native_sim ztest application."""
import argparse
from datetime import datetime, timezone
import os
from pathlib import Path
import shlex
import subprocess
import sys

from native_build import MODULE, WEST, WORKSPACE
from native_process import BUILD_TIMEOUT, UTILITY_TIMEOUT, run_logged

APP = MODULE / "tests" / "unit"
SUCCESS_MARKER = "PROJECT EXECUTION SUCCESSFUL"


def unique_log(directory, name):
    directory.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    path = directory / f"{stamp}-{name}.log"
    suffix = 1
    while path.exists():
        path = directory / f"{stamp}-{name}-{suffix}.log"
        suffix += 1
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=MODULE / "build/ztest")
    parser.add_argument("--artifact-dir", type=Path,
                        default=MODULE / "build/native-artifacts/ztest")
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    artifact_dir = args.artifact_dir.resolve()
    build_dir.mkdir(parents=True, exist_ok=True)
    if not (WORKSPACE / "app" / "CMakeLists.txt").is_file():
        parser.error(f"ZMK workspace is missing app/CMakeLists.txt: {WORKSPACE}")
    if not APP.is_dir():
        parser.error(f"standalone ztest app is missing: {APP}")

    build_command = shlex.split(WEST) + [
        "build", "-s", str(APP), "-d", str(build_dir),
        "-b", "native_sim/native/64", "-p",
    ]
    build_log = unique_log(artifact_dir, "build")
    print("Building standalone native_sim ztest app", flush=True)
    try:
        built = run_logged(
            build_command, cwd=WORKSPACE, log_path=build_log,
            timeout=BUILD_TIMEOUT, text=True, stderr=subprocess.STDOUT,
            env={**os.environ, "ZEPHYR_TOOLCHAIN_VARIANT": "host"},
        )
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(f"ztest build timed out; log: {build_log}") from error
    if built.returncode:
        raise RuntimeError(f"ztest build failed (exit {built.returncode}); log: {build_log}\n"
                           f"{built.stdout[-5000:]}")

    executable = build_dir / "zephyr" / "zephyr.exe"
    if not executable.is_file():
        raise RuntimeError(f"ztest executable was not produced: {executable}; build log: {build_log}")

    run_log = unique_log(artifact_dir, "run")
    try:
        result = run_logged(
            [str(executable), "-no-rt"], cwd=WORKSPACE, log_path=run_log,
            timeout=UTILITY_TIMEOUT, text=True, stderr=subprocess.STDOUT,
        )
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(f"ztest execution timed out; log: {run_log}") from error
    if result.returncode or SUCCESS_MARKER not in result.stdout:
        raise RuntimeError(f"ztest execution failed (exit {result.returncode}, "
                           f"success marker required); log: {run_log}\n{result.stdout[-5000:]}")
    print(f"PASS standalone native_sim ztest; log: {run_log}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
