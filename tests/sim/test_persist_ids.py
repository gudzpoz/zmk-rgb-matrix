#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Validate that node names do not constrain positional persistence."""
from pathlib import Path
import subprocess
import tempfile

from native_process import run_logged
from native_build import SELECTORS, build

SELECTOR_IDS = SELECTORS.replace("keypaw,rgb-matrix-selector-probe",
                                 "keypaw,rgb-matrix-solid").replace(" animate;", "")


def run_image(root, name):
    run = run_logged(
        [str(root / name / "build/zephyr/zmk.exe"), "--stop_at=1", "-no-rt"],
        cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=30, log_path=root / f"{name}.log")
    assert run.returncode == 0, run.stdout
    assert "<err>" not in run.stdout, run.stdout


def main():
    root = Path(tempfile.mkdtemp(prefix="rgb-persist-ids-"))
    print(f"ARTIFACTS {root}", flush=True)

    accepted = (SELECTOR_IDS
        .replace('selector_a: selector_a {',
                 'selector_a: selector+bad { #address-cells = <1>; #size-cells = <0>;')
        .replace('probe_a: a_static {', 'probe_a: a_static+bad {')
        .replace('probe_c: a_motion {', 'probe_c: motion@2 { reg = <2>;')
        .replace('compatible = "keypaw,rgb-matrix-solid";',
                 'compatible = "test,rgb-token-solid", "keypaw,rgb-matrix-solid";')
        .replace('disabled {', 'bad+disabled {'))
    for settings in ("n", "y"):
        suffix = "on" if settings == "y" else "off"
        conf = f"CONFIG_SETTINGS={settings}\n"
        name = f"accepted-tokens-settings-{suffix}"
        result = build(root, name, "", selectors=accepted, extra_conf=conf)
        assert result.returncode == 0, result.stdout[-16000:]
        run_image(root, name)
        print(f"PASS punctuation/unit-address names and disabled child settings-{suffix}", flush=True)

        maximum = SELECTOR_IDS.replace("selector_a: selector_a {", "selector_a: " + "s" * 30 + " {").replace(
            "probe_a: a_static {", "probe_a: " + "e" * 31 + " {")
        name = f"maximum-names-settings-{suffix}"
        result = build(root, name, "", selectors=maximum, extra_conf=conf)
        assert result.returncode == 0, result.stdout[-16000:]
        run_image(root, name)
        print(f"PASS former token boundaries accepted settings-{suffix}", flush=True)

        for target, declaration in (("selector", "selector_a: selector_a {"),
                                    ("member", "probe_a: a_static {")):
            selectors = SELECTOR_IDS.replace(
                declaration, declaration.split(":")[0] + ": " + "x" * 64 + " {")
            if target == "member":
                # The Zephyr device-name limit is independent of persistence.
                selectors = selectors.replace("x" * 64 + " {", "x" * 64 + ' { label = "longfx";')
            name = f"overlong-{target}-settings-{suffix}"
            result = build(root, name, "", selectors=selectors, extra_conf=conf)
            assert result.returncode == 0, result.stdout[-16000:]
            run_image(root, name)
            print(f"PASS long {target} name accepted settings-{suffix}", flush=True)

    collision = SELECTOR_IDS + '''
/ { second_parent {
    collision_selector: selector-a {
    compatible = "keypaw,rgb-overlay"; leds = <0 1>;
    controllable; #binding-cells = <2>; label = "colctl";
    condition = <&preview_visible>; initial-effect = <0>;
    collision_effect: collision_effect { compatible = "keypaw,rgb-matrix-solid";
        };
}; }; };
'''
    for settings in ("n", "y"):
        name = f"cross-parent-token-collision-settings-{settings}"
        result = build(root, name, "", selectors=collision,
                       extra_conf=f"CONFIG_SETTINGS={settings}\n")
        assert result.returncode != 0, result.stdout[-16000:]
        assert "overlay must be a child of the keypaw,rgb-matrix node" in result.stdout, result.stdout[-16000:]
        print(f"PASS cross-parent selector rejected settings-{settings}", flush=True)


if __name__ == "__main__":
    main()
