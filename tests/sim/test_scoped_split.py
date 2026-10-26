#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Real split worker/receiving contracts with external transport mocks only."""
import argparse
from pathlib import Path
import tempfile

from native_build import SELECTORS, COMPACT_PREVIEW, build, run
from native_protocol import SOLID_SELECTORS, CONF

ZERO = '''
&rgb_matrix { /delete-property/ strip; mapping = <>; };
&selector_a { /delete-property/ leds; };
&selector_b { /delete-property/ leds; };
&ordinary { /delete-property/ leds; };
'''
ORDERING_SELECTORS = (SELECTORS.replace("keypaw,rgb-matrix-selector-probe",
                                       "keypaw,rgb-matrix-solid").replace(" animate;", ""))
ORDERING_GATES = '''
&rgb_conditions {
    ordering_local: ordering_local {
        compatible = "keypaw,rgb-condition-effect-selected";
        selector = <&selector_b>; effect = <&probe_d>;
    };
};
&ordinary { local; condition = <&ordering_local>; };
'''
ORDERING_SETTINGS_CONF = (CONF + "CONFIG_SETTINGS=y\nCONFIG_SETTINGS_NONE=y\n"
                          "CONFIG_SETTINGS_RUNTIME=y\n")
SCENARIOS = ("existing", "retry-color", "retry-period", "retry-selection",
             "pacing-color", "pacing-period", "pacing-selection", "transport-change",
             "local-silence", "clock", "clock-failure", "no-transport", "negative-query")
ORDERING_SCENARIOS = ("ordering-before", "ordering-during", "ordering-overlap-forward",
                      "ordering-overlap-reverse", "ordering-overlap-active-forward",
                      "ordering-overlap-active-reverse", "ordering-release", "ordering-enqueue-error",
                      "ordering-cleanup", "ordering-publications", "ordering-noop",
                      "ordering-defaults", "ordering-gates", "ordering-isolated", "ordering-relative",
                      "ordering-failure", "ordering-global-failure", "ordering-silence", "ordering-gates-repair")
ORDERING_SCENARIOS += tuple(f"ordering-request-{where}" for where in
                            ("enqueue", "completion", "exit", "settling", "disconnect"))
SWEEP_SCENARIOS = tuple(f"ordering-sweep-{kind}-{phase}" for kind in
                        ("mutate", "reset", "failed-mutate", "failed-reset") for phase in range(10))
ORDERING_SCENARIOS += tuple(s for s in SWEEP_SCENARIOS if int(s[-1]) < 8)
SETTINGS_SCENARIOS = ("ordering-settings", "ordering-settings-active")
THREE = '''&selector_a { probe_e: a_third { compatible = "keypaw,rgb-matrix-solid";
    color = <0xff0000>; duration = <1700>; }; };'''
DIM = '''&probe_a { color = <0x008000>; };
&probe_c { color = <0x000040>; };
&probe_b { color = <0x808000>; };
&probe_d { color = <0x004040>; };'''
MULTIWORD = '&rgb_matrix {\n' + '\n'.join(f'''
    split_gate_{index}: split_gate_{index} {{
        compatible = "keypaw,rgb-overlay";
        all-leds;
        gate_fx_{index} {{ compatible = "keypaw,rgb-matrix-solid"; }};
    }};
''' for index in range(65)) + '};\n'
MULTIWORD_SCENARIOS = ("multiword-later", "multiword-final")
CONFIGS = {
    "transport": ("", CONF), "transport-zero": (ZERO, CONF),
    "transport-multiword": (MULTIWORD, CONF),
    "transport-multiword-zero": (MULTIWORD + ZERO, CONF),
    "transport-multiword-control": (MULTIWORD, CONF + "CONFIG_SETTINGS=y\nCONFIG_SETTINGS_CUSTOM=y\nCONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE=20\n"),
    "transport-multiword-control-zero": (MULTIWORD + ZERO, CONF + "CONFIG_SETTINGS=y\nCONFIG_SETTINGS_CUSTOM=y\nCONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE=20\n"),
    "ordering": (ORDERING_GATES, CONF), "ordering-zero": (ORDERING_GATES + ZERO, CONF),
    "ordering-settings": (ORDERING_GATES, ORDERING_SETTINGS_CONF),
    "brightness": (DIM, CONF), "brightness-zero": (DIM + ZERO, CONF),
    "transport-settling": ("", CONF + "CONFIG_KEYPAW_RGB_SPLIT_SYNC_SETTLE_MS=300\n"),
    "transport-settling-zero": (ZERO, CONF + "CONFIG_KEYPAW_RGB_SPLIT_SYNC_SETTLE_MS=300\n"),
    "transport-absent": ("", CONF + "CONFIG_ZMK_BLE_CLEAR_BONDS_ON_START=y\nCONFIG_SETTINGS=y\nCONFIG_SETTINGS_NONE=y\n"),
    "transport-absent-zero": (ZERO, CONF + "CONFIG_ZMK_BLE_CLEAR_BONDS_ON_START=y\nCONFIG_SETTINGS=y\nCONFIG_SETTINGS_NONE=y\n"),
    "transport-three": (THREE, CONF), "transport-three-zero": (THREE + ZERO, CONF),
    "late-global": ('''&rgb_matrix { compatible = "test,rgb-split-late-global",
        "keypaw,rgb-matrix"; zephyr,deferred-init; };''', CONF),
}
for name in ("receiving", "receiving-zero", "receiving-multiword", "receiving-multiword-zero"):
    CONFIGS[name] = ((MULTIWORD if "multiword" in name else "") +
        (ZERO if name.endswith("-zero") else ""),
        "CONFIG_KEYPAW_SIM_SELECTOR_TEST=n\nCONFIG_KEYPAW_SIM_SPLIT_TEST=y\n"
        "CONFIG_TEST=y\nCONFIG_IRQ_OFFLOAD=y\nCONFIG_ZMK_SPLIT=y\nCONFIG_ZMK_SPLIT_ROLE_CENTRAL=n\n")


def supported(name):
    if name == "ordering-settings":
        return SETTINGS_SCENARIOS
    if name.startswith("ordering"):
        return ORDERING_SCENARIOS
    if name.startswith("receiving"):
        return ("receiving",)
    if "multiword-control" in name:
        return ("multiword-control",)
    if "multiword" in name:
        return MULTIWORD_SCENARIOS + tuple(s for s in SCENARIOS if s != "no-transport")
    if name.startswith("brightness"):
        return ("retry-color",) + tuple(s for s in SWEEP_SCENARIOS if int(s[-1]) < 8)
    if name == "late-global":
        return (name,)
    if "absent" in name:
        return ("no-transport",)
    if "settling" in name:
        return ("ordering-request-settling",)
    return tuple(s for s in SCENARIOS if s != "no-transport") + tuple(
        s for s in SWEEP_SCENARIOS if "three" in name or int(s[-1]) < 8)


def runtime(root, name, scenarios, repeat=1, no_build=False):
    fragment, config = CONFIGS[name]
    peripheral = name.startswith("receiving")
    if not no_build:
        result = build(root, name, '''/ { chosen { zephyr,bt-hci = &test_hci; };
            test_hci: bt_hci { compatible = "zephyr,bt-hci-userchan"; }; };''' + COMPACT_PREVIEW + fragment,
                       runtime=True, selectors=ORDERING_SELECTORS if name.startswith("ordering") else SOLID_SELECTORS, extra_conf=config)
        assert result.returncode == 0, result.stdout[-20000:]
    assert (root / name / "build/zephyr/zmk.exe").is_file()
    for scenario in scenarios * repeat:
        marker = f"PASS split {scenario}"
        print("COMMAND", root / name / "build/zephyr/zmk.exe", "-no-rt",
              f"--split-scenario={scenario}", "--stop_at=40", flush=True)
        result = run(root, name, scenario, arguments=("--stop_at=40",), timeout=50,
                     marker=marker, scenario_option="split-scenario")
        assert marker in result.stdout, result.stdout
        errors = [line for line in result.stdout.splitlines() if "<err>" in line]
        assert all("HCI driver is not ready" in line or "BLUETOOTH FAILED (-19)" in line or
                   (peripheral and "Failed to invoke behavior" in line)
                   for line in errors), result.stdout
        print(f"PASS split native {name}/{scenario}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario", choices=MULTIWORD_SCENARIOS + SCENARIOS + ORDERING_SCENARIOS + SWEEP_SCENARIOS + SETTINGS_SCENARIOS + ("receiving", "late-global", "multiword-control"), action="append")
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
