#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Build real native_sim devicetrees to exercise ownership validation.

Requires the same workspace/toolchain as run-smoke.sh. Every case has a pristine
build directory; positive controls distinguish rejection from a broken toolchain.
"""
import argparse
import os
import re
from pathlib import Path
import subprocess

from native_build import native_build_root

SIM = Path(__file__).resolve().parent
MODULE = SIM.parent.parent
REPO = MODULE.parent.parent
WORKSPACE = Path(os.environ.get("ZMK_WS", REPO / "zmk"))
LOCAL_WEST = REPO / ".venv/bin/west"
WEST = os.environ.get("WEST", str(LOCAL_WEST) if LOCAL_WEST.is_file() else "west")
EXAMPLE = Path(os.environ.get("RGB_EXAMPLE_MODULE", MODULE.parent / "zmk-rgb-effect-example"))

CASES = {
    "baseline": (True, ""),
    "startup-priority-98": (True, ""),
    "startup-priority-99": (False, ""),
    # The separately owned example binding still requires this property.
    "external-effect": (True, '''
&preview_selector { external_example { compatible = "keypaw,rgb-matrix-example";
    #binding-cells = <0>; }; };
'''),
    "arbitrary-effect-parent": (True, '''
/ { arbitrary_effect_parent { nested_effect { compatible = "keypaw,rgb-matrix-rainbow";
    duration = <321>; }; }; };
'''),
    "no-controller": (False, '&rgb_matrix { status = "disabled"; };'),
    "two-controllers": (False, '''
/ { extra_rgb { compatible = "keypaw,rgb-matrix"; #binding-cells = <2>;
    physical-layout = <&layout_ortho_4x12_all1u>; mapping = <>; }; };
'''),
    "overlay-wrong-parent": (False, '''/ { wrong_parent { bad_overlay {
    compatible = "keypaw,rgb-overlay-battery"; all-leds;
}; }; };'''),
    "missing-binding-cells": (False, '&rgb_matrix { /delete-property/ #binding-cells; };'),
    "controller-leds": (False, "&rgb_matrix { leds = <0 1>; };"),
    "flat-controller-child": (False, '''&rgb_matrix { flat_effect {
        compatible = "keypaw,rgb-matrix-solid";
    }; };'''),
    "effect-reference": (False, "&fixture_overlay { effect = <&fx_solid>; };"),
    "missing-child": (False, "&fixture_overlay { /delete-node/ fx_fixture; };"),
    "two-enabled-children": (False, '''
&fixture_overlay { second { compatible = "keypaw,rgb-matrix-solid";
    }; };
'''),
    "disabled-sibling": (True, '''
&fixture_overlay { disabled { compatible = "keypaw,rgb-matrix-solid";
    status = "disabled"; }; };
'''),
    "shared-selection-gate": (True, '''
&rgb_conditions { selected_preview: selected_preview {
    compatible = "keypaw,rgb-condition-effect-selected";
    selector = <&preview_selector>; effect = <&fx_solid>;
}; };
&fixture_overlay { condition = <&selected_preview>; };
&fixture_battery { condition = <&selected_preview>; };
'''),
    "heatmap-spread-zero": (True, '''
&preview_selector { heatmap_zero { compatible = "keypaw,rgb-matrix-heatmap";
    spread = <0>; }; };
'''),
    # Zephyr permits undeclared #-properties; this does not register a behavior.
    "inert-effect-binding-cells": (True, '&fx_solid { #binding-cells = <0>; };'),
    "direct-effect-keymap-binding": (False, ""),
    "triggers-present": (True, '''/ { stage14_triggers {
        compatible = "keypaw,rgb-trigger-table";
        edge { condition = <&fixture_latch>; on-enter = <&none>; };
    }; };'''),
    "triggers-disabled": (True, '''/ { stage14_triggers {
        compatible = "keypaw,rgb-trigger-table"; status = "disabled";
        edge { condition = <&fixture_latch>; on-enter = <&none>; };
    }; };'''),
    "advanced-overrides": (True, ""),
}


DIAGNOSTICS = {
    "startup-priority-99": r"RGB automatic control startup must follow matrix initialization",
    "no-controller": r"DT_HAS_KEYPAW_RGB_MATRIX_ENABLED.*\(=n\)",
    "two-controllers": r"exactly one enabled matrix node",
    "overlay-wrong-parent": r"overlay must be a child of the keypaw,rgb-matrix node",
    "missing-binding-cells": r"#binding-cells.*required|required.*#binding-cells",
    "controller-leds": r"['\"]leds['\"].*not declared",
    "flat-controller-child": r"matrix children must be overlays, not effects",
    "effect-reference": r"['\"]effect['\"].*not declared",
    "missing-child": r"exactly one enabled nested effect",
    "two-enabled-children": r"exactly one enabled nested effect",
    "direct-effect-keymap-binding": r"fx_reactive.*lacks #binding-cells",
}

SOURCE_CASES = {
    'endpoint-any': (True, 'endpoint', 'transport = "any";', ''),
    'endpoint-profile-low': (False, 'endpoint', 'transport = "any"; profile = <(-2)>;', 'endpoint profile must be >= -1'),
    'endpoint-profile-transport': (False, 'endpoint', 'transport = "any"; profile = <0>;', 'endpoint profile requires BLE'),
    'endpoint-open-transport': (False, 'endpoint', 'transport = "any"; state = "open";', 'endpoint open/any requires BLE'),
    'endpoint-any-transport': (False, 'endpoint', 'transport = "any"; state = "any";', 'endpoint open/any requires BLE'),
    'endpoint-powered-transport': (False, 'endpoint', 'transport = "any"; state = "powered";', 'endpoint powered requires USB'),
    'endpoint-ignore-any': (False, 'endpoint', 'transport = "any"; ignore-routing;', 'endpoint ignore-routing requires BLE or USB connected'),
    'endpoint-ignore-powered': (False, 'endpoint', 'transport = "usb"; state = "powered"; ignore-routing;', 'endpoint ignore-routing requires BLE or USB connected'),
    'endpoint-ignore-disconnected': (False, 'endpoint', 'transport = "usb"; state = "disconnected"; ignore-routing;', 'endpoint ignore-routing requires BLE or USB connected'),
    'endpoint-no-ble': (False, 'endpoint', '', 'endpoint BLE requires CONFIG_ZMK_BLE on host side'),
    'endpoint-no-usb': (False, 'endpoint', 'transport = "usb";', 'endpoint USB requires CONFIG_USB_DEVICE_STACK on host side'),
    'battery-below-low': (False, 'battery', 'below = <0>;', 'battery below must be in 1..101'),
    'battery-below-high': (False, 'battery', 'below = <102>;', 'battery below must be in 1..101'),
    'battery-above-low': (False, 'battery', 'above = <(-2)>;', 'battery above must be in -1..99'),
    'battery-above-high': (False, 'battery', 'above = <100>;', 'battery above must be in -1..99'),
    'battery-empty': (False, 'battery', 'above = <19>; below = <20>;', 'battery threshold interval must not be empty'),
    'battery-unconstrained': (False, 'battery', '', 'battery condition must constrain SOC or power'),
    'battery-no-reporting': (False, 'battery', 'below = <20>;', 'battery SOC requires CONFIG_ZMK_BATTERY_REPORTING'),
    'battery-no-usb': (False, 'battery', 'state = "charging";', 'battery power requires CONFIG_USB_DEVICE_STACK'),
}
for name, (valid, kind, properties, diagnostic) in SOURCE_CASES.items():
    CASES[name] = (valid, f'&rgb_conditions {{ source_test {{ compatible = "keypaw,rgb-condition-{kind}"; {properties} }}; }};')
    DIAGNOSTICS[name] = re.escape(diagnostic)

CASES['battery-hue-ignores-bar-properties'] = (True, '&fixture_battery { mode = "hue"; color = <0x123456>; background-color = <0xabcdef>; reverse; };')
CASES['battery-bar-ignores-hue-properties'] = (True, '&fixture_battery { hue-empty = <359>; hue-full = <0>; };')
CASES['battery-hue-only'] = (True, '&fixture_batt_sub { status = "disabled"; }; &fixture_battery { mode = "hue"; };')
for prop, value in (('hue-empty', -1), ('hue-full', 360)):
    name = f'battery-{prop}-invalid'
    CASES[name] = (False, f'&fixture_battery {{ {prop} = <({value})>; }};')
    DIAGNOSTICS[name] = r'battery hue endpoints must be in 0\.\.359'

for prop in ("max-brightness", "idle-brightness", "initial-brightness"):
    for cap in (-1, 0, 100, 101):
        name = f"{prop}-{cap}"
        CASES[name] = (0 <= cap <= 100, f'&rgb_matrix {{ {prop} = <({cap})>; }};')
        DIAGNOSTICS[name] = rf'{prop} must be in 0\.\.100'

for opacity in (-1, 0, 100, 101, 256):
    name = f"overlay-opacity-{opacity}"
    CASES[name] = (0 <= opacity <= 100,
                   f'&fixture_overlay {{ opacity = <({opacity})>; }};')
    DIAGNOSTICS[name] = r"overlay opacity must be 0\.\.100"

for radius in (-1, 0, 65535, 65536):
    name = f"reactive-radius-{radius}"
    CASES[name] = (0 <= radius <= 65535,
                   f'&fx_reactive {{ spread-radius = <({radius})>; }};')
    DIAGNOSTICS[name] = r"reactive spread-radius must be in 0\.\.65535"


for layer in (-1, 0, 255, 256):
    name = f"layer-condition-{layer}"
    CASES[name] = (0 <= layer <= 255,
                   f"&fixture_layer {{ layer = <({layer})>; }};")
    DIAGNOSTICS[name] = r"layer condition layer must be in 0\.\.255"

for effect in ("rain", "starlight"):
    for interval in (0, -1, 65536, 1, 65535):
        name = f"{effect}-interval-{interval}"
        CASES[name] = (1 <= interval <= 65535, f'''
&preview_selector {{ interval_test {{ compatible = "keypaw,rgb-matrix-{effect}";
    step-interval-ms = <({interval})>; }}; }};
''')
        DIAGNOSTICS[name] = rf"{effect} step-interval-ms must be in 1\.\.65535"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=CASES, action="append")
    parser.add_argument("--build-dir", type=Path)
    args = parser.parse_args()
    root = native_build_root(args.build_dir, "configuration", prefix="rgb-config-")
    print(f"ARTIFACTS {root}", flush=True)
    for name in args.case or CASES:
        valid, fragment = CASES[name]
        if name == "external-effect" and not EXAMPLE.is_dir():
            print(f"SKIP external-effect: {EXAMPLE} is not available", flush=True)
            continue
        modules = [str(MODULE), str(SIM / "module")]
        if name == "external-effect":
            modules.append(str(EXAMPLE))
        config = root / name / "config"
        config.mkdir(parents=True, exist_ok=True)
        for filename in ("native_sim.conf", "native_sim.keymap"):
            text = (SIM / "config" / filename).read_text()
            if filename.endswith(".keymap") and name == "direct-effect-keymap-binding":
                text = text.replace("                &none", "                &fx_reactive", 1)
            if filename.endswith(".conf") and name == "advanced-overrides":
                text += ("\nCONFIG_KEYPAW_RGB_MATRIX_TICK_MS=17\n"
                         "CONFIG_KEYPAW_RGB_WORKQUEUE_STACK_SIZE=3072\n"
                         "CONFIG_KEYPAW_RGB_WORKQUEUE_PRIORITY=11\n"
                         "CONFIG_KEYPAW_RGB_MATRIX_HUE_STEP=13\n"
                         "CONFIG_KEYPAW_RGB_MATRIX_SAT_STEP=12\n"
                         "CONFIG_KEYPAW_RGB_MATRIX_BRT_STEP=11\n"
                         "CONFIG_KEYPAW_RGB_MATRIX_DURATION_STEP=123\n"
                         "CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS=120\n"
                         "CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS=9000\n")
            if filename.endswith(".conf") and name == "no-controller":
                text += "\nCONFIG_KEYPAW_RGB_MATRIX=y\n"
            if filename.endswith(".conf") and name.startswith("startup-priority-"):
                text += f"\nCONFIG_SETTINGS=n\nCONFIG_APPLICATION_INIT_PRIORITY={name.rsplit('-', 1)[1]}\n"
            (config / filename).write_text(text)
        (config / "native_sim.overlay").write_text(
            f'#include "{SIM / "config/native_sim.overlay"}"\n' + fragment
        )
        command = [WEST, "build", "-s", "app", "-d", str(root / name / "build"),
                   "-b", "native_sim//zmk_test_mock", "-p", "--",
                   "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                   f"-DZMK_CONFIG={config}",
                   f"-DZMK_EXTRA_MODULES={';'.join(modules)}"]
        result = subprocess.run(command, cwd=WORKSPACE, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
        (root / f"{name}.log").write_text(result.stdout)
        if (result.returncode == 0) != valid:
            raise AssertionError(f"{name}: expected {'acceptance' if valid else 'rejection'}\n"
                                 + result.stdout[-12000:])
        if valid:
            generated = (root / name / "build/zephyr/.config").read_text()
            assert "CONFIG_CHECK_INIT_PRIORITIES=y" in generated
            assert "CONFIG_KEYPAW_RGB_SPLIT_SYNC=y" not in generated, generated
            assert ("CONFIG_KEYPAW_RGB_TRIGGERS=y" in generated) == (name == "triggers-present"), generated
            if name == "advanced-overrides":
                for symbol, value in (("MATRIX_TICK_MS", 17), ("WORKQUEUE_STACK_SIZE", 3072),
                                      ("WORKQUEUE_PRIORITY", 11), ("MATRIX_HUE_STEP", 13),
                                      ("MATRIX_SAT_STEP", 12), ("MATRIX_BRT_STEP", 11),
                                      ("MATRIX_DURATION_STEP", 123), ("MATRIX_DURATION_MIN_MS", 120),
                                      ("MATRIX_DURATION_MAX_MS", 9000)):
                    assert f"CONFIG_KEYPAW_RGB_{symbol}={value}\n" in generated, generated
        if not valid:
            output = re.sub(r"\x1b\[[0-9;]*m", "", result.stdout)
            assert re.search(DIAGNOSTICS[name], output), output[-12000:]
        print(f"PASS {name}", flush=True)


if __name__ == "__main__":
    main()
