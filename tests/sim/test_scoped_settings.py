#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Real whole-snapshot settings handler, bounded failure and in-flight request tests."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from native_process import run_logged
from native_build import SELECTORS, build

SELECTORS = (SELECTORS.replace('keypaw,rgb-matrix-selector-probe', 'keypaw,rgb-matrix-solid')
             .replace(' animate;', ''))
REORDERED = '''
&rgb_matrix {
    /delete-node/ fixture_overlay; /delete-node/ fixture_battery; /delete-node/ fixture_batt_sub;
    selector_b: selector_b {
        compatible = "keypaw,rgb-overlay"; initial-effect = <1>;
        controllable; #binding-cells = <2>; label = "rgbs_b";
        leds = <2 3>; condition = <&preview_visible>;
        probe_d: b_alt { compatible = "keypaw,rgb-matrix-solid";
            color = <0x00ffff>; duration = <0xffffffff>; };
        probe_b: b_static { compatible = "keypaw,rgb-matrix-solid";
            color = <0xffff00>; duration = <0xffffffff>; };
    };
    selector_a: selector_a {
        compatible = "keypaw,rgb-overlay"; initial-effect = <2>;
        controllable; #binding-cells = <2>; label = "rgbs_a";
        leds = <0 1>; condition = <&preview_visible>;
        optout: optout { compatible = "keypaw,rgb-matrix-solid"; color = <0xff0000>; };
        probe_c: a_motion { compatible = "keypaw,rgb-matrix-solid"; color = <0x0000ff>; };
        disabled { compatible = "keypaw,rgb-matrix-solid"; status = "disabled"; };
        probe_a: a_static { compatible = "keypaw,rgb-matrix-solid"; color = <0x00ff00>; };
    };
    ordinary: ordinary { compatible = "keypaw,rgb-overlay"; leds = <1>;
        condition = <&preview_visible>;
        private_white: white { compatible = "keypaw,rgb-matrix-solid"; color = <0xffffff>; };
    };
};
&rgb_conditions {
    preview_visible: preview_visible { compatible = "keypaw,rgb-condition-effect-selected";
        selector = <&preview_selector>; effect = <&fx_solid>; };
};
&kscan { events = <>; };
'''
BOOT_ACTION = '''
#include <dt-bindings/keypaw/rgb_matrix.h>
&rgb_conditions {
    reinterpreted_selection: reinterpreted_selection {
        compatible = "keypaw,rgb-condition-effect-selected";
        selector = <&selector_a>; effect = <&probe_c>;
    };
};
/ {
    settings_boot_actions {
        compatible = "keypaw,rgb-trigger-table";
        selected { condition = <&reinterpreted_selection>;
            on-enter = <&rgb_matrix RGB_OFF_CMD 0>; };
    };
};
'''
CONF = '''CONFIG_KEYPAW_SIM_SELECTOR_TEST=n
CONFIG_SETTINGS=y
CONFIG_SETTINGS_CUSTOM=y
CONFIG_KEYPAW_SIM_SETTINGS_TEST=y
CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE=20
'''
SCENARIOS = ("existing", "validation",
             "debounce", "global-failure", "staged-reset",
             "selector-dirty", "selector-failed-dirty",
             "selector-dirty-second-failure", "selector-failed-dirty-second-failure",
             "selector-reset", "selector-failed-reset",
             "selector-reset-second-failure", "selector-failed-reset-second-failure",
             "global-reset", "global-failed-reset",
             "global-reset-second-failure", "global-failed-reset-second-failure",
             "reordering")
CONFIGS = ("auto", "original", "reordered")


def unique_log(root, stem):
    path = root / f"{stem}.log"
    attempt = 2
    while path.exists():
        path = root / f"{stem}-{attempt}.log"
        attempt += 1
    return path


def run_case(root, name, scenario, storage=None):
    command = [str(root / name / "build/zephyr/zmk.exe"), "-no-rt",
               "--stop_at=10", f"--settings-scenario={scenario}"]
    if storage is not None:
        command.append(f"--settings-storage={storage}")
    print("COMMAND", " ".join(command), flush=True)
    result = run_logged(command, cwd=root, text=True, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, timeout=30,
                        log_path=unique_log(root, f"{name}-{scenario}"))
    assert result.returncode == 0, result.stdout[-20000:]
    assert f"PASS settings {scenario}" in result.stdout, result.stdout
    assert "PASS scoped settings provider before restoration" in result.stdout
    if scenario == "reordering":
        assert "PASS settings positional boot action" in result.stdout
    if scenario == "existing":
        assert "PASS scoped settings legacy records ignored" in result.stdout
    errors = [line for line in result.stdout.splitlines() if "<err>" in line]
    assert all("set-value failure. key: keypaw/rgb_matrix/" in line
               for line in errors), result.stdout
    print(f"PASS scoped settings native {name}/{scenario}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario", choices=SCENARIOS, action="append")
    parser.add_argument("--config", choices=CONFIGS, action="append")
    parser.add_argument("--artifacts", type=Path)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("--repeat must be positive")
    root = (args.artifacts or Path(tempfile.mkdtemp(prefix="rgb-scoped-settings-"))).resolve()
    root.mkdir(parents=True, exist_ok=True)
    print(f"ARTIFACTS {root}", flush=True)
    configs = args.config or CONFIGS
    if "reordered" in configs and "original" not in configs:
        parser.error("--config reordered requires --config original for the shared-blob oracle")
    for name in configs:
        if not args.no_build:
            result = build(root, name, BOOT_ACTION if name in ("original", "reordered") else "",
                           runtime=True, selectors=REORDERED if name == "reordered" else SELECTORS,
                           extra_conf=CONF)
            assert result.returncode == 0, result.stdout[-20000:]
        if name in ("original", "reordered"):
            continue
        scenarios = args.scenario or SCENARIOS
        for _ in range(args.repeat):
            for scenario in scenarios:
                if scenario != "reordering":
                    run_case(root, name, scenario)
    if (not args.scenario or "reordering" in args.scenario) and "original" in configs:
        for repeat in range(args.repeat):
            storage = root / f"settings-storage-{repeat}.bin"
            storage.unlink(missing_ok=True)
            run_case(root, "original", "reordering", storage)
            original = storage.read_bytes()
            assert original == bytes((1, 1, 61, 0, 3, 0, 0, 0, 0, 0, 1, 0)), original
            if "reordered" in configs:
                run_case(root, "reordered", "reordering", storage)
                assert storage.read_bytes() == original
                run_case(root, "original", "reordering", storage)
                assert storage.read_bytes() == original
                print("PASS settings same blob original/reordered/original boot actions", flush=True)


if __name__ == "__main__":
    main()
