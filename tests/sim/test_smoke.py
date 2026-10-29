#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Bounded smoke capture and explicit capture-error regressions."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys

from native_build import MODULE, SIM, WEST, WORKSPACE, native_build_root
from native_process import BUILD_TIMEOUT, run_logged


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--artifacts', type=Path)
    parser.add_argument('--duration', type=float, default=float(os.environ.get('DURATION', '7')))
    args = parser.parse_args()
    root = native_build_root(args.artifacts, "smoke", prefix='rgb-smoke-',
                             default=os.environ.get('OUT_DIR'))
    root.mkdir(parents=True, exist_ok=True)
    build_dir = native_build_root(args.build_dir, "smoke-build",
                                  default=os.environ.get('BUILD_DIR', MODULE / 'build/native-smoke'))
    print(f'Smoke artifacts: {root}', flush=True)

    def execute(name, command, timeout=30):
        log = root / (name + '.log')
        print(f'COMMAND {shlex.join(map(str, command))}; log: {log}', flush=True)
        try:
            return run_logged(command, cwd=WORKSPACE, log_path=log, timeout=timeout,
                              env={**os.environ, 'ZEPHYR_TOOLCHAIN_VARIANT': 'host'},
                              text=True, stderr=subprocess.STDOUT)
        except subprocess.TimeoutExpired as error:
            raise RuntimeError(f'{name} timed out; log: {log}') from error

    built = execute('build', shlex.split(WEST) + [
        'build', '-s', 'app', '-d', str(build_dir), '-b', 'native_sim//zmk_test_mock', '-p', '--',
        '-DCONFIG_ASSERT=y', f'-DZMK_CONFIG={SIM / "config-smoke"}',
        f'-DZMK_EXTRA_MODULES={MODULE};{SIM / "module"}',
    ], BUILD_TIMEOUT)
    if built.returncode:
        raise RuntimeError(f'Smoke build failed; log: {root / "build.log"}\n{built.stdout[-5000:]}')
    exe = build_dir / 'zephyr/zmk.exe'
    capture = root / 'capture.bin'
    result = execute('runtime', [str(exe), f'--capture={capture}', f'--stop_at={args.duration}',
                                 '--caps-on=4200', '--caps-off=5500', '-no-rt'])
    if result.returncode:
        raise RuntimeError(f'Smoke runtime failed; log: {root / "runtime.log"}\n{result.stdout[-5000:]}')
    checked = execute('capture-check', [sys.executable, str(SIM / 'check_capture.py'), str(capture)])
    if checked.returncode:
        raise RuntimeError(f'Smoke capture assertions failed; log: {root / "capture-check.log"}\n{checked.stdout}')
    print('PASS smoke capture', flush=True)

    missing = capture / 'missing.bin'
    result = execute('negative-open', [str(exe), f'--capture={missing}', '--stop_at=1', '-no-rt'])
    if result.returncode != 1 or 'cannot open' not in result.stdout:
        raise RuntimeError(f'Wrong open rejection; log: {root / "negative-open.log"}\n{result.stdout}')
    print('PASS smoke open rejection', flush=True)
    if os.access('/dev/full', os.W_OK):
        result = execute('negative-full', [str(exe), '--capture=/dev/full', '--stop_at=1', '-no-rt'])
        if result.returncode != 1 or 'capture write failed' not in result.stdout:
            raise RuntimeError(f'Wrong write rejection; log: {root / "negative-full.log"}\n{result.stdout}')
        print('PASS smoke write rejection', flush=True)
    else:
        print('Unavailable /dev/full: Linux write-error boundary not exercised.', flush=True)
    print(f'PASS smoke tests; artifacts: {root}', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
