#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Real split worker/receiving contracts with external transport mocks only."""
import argparse
from pathlib import Path
import tempfile

from native_build import build, run
from native_protocol import SOLID_SELECTORS, CONF

ZERO = '''
&rgb_matrix { /delete-property/ strip; mapping = <>; };
&selector_a { /delete-property/ leds; };
&selector_b { /delete-property/ leds; };
&ordinary { /delete-property/ leds; };
'''
SCENARIOS = ("existing", "color", "period", "selection", "reset-color", "reset-period",
             "reset-selection", "pacing-color", "pacing-period", "pacing-selection",
             "fairness", "transport-change", "local-silence", "clock")
SCENARIOS += tuple(f"sweep-{kind}-{phase}" for kind in
                   ("dirty", "reset", "failed-dirty", "failed-reset") for phase in range(7))
SCENARIOS += tuple(f"global-reset-{kind}-{phase}" for kind in ("success", "failed")
                   for phase in ("color", "period", "selection"))
SCENARIOS += ("identical", "dirty-hook", "dirty-timing", "no-transport", "negative-query")
THREE = '''&selector_a { probe_e: a_third { compatible = "keypaw,rgb-matrix-solid";
    #binding-cells = <0>; color = <0xff0000>; duration = <1700>; }; };'''
CONFIGS = {
    "transport": ("", CONF), "transport-zero": (ZERO, CONF),
    "transport-dirty-timing": ("", CONF + "CONFIG_KEYPAW_RGB_SPLIT_SYNC_SETTLE_MS=300\n"),
    "transport-dirty-timing-zero": (ZERO, CONF + "CONFIG_KEYPAW_RGB_SPLIT_SYNC_SETTLE_MS=300\n"),
    "transport-absent": ("", CONF + "CONFIG_ZMK_BLE_CLEAR_BONDS_ON_START=y\nCONFIG_SETTINGS=y\nCONFIG_SETTINGS_NONE=y\n"),
    "transport-absent-zero": (ZERO, CONF + "CONFIG_ZMK_BLE_CLEAR_BONDS_ON_START=y\nCONFIG_SETTINGS=y\nCONFIG_SETTINGS_NONE=y\n"),
    "transport-three": (THREE, CONF), "transport-three-zero": (THREE + ZERO, CONF),
    "late-control": ('''&rgbs_a { compatible = "test,rgb-selector-late-control",
        "keypaw,behavior-rgb-selector"; zephyr,deferred-init; };''', CONF),
    "late-member": ('''&probe_a { compatible = "test,rgb-settings-late-effect", "keypaw,rgb-matrix-solid"; zephyr,deferred-init; };
        &probe_c { compatible = "test,rgb-settings-late-effect", "keypaw,rgb-matrix-solid"; zephyr,deferred-init; };''', CONF),
    "late-global": ('''&rgb_matrix { compatible = "test,rgb-split-late-global",
        "keypaw,rgb-matrix"; zephyr,deferred-init; };''', CONF),
}
for kind in ("control", "member"):
    CONFIGS[f"ready-{kind}"] = CONFIGS[f"late-{kind}"]
for name in ("receiving", "receiving-zero"):
    CONFIGS[name] = (ZERO if name.endswith("-zero") else "",
        "CONFIG_KEYPAW_SIM_SELECTOR_TEST=n\nCONFIG_KEYPAW_SIM_SPLIT_TEST=y\n"
        "CONFIG_TEST=y\nCONFIG_IRQ_OFFLOAD=y\nCONFIG_ZMK_SPLIT=y\nCONFIG_ZMK_SPLIT_ROLE_CENTRAL=n\n")


def supported(name):
    if name.startswith("receiving"):
        return ("receiving",)
    if name.startswith(("late-", "ready-")):
        return (name,)
    if "absent" in name:
        return ("no-transport",)
    if "dirty-timing" in name:
        return ("dirty-timing",)
    return tuple(s for s in SCENARIOS if s not in ("no-transport", "dirty-timing") and not (s.startswith("sweep-") and
                 int(s[-1]) >= 5 and "three" not in name))


def runtime(root, name, scenarios, repeat=1, no_build=False):
    fragment, config = CONFIGS[name]
    peripheral = name.startswith("receiving")
    if not no_build:
        result = build(root, name, '''/ { chosen { zephyr,bt-hci = &test_hci; };
            test_hci: bt_hci { compatible = "zephyr,bt-hci-userchan"; }; };''' + fragment,
                       runtime=True, selectors=SOLID_SELECTORS, extra_conf=config)
        assert result.returncode == 0, result.stdout[-20000:]
    assert (root / name / "build/zephyr/zmk.exe").is_file()
    for scenario in scenarios * repeat:
        marker = ("PASS scoped split off/inhibited reconnect endpoints/qualified ordering"
                  if scenario == "existing" else f"PASS split {scenario}")
        print("COMMAND", root / name / "build/zephyr/zmk.exe", "-no-rt",
              f"--split-scenario={scenario}", "--stop_at=20", flush=True)
        result = run(root, name, scenario, arguments=("--stop_at=20",), timeout=30,
                     marker=marker, scenario_option="split-scenario")
        if scenario == "existing":
            for marker in ("off/inhibited reconnect endpoints/qualified ordering",
                           "same-member parameter repair/independent lanes/frozen retries",
                           "rapid changes/retry convergence",
                           "disconnect/reconnect discards in-flight snapshot",
                           "outside-mutex transport/local errors",
                           "reset repairs inactive members after frozen retry",
                           "global behavior repairs frozen power/brightness",
                           "global reset repairs all selectors/members", "transport acceptance only"):
                assert f"PASS scoped split {marker}" in result.stdout, result.stdout
        else:
            assert f"PASS split {scenario}" in result.stdout, result.stdout
        errors = [line for line in result.stdout.splitlines() if "<err>" in line]
        assert all("HCI driver is not ready" in line or "BLUETOOTH FAILED (-19)" in line or
                   (name.startswith("late-") and "RGB control startup failed" in line) or
                   (peripheral and "Failed to invoke behavior" in line) or
                   (name == "late-control" and "selector_a canonical control is not ready" in line) or
                   (name == "late-member" and "selector_a has no ready effect" in line)
                   for line in errors), result.stdout
        print(f"PASS split native {name}/{scenario}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario", choices=SCENARIOS + ("receiving", "late-control", "late-member", "late-global", "ready-control", "ready-member"), action="append")
    parser.add_argument("--config", choices=CONFIGS, action="append")
    parser.add_argument("--artifacts", type=Path)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()
    root = (args.artifacts or Path(tempfile.mkdtemp(prefix="rgb-scoped-split-"))).resolve()
    root.mkdir(parents=True, exist_ok=True)
    print(f"ARTIFACTS {root}", flush=True)
    if args.repeat < 1 or (args.no_build and args.artifacts is None):
        parser.error("repeat must be positive; --no-build requires --artifacts")
    names = args.config or CONFIGS
    for name in names:
        invalid = set(args.scenario or ()) - set(supported(name))
        if invalid:
            parser.error(f"{name} does not support {sorted(invalid)}")
    for name in names:
        runtime(root, name, tuple(args.scenario or supported(name)), args.repeat, args.no_build)


if __name__ == "__main__":
    main()
