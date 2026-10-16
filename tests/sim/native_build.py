# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Shared pristine native builds and isolated scenario processes."""
import os
from pathlib import Path
import shlex
import subprocess

from native_process import run_logged

SIM = Path(__file__).resolve().parent
MODULE = SIM.parent.parent
REPO = MODULE.parent.parent
WORKSPACE = Path(os.environ.get("ZMK_WS", REPO / "zmk"))
LOCAL_WEST = REPO / ".venv/bin/west"
WEST = os.environ.get("WEST", str(LOCAL_WEST) if LOCAL_WEST.is_file() else "west")
SELECTORS = '''
&rgb_matrix {
    /delete-node/ fixture_overlay;
    /delete-node/ fixture_battery;
    /delete-node/ fixture_batt_sub;
    selector_a: selector_a {
        compatible = "keypaw,rgb-overlay-selector";
        leds = <0 1>;
        condition = <&preview_visible>;
        initial-effect = <0>;
        disabled { compatible = "keypaw,rgb-matrix-solid";
            #binding-cells = <0>; status = "disabled"; };
        probe_a: a_static { compatible = "keypaw,rgb-matrix-selector-probe";
            #binding-cells = <0>; color = <0x00ff00>; };
        probe_c: a_motion { compatible = "keypaw,rgb-matrix-selector-probe";
            #binding-cells = <0>; color = <0x0000ff>; animate; };
    };
    selector_b: selector_b {
        compatible = "keypaw,rgb-overlay-selector";
        condition = <&preview_visible>;
        leds = <2 3>;
        initial-effect = <99>;
        initial-duration-ms = <0xffffffff>;
        probe_b: b_static { compatible = "keypaw,rgb-matrix-selector-probe";
            #binding-cells = <0>; color = <0xffff00>; };
        probe_d: b_alt { compatible = "keypaw,rgb-matrix-selector-probe";
            #binding-cells = <0>; color = <0x00ffff>; duration = <0>; };
    };
    ordinary: ordinary {
        compatible = "keypaw,rgb-overlay";
        leds = <1>;
        condition = <&preview_visible>;
        private_white: white { compatible = "keypaw,rgb-matrix-solid"; #binding-cells = <0>; color = <0xffffff>; };
    };
};
/ {
    behaviors {
        rgbs_a: rgbs_a {
            compatible = "keypaw,behavior-rgb-selector";
            #binding-cells = <2>;
            selector = <&selector_a>;
        };
        rgbs_b: rgbs_b {
            compatible = "keypaw,behavior-rgb-selector";
            #binding-cells = <2>;
            selector = <&selector_b>;
        };
    };
};
&rgb_conditions {
    preview_visible: preview_visible {
        compatible = "keypaw,rgb-condition-effect-selected";
        selector = <&preview_selector>; effect = <&fx_solid>;
    };
};
&kscan { events = <>; };
'''


def build(root, name, fragment, runtime=False, selectors=None, extra_conf=""):
    root = Path(root)
    config = root / name / "config"
    config.mkdir(parents=True, exist_ok=True)
    for filename in ("native_sim.conf", "native_sim.keymap"):
        (config / filename).write_text((SIM / "config" / filename).read_text())
    with (config / "native_sim.conf").open("a") as output:
        output.write("\nCONFIG_ASSERT=y\nCONFIG_ZMK_BEHAVIOR_METADATA=y\n")
        if runtime:
            output.write("CONFIG_KEYPAW_SIM_SELECTOR_TEST=y\n")
        output.write(extra_conf)
    (config / "native_sim.overlay").write_text(
        f'#include "{SIM / "config/native_sim.overlay"}"\n'
        + (SELECTORS if selectors is None else selectors) + fragment)
    command = shlex.split(WEST) + ["build", "-s", "app", "-d", str(root / name / "build"),
               "-b", "native_sim//zmk_test_mock", "-p", "--",
               f"-DZMK_CONFIG={config}", f"-DZMK_EXTRA_MODULES={MODULE};{SIM / 'module'}"]
    print("COMMAND", " ".join(command), flush=True)
    return run_logged(command, cwd=WORKSPACE, env={**os.environ, "ZEPHYR_TOOLCHAIN_VARIANT": "host"}, text=True,
                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                      timeout=600, log_path=root / f"{name}-build.log")


def run(root, image, scenario, *, arguments=(), timeout=30, marker=None, scenario_option="rgb-test-scenario"):
    root = Path(root)
    binary = root / image / "build/zephyr/zmk.exe"
    command = [str(binary), "-no-rt", f"--{scenario_option}={scenario}", *arguments]
    log_path = root / f"{image}-{scenario}.log"
    attempt = 2
    while log_path.exists():
        log_path = root / f"{image}-{scenario}-{attempt}.log"
        attempt += 1
    result = run_logged(command, text=True, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, timeout=timeout, log_path=log_path)
    if result.returncode or (marker or f"PASS {scenario}") not in result.stdout:
        raise AssertionError(f"Native scenario failed: {log_path}\n{result.stdout}")
    return result
