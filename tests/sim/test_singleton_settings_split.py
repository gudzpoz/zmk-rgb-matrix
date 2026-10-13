#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Selector-free native settings and two-word global split regressions."""
import argparse
from pathlib import Path
import tempfile

from native_build import build, run
from native_protocol import SOLID_SELECTORS, CONF

SINGLETON = SOLID_SELECTORS + '''
&rgb_overlays { /delete-node/ selector_a; /delete-node/ selector_b; };
/ { behaviors { /delete-node/ rgbs_a; /delete-node/ rgbs_b; }; };
&kprgb { /delete-property/ selector; };
'''
MASKS = (0x1234, 0xabcd)
MASK_SCENE = '''
&rgb_conditions { singleton_closed: singleton_closed { compatible = "keypaw,rgb-condition-latch"; }; };
&ordinary { condition = <&singleton_closed>; };
&rgb_overlays {
''' + "\n".join(f'''
    singleton_mask_{index}: singleton_mask_{index} {{
        compatible = "keypaw,rgb-overlay";
        all-leds;
        condition = <&{"fixture_always" if MASKS[index // 16] & (1 << (index % 16)) else "singleton_closed"}>;
        mask_fx_{index} {{ compatible = "keypaw,rgb-matrix-solid"; #binding-cells = <0>; }};
    }};
''' for index in range(1, 32)) + "};\n"
SINGLETON_CONF = CONF + "CONFIG_KEYPAW_SIM_SINGLETON_SPLIT_TEST=y\n"

CONFIGS = {
    "settings": "CONFIG_KEYPAW_SIM_SETTINGS_TEST=y\nCONFIG_SETTINGS=y\nCONFIG_SETTINGS_CUSTOM=y\nCONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE=20\n",
    "settings-disabled": "CONFIG_KEYPAW_SIM_SETTINGS_TEST=y\nCONFIG_SETTINGS=n\n",
    "split": SINGLETON_CONF,
    "split-zero": SINGLETON_CONF,
    "split-unready": SINGLETON_CONF + "CONFIG_KEYPAW_RGB_CONTROL_AUTO_START=n\n",
}
PHASES = ("brightness", "power", "mask-0", "mask-1")
SPLIT_SCENARIOS = (
    "brightness", "power", "overlay", "overlay-1",
    "race-brightness", "race-power", "race-overlay", "race-overlay-1",
    *(f"revision-{phase}" for phase in (*PHASES, "complete")),
    *(f"revision-failed-{phase}" for phase in PHASES),
    *(f"frozen-{phase}" for phase in PHASES),
    "query-error",
)
SCENARIOS = {
    "settings": ("existing", "validation", "rejection-current-first", "rejection-legacy-first",
                 "failure", "dirty", "reset", "failed-dirty", "failed-reset", "debounce", "staged-reset"),
    "settings-disabled": ("disabled",),
    "split": SPLIT_SCENARIOS,
    "split-zero": SPLIT_SCENARIOS,
    "split-unready": ("unready",),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", choices=CONFIGS, action="append")
    parser.add_argument("--scenario", choices=sorted({s for v in SCENARIOS.values() for s in v}), action="append")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--artifacts", type=Path)
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("--repeat must be positive")
    if args.no_build and args.artifacts is None:
        parser.error("--no-build requires --artifacts")
    configurations = tuple(args.config or CONFIGS)
    if args.scenario:
        unsupported = set(args.scenario) - {s for c in configurations for s in SCENARIOS[c]}
        if unsupported:
            parser.error(f"No selected configuration supports scenarios: {sorted(unsupported)}")
    root = (args.artifacts or Path(tempfile.mkdtemp(prefix="rgb-singleton-settings-split-"))).resolve()
    print(f"ARTIFACTS {root}", flush=True)
    for name in configurations:
        scenarios = tuple(s for s in SCENARIOS[name] if not args.scenario or s in args.scenario)
        if not scenarios:
            print(f"SKIP {name}: requested scenarios not supported", flush=True)
            continue
        split = name.startswith("split")
        fragment = '''/ { chosen { zephyr,bt-hci = &test_hci; };
            test_hci: bt_hci { compatible = "zephyr,bt-hci-userchan"; }; };''' if split else ""
        if split:
            fragment += MASK_SCENE
        if name == "split-zero":
            fragment += '&rgb_matrix { /delete-property/ strip; mapping = <>; }; &ordinary { /delete-property/ leds; };'
        elif name == "split-unready":
            fragment += '''&kprgb {
                compatible = "test,rgb-global-late-control", "keypaw,behavior-rgb-matrix";
                zephyr,deferred-init;
            };'''
        if not args.no_build:
            result = build(root, name, fragment, selectors=SINGLETON,
                           extra_conf=CONFIGS[name] + "CONFIG_KEYPAW_SIM_SELECTOR_TEST=n\n")
            assert result.returncode == 0, result.stdout[-20000:]
        for iteration in range(args.repeat):
            for scenario in scenarios:
                kind = "split" if split else "settings"
                run(root, name, scenario, arguments=("--stop_at=20",),
                    scenario_option=f"{kind}-scenario", marker=f"PASS singleton {kind} {scenario}")
                print(f"PASS native singleton {name}/{scenario} iteration={iteration + 1}", flush=True)


if __name__ == "__main__":
    main()
