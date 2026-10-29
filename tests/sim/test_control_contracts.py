#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Real Zephyr control contracts; each scenario starts a fresh firmware process."""
import argparse
from pathlib import Path

from native_build import build, run, SIM, SELECTORS, COMPACT_PREVIEW, native_build_root

SCENARIOS = {
    "control-early": "controls",
    "control-registry": "controls",
    "control-one-child": "one-child",
    "control-parameters": "controls",
    "control-global-defaults": "controls",
    "control-noop": "controls",
    "control-peripheral": "peripheral",
    "control-conversion": "controls",
    "control-conversion-metadata-off": "controls-metadata-off",
    "control-validation": "controls",
    "control-canonical-validation": "controls",
    "control-normalize-commands": "controls",
    "control-global-commands": "controls",
    "control-overlay-words": "overlay-words",
    "control-overlay-words-peripheral": "overlay-words-peripheral",
    "control-reset": "controls",
    "control-reset-fallback-noop": "reset-fallback",
    "control-reset-fallback-notify": "reset-fallback",
    "control-reset-prestart": "reset-fallback",
    "control-isr": "controls",
    "control-large-step": "large-step",
    "control-local-only": "local-only",
    "control-owner": "owner",
    "control-duration-defaults": "duration-defaults",
    "control-matrix-default": "matrix-default",
    "control-matrix-custom": "matrix-custom",
}
CONF = "\nCONFIG_KEYPAW_SIM_CONTROL_CONTRACTS=y\nCONFIG_TEST=y\nCONFIG_IRQ_OFFLOAD=y\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--scenario", choices=SCENARIOS, action="append")
    parser.add_argument("--repeat", type=int, default=1)
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("--repeat must be positive")
    root = native_build_root(args.build_dir, "control-contracts", prefix="rgb-control-contracts-")
    print(f"ARTIFACTS {root}", flush=True)
    selected = args.scenario or list(SCENARIOS)
    fixture = (SIM / "config/control_contracts.overlay").read_text()
    for image in dict.fromkeys(SCENARIOS[s] for s in selected):
        fragment, selectors, conf = fixture, None, CONF
        if image == "reset-fallback":
            selectors = SELECTORS.replace('''        disabled { compatible = "keypaw,rgb-matrix-solid";
            status = "disabled"; };
        probe_a: a_static { compatible = "keypaw,rgb-matrix-selector-probe";
            color = <0x00ff00>; };
        probe_c: a_motion { compatible = "keypaw,rgb-matrix-selector-probe";
            color = <0x0000ff>; animate; };''', '''        probe_a: a_static { compatible = "keypaw,rgb-matrix-selector-probe";
            color = <0x00ff00>; };
        disabled { compatible = "keypaw,rgb-matrix-solid";
            status = "disabled"; };
        probe_c: a_motion { compatible = "keypaw,rgb-matrix-selector-probe";
            color = <0x0000ff>; animate; };''')
            fragment += "&selector_a { initial-effect = <99>; };"
        if image == "controls-metadata-off":
            conf += "CONFIG_ZMK_BEHAVIOR_METADATA=n\n"
            fragment += "&probe_a { #binding-cells = <0>; };\n"
        elif image == "large-step":
            conf += "CONFIG_KEYPAW_RGB_MATRIX_DURATION_STEP=100000\n"
        elif image == "one-child":
            conf += "CONFIG_SETTINGS=y\nCONFIG_SETTINGS_CUSTOM=y\nCONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE=20\n"
            fragment += '''&rgb_matrix {
                single_control: single_control {
                    compatible = "keypaw,rgb-overlay";
                    controllable; #binding-cells = <2>; label = "onechild";
                    all-leds;
                    single_effect: single_effect { compatible = "keypaw,rgb-matrix-solid"; };
                };
            }; &ordinary { #binding-cells = <2>; };'''
        elif image == "duration-defaults":
            fragment += "&probe_a { duration = <0>; }; &probe_c { duration = <66236>; }; &probe_b { duration = <0xffffffff>; };\n"
        elif image == "peripheral":
            conf += "CONFIG_ZMK_SPLIT=y\nCONFIG_ZMK_SPLIT_ROLE_CENTRAL=n\n"
            fragment += "&preview_selector { local; }; &selector_a { local; }; &selector_b { local; };\n"
        elif image == "owner":
            fragment += "&selector_a { opacity = <50>; }; &selector_b { opacity = <50>; };\n"
        elif image == "local-only":
            fragment += COMPACT_PREVIEW
            fragment += '''/ { chosen { zephyr,bt-hci = &control_hci; };
control_hci: control_hci { compatible = "zephyr,bt-hci-userchan"; }; };
'''
            conf += '''CONFIG_SETTINGS=y
CONFIG_SETTINGS_CUSTOM=y
CONFIG_SETTINGS_NONE=n
CONFIG_SETTINGS_NVS=n
CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE=100
CONFIG_ZMK_SPLIT=y
CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y
CONFIG_ZMK_BLE=y
CONFIG_ZMK_SPLIT_BLE=y
CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS=1
CONFIG_BT_MAX_CONN=2
CONFIG_BT_MAX_PAIRED=2
CONFIG_KEYPAW_RGB_SPLIT_SYNC_POLL_MS=25
CONFIG_KEYPAW_RGB_SPLIT_SYNC_SETTLE_MS=30
CONFIG_KEYPAW_RGB_SPLIT_SYNC_CLOCK_MS=1000
'''
        elif image.startswith("matrix-"):
            selectors = ""
            fragment = '''&kscan { events = <>; };
&rgb_matrix {
    /delete-node/ preview_selector;
    /delete-node/ fixture_overlay;
    /delete-node/ fixture_battery;
    /delete-node/ fixture_batt_sub;
};
'''
            fragment += {
                "matrix-default": "&rgb_matrix { /delete-property/ initial-on; /delete-property/ initial-brightness; /delete-property/ max-brightness; /delete-property/ idle-brightness; };",
                "matrix-custom": "&rgb_matrix { /delete-property/ initial-on; initial-brightness = <73>; max-brightness = <91>; idle-brightness = <12>; };",
            }[image]
        if image.startswith("overlay-words"):
            fragment += '&rgb_matrix {\n' + '\n'.join(
                f'word_overlay_{i} {{ compatible = "keypaw,rgb-overlay-battery"; all-leds; }};'
                for i in range(60)) + '\n};\n'
            if image.endswith("peripheral"):
                conf += 'CONFIG_ZMK_SPLIT=y\nCONFIG_ZMK_SPLIT_ROLE_CENTRAL=n\n'
                fragment += '&preview_selector { local; }; &selector_a { local; }; &selector_b { local; };\n'
        result = build(root, image, fragment, selectors=selectors, extra_conf=conf)
        assert result.returncode == 0, result.stdout[-16000:]
        for scenario in selected:
            if SCENARIOS[scenario] == image:
                for _ in range(args.repeat):
                    run(root, image, "control-conversion" if scenario == "control-conversion-metadata-off" else scenario)
                print(f"PASS {scenario} x{args.repeat}", flush=True)


if __name__ == "__main__":
    main()
