#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Selector-free native settings and whole-mask global split regressions."""
import argparse
from pathlib import Path
import tempfile

from native_build import build, run
from native_protocol import SOLID_SELECTORS, CONF

SINGLETON = SOLID_SELECTORS + '''
&rgb_matrix { /delete-node/ selector_a; /delete-node/ selector_b; };
'''
MASKS = (0xabcd1234, 0x80000001, 1)


def mask_scene(count):
    return '''
&rgb_conditions { singleton_closed: singleton_closed { compatible = "keypaw,rgb-condition-latch"; }; };
&ordinary { condition = <&singleton_closed>; };
&rgb_matrix {
''' + "\n".join(f'''
    singleton_mask_{index}: singleton_mask_{index} {{
        compatible = "keypaw,rgb-overlay";
        all-leds;
        {"" if MASKS[index // 32] & (1 << (index % 32)) else "condition = <&singleton_closed>;"}
        mask_fx_{index} {{ compatible = "keypaw,rgb-matrix-solid"; }};
    }};
''' for index in range(1, count)) + "};\n"


MASK_SCENE = mask_scene(32)
MULTIWORD_SCENE = mask_scene(65)
SINGLETON_CONF = CONF + "CONFIG_KEYPAW_SIM_SINGLETON_SPLIT_TEST=y\n"

CONFIGS = {
    "settings": "CONFIG_KEYPAW_SIM_SETTINGS_TEST=y\nCONFIG_SETTINGS=y\nCONFIG_SETTINGS_CUSTOM=y\nCONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE=20\n",
    "settings-disabled": "CONFIG_KEYPAW_SIM_SETTINGS_TEST=y\nCONFIG_SETTINGS=n\n",
    "split": SINGLETON_CONF,
    "split-zero": SINGLETON_CONF,
    "split-unready": SINGLETON_CONF,
    "split-multiword": SINGLETON_CONF,
    "split-multiword-zero": SINGLETON_CONF,
}
SPLIT_SCENARIOS = (
    "brightness", "power", "overlay",
    "race-brightness", "race-power", "race-gate",
    *(f"current-retry-{phase}" for phase in ("brightness", "power", "gate")),
    "persistent-failure", "global-isolation", "query-error",
)
SCENARIOS = {
    "settings": ("existing", "validation", "rejection-current-first", "rejection-legacy-first",
                 "failure", "dirty", "reset", "failed-dirty", "failed-reset", "debounce",
                 "staged-reset", "repeated-commits"),
    "settings-disabled": ("disabled",),
    "split": SPLIT_SCENARIOS,
    "split-zero": SPLIT_SCENARIOS,
    "split-unready": ("unready",),
    "split-multiword": SPLIT_SCENARIOS + ("current-retry-later", "current-retry-final", "race-later", "race-final"),
    "split-multiword-zero": SPLIT_SCENARIOS + ("current-retry-later", "current-retry-final", "race-later", "race-final"),
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
            fragment += MULTIWORD_SCENE if "multiword" in name else MASK_SCENE
        if name.endswith("-zero"):
            fragment += '&rgb_matrix { /delete-property/ strip; mapping = <>; }; &ordinary { /delete-property/ leds; };'
        elif name == "split-unready":
            fragment += '''&rgb_matrix {
                compatible = "test,rgb-global-late-control", "keypaw,rgb-matrix";
                zephyr,deferred-init;
            };'''
        if not args.no_build:
            result = build(root, name, fragment, selectors=SINGLETON,
                           extra_conf=CONFIGS[name] + "CONFIG_KEYPAW_SIM_SELECTOR_TEST=n\n")
            assert result.returncode == 0, result.stdout[-20000:]
        for iteration in range(args.repeat):
            for scenario in scenarios:
                kind = "split" if split else "settings"
                result = run(root, name, scenario, arguments=("--stop_at=20",),
                             scenario_option=f"{kind}-scenario", marker=f"PASS singleton {kind} {scenario}")
                if name == "settings" and scenario in ("failure", "failed-dirty", "failed-reset"):
                    assert "RGB settings save failed" in result.stdout, result.stdout
                print(f"PASS native singleton {name}/{scenario} iteration={iteration + 1}", flush=True)


if __name__ == "__main__":
    main()
