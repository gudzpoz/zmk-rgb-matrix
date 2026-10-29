#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Real native images for static overlay targets and iterable conditions."""
import argparse
from pathlib import Path
import subprocess

from native_build import MODULE, build, native_build_root
from native_process import run_logged

FIXTURE = Path(__file__).resolve().parent / "config-static-metadata"
CASES = ("dense-peripheral", "zero-leds", "zero-overlays", "ordinal-boundary")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", action="append", choices=CASES)
    parser.add_argument("--build-dir", type=Path)
    args = parser.parse_args()
    root = native_build_root(args.build_dir, "static-metadata", prefix="static-metadata-",
                             parent=MODULE / "build/native-artifacts")
    print(f"Static metadata artifacts: {root}", flush=True)
    cases = list(dict.fromkeys(args.case or CASES))
    for name in cases:
        fixture = 'dense' if name in ('ordinal-boundary', 'dense-peripheral') else name
        fragment = f'#include "{FIXTURE / (fixture + ".overlay")}"\n'
        if name == 'ordinal-boundary':
            fragment += '/ { ordinal_boundary: ordinal_boundary {}; };\n&rgb_conditions {\n'
            fragment += '\n'.join(
                f'ordinal_{i} {{ compatible = "test,rgb-static-condition-b"; }};'
                for i in range(128))
            fragment += '\n};\n'
        conf = "\nCONFIG_KEYPAW_SIM_STATIC_METADATA_TEST=y\nCONFIG_ZMK_HID_INDICATORS=y\n"
        if name == 'dense-peripheral':
            fragment += '&sm00 { local; }; &sm06 { local; };\n'
            conf += 'CONFIG_ZMK_SPLIT=y\nCONFIG_ZMK_SPLIT_ROLE_CENTRAL=n\n'
        result = build(root, name, fragment, selectors="", extra_conf=conf)
        if result.returncode:
            raise AssertionError(f"Build failed: {root / (name + '-build.log')}\n"
                                 + result.stdout[-16000:])
        result = run_logged([str(root / name / "build/zephyr/zmk.exe"), "-no-rt"],
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=30, log_path=root / f"{name}-runtime.log")
        if result.returncode or "PASS static_metadata" not in result.stdout:
            raise AssertionError(f"Runtime failed: {root / (name + '-runtime.log')}\n"
                                 + result.stdout)
        print(f"PASS static_metadata {name}", flush=True)
    print(f"PASS static_metadata suite: {len(cases)} cases; artifacts: {root}", flush=True)


if __name__ == "__main__":
    main()
