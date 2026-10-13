#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Real settings handler, storage failures and unlocked-save revision races."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from native_process import run_logged
from native_build import SELECTORS, build

SELECTORS = (SELECTORS.replace('keypaw,rgb-matrix-selector-probe', 'keypaw,rgb-matrix-solid')
             .replace(' animate;', '') + '''
&selector_a { persist-id = "a"; };
&selector_b { persist-id = "b"; };
&probe_a { persist-id = "shared"; };
&probe_b { persist-id = "shared"; };
&probe_c { persist-id = "alt"; };
&probe_d { persist-id = "alt"; };
''')
REORDERED = '''
&rgb_overlays {
    /delete-node/ fixture_overlay; /delete-node/ fixture_battery; /delete-node/ fixture_batt_sub;
    selector_b: selector_b {
        compatible = "keypaw,rgb-overlay-selector"; persist-id = "b"; initial-effect = <1>;
        initial-duration-ms = <0xffffffff>; leds = <2 3>; condition = <&preview_visible>;
        probe_d: b_alt { compatible = "keypaw,rgb-matrix-solid"; #binding-cells = <0>;
            color = <0x00ffff>; duration = <0>; persist-id = "alt"; };
        probe_b: b_static { compatible = "keypaw,rgb-matrix-solid"; #binding-cells = <0>;
            color = <0xffff00>; persist-id = "shared"; };
    };
    selector_a: selector_a {
        compatible = "keypaw,rgb-overlay-selector"; persist-id = "a"; initial-effect = <3>;
        leds = <0 1>; condition = <&preview_visible>;
        optout: optout { compatible = "keypaw,rgb-matrix-solid"; #binding-cells = <0>;
            color = <0xff0000>; };
        probe_c: a_motion { compatible = "keypaw,rgb-matrix-solid"; #binding-cells = <0>;
            color = <0x0000ff>; persist-id = "alt"; };
        disabled { compatible = "keypaw,rgb-matrix-solid"; #binding-cells = <0>; status = "disabled"; };
        probe_a: a_static { compatible = "keypaw,rgb-matrix-solid"; #binding-cells = <0>;
            color = <0x00ff00>; persist-id = "shared"; };
    };
    ordinary: ordinary { compatible = "keypaw,rgb-overlay"; leds = <1>;
        condition = <&preview_visible>;
        private_white: white { compatible = "keypaw,rgb-matrix-solid"; #binding-cells = <0>; color = <0xffffff>; };
    };
};
/ { behaviors {
    rgbs_a: rgbs_a { compatible = "keypaw,behavior-rgb-selector"; #binding-cells = <2>; selector = <&selector_a>; };
    rgbs_b: rgbs_b { compatible = "keypaw,behavior-rgb-selector"; #binding-cells = <2>; selector = <&selector_b>; };
}; };
&kprgb { selector = <&selector_a>; };
&rgb_conditions {
    preview_visible: preview_visible { compatible = "keypaw,rgb-condition-effect-selected";
        selector = <&preview_selector>; effect = <&fx_solid>; };
};
&kscan { events = <>; };
'''
CONF = '''CONFIG_KEYPAW_SIM_SELECTOR_TEST=n
CONFIG_SETTINGS=y
CONFIG_SETTINGS_CUSTOM=y
CONFIG_KEYPAW_SIM_SETTINGS_TEST=y
CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE=20
'''
SCENARIOS = ("existing", "validation", "debounce", "global-failure", "staged-reset",
             "selector-dirty", "selector-reset", "selector-failed-dirty",
             "selector-failed-reset", "global-dirty", "global-reset",
             "global-failed-dirty", "global-failed-reset", "reset-revision-second-failure", "reordering")
CONFIGS = {
    "auto": ("", ""),
    "manual": ("", "CONFIG_KEYPAW_RGB_CONTROL_AUTO_START=n\n"),
    "late-control": ('''&rgbs_a { compatible = "test,rgb-selector-late-control", "keypaw,behavior-rgb-selector"; zephyr,deferred-init; };''',
                     "CONFIG_KEYPAW_RGB_CONTROL_AUTO_START=n\n"),
    "late-member": ('&probe_b { compatible = "test,rgb-settings-late-effect", "keypaw,rgb-matrix-solid"; zephyr,deferred-init; };',
                    "CONFIG_KEYPAW_RGB_CONTROL_AUTO_START=n\n"),
    "reordered": ("", ""),
    "ids-duplicate": ('&selector_b { persist-id = "a"; };', ""),
    "ids-invalid": ('&selector_b { persist-id = "bad/id"; };', ""),
}


def unique_log(root, stem):
    path = root / f"{stem}.log"
    attempt = 2
    while path.exists():
        path = root / f"{stem}-{attempt}.log"
        attempt += 1
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario", choices=SCENARIOS + ("ids-duplicate", "ids-invalid"), action="append")
    parser.add_argument("--config", choices=CONFIGS, action="append")
    parser.add_argument("--artifacts", type=Path)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("--repeat must be positive")
    root = args.artifacts or Path(tempfile.mkdtemp(prefix="rgb-scoped-settings-"))
    root.mkdir(parents=True, exist_ok=True)
    print(f"ARTIFACTS {root}", flush=True)
    for name in args.config or CONFIGS:
        fragment, config = CONFIGS[name]
        if not args.no_build:
            result = build(root, name, fragment, runtime=True,
                           selectors=REORDERED if name == "reordered" else SELECTORS,
                           extra_conf=CONF + config)
            assert result.returncode == 0, result.stdout[-20000:]
        scenarios = ((name,) if name.startswith("ids-") else
                     ("reordering", "validation", "staged-reset") if name == "reordered" else SCENARIOS)
        selected = args.scenario or scenarios
        assert all(scenario in scenarios for scenario in selected), (name, selected, scenarios)
        for scenario in (scenario for _ in range(args.repeat) for scenario in selected):
            command = [str(root / name / "build/zephyr/zmk.exe"), "-no-rt",
                       "--stop_at=10", f"--settings-scenario={scenario}"]
            if scenario == "reordering":
                command.append(f"--settings-storage={root / 'settings-storage.bin'}")
            print("COMMAND", " ".join(command), flush=True)
            result = run_logged(command, cwd=root, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=30,
                                log_path=unique_log(root, f"{name}-{scenario}"))
            assert result.returncode == 0, result.stdout[-20000:]
            markers = ("restored before startup notification", "legacy records ignored",
                       "independent save despite persistent failure",
                       "addressed save/reset/reload isolation", "global reset/reload")
            if scenario == "existing":
                for marker in markers:
                    assert f"PASS scoped settings {marker}" in result.stdout, result.stdout
            else:
                assert f"PASS settings {scenario}" in result.stdout, result.stdout
            if name in ("manual", "late-control", "late-member"):
                assert "PASS scoped settings subtree/manual startup" in result.stdout
            errors = [line for line in result.stdout.splitlines() if "<err>" in line]
            assert all("set-value failure. key: keypaw/rgb_matrix/" in line or
                       (name == "late-control" and "canonical control is not ready" in line) or
                       (name == "ids-duplicate" and "RGB selector persist-id or node name must be globally unique" in line) or
                       (name == "ids-invalid" and "persistence IDs invalid (err -22)" in line)
                       for line in errors), result.stdout
            print(f"PASS scoped settings native {name}/{scenario}", flush=True)

    if (not args.scenario or "reordering" in args.scenario) and all(
            name in (args.config or CONFIGS) for name in ("auto", "reordered")):
        command = [str(root / "auto/build/zephyr/zmk.exe"), "-no-rt", "--stop_at=10",
                   "--settings-scenario=reordering", f"--settings-storage={root / 'settings-storage.bin'}"]
        result = run_logged(command, cwd=root, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=30,
                            log_path=unique_log(root, "reordered-back-to-auto"))
        assert result.returncode == 0 and "PASS settings reordering" in result.stdout, result.stdout
        print("PASS settings reordered firmware back to original slots", flush=True)

if __name__ == "__main__":
    main()
