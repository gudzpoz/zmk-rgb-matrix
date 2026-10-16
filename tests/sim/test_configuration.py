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
import tempfile

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
    "zero-local-leds": (True, '&rgb_matrix { /delete-property/ strip; mapping = <>; };'),
    "period-zero": (True, '&preview_selector { initial-duration-ms = <0>; }; &fx_solid { duration = <0>; };'),
    "period-wide": (True, '''
&preview_selector { initial-duration-ms = <70000>; };
&fx_solid { duration = <70000>; };
&fixture_overlay { fx_fixture { duration = <70000>; }; };
'''),
    "period-u32-max": (True, '''
&preview_selector { initial-duration-ms = <0xffffffff>; };
&fx_solid { duration = <0xffffffff>; };
&fixture_overlay { fx_fixture { duration = <0xffffffff>; }; };
'''),
    "external-effect": (True, '''
&preview_selector { external_example { compatible = "keypaw,rgb-matrix-example";
    #binding-cells = <0>; }; };
'''),
    "arbitrary-effect-parent": (True, '''
/ { arbitrary_effect_parent { nested_effect { compatible = "keypaw,rgb-matrix-rainbow";
    #binding-cells = <0>; duration = <321>; }; }; };
'''),
    "two-child-overlay": (True, '''
&rgb_matrix { fixture_pair: fixture_pair {
    compatible = "keypaw,rgb-overlay-pair";
    condition = <&fixture_always>;
    all-leds;
    first { compatible = "keypaw,rgb-matrix-reactive"; #binding-cells = <0>;
        color = <0x20ff40>; duration = <700>; };
    second { compatible = "keypaw,rgb-matrix-rainbow"; #binding-cells = <0>;
        duration = <1800>; };
}; };
'''),
    "two-child-private-parameters": (True, '''
&rgb_matrix { fixture_pair: fixture_pair {
    compatible = "keypaw,rgb-overlay-pair";
    all-leds;
    first { compatible = "keypaw,rgb-matrix-reactive"; #binding-cells = <0>;
        color = <0xa040ff>; duration = <900>; };
    second { compatible = "keypaw,rgb-matrix-rainbow"; #binding-cells = <0>;
        duration = <2300>; };
}; };
'''),
    "no-controller": (False, '&rgb_matrix { status = "disabled"; };'),
    "two-controllers": (False, '''
/ { extra_rgb { compatible = "keypaw,rgb-matrix"; #binding-cells = <2>;
    physical-layout = <&layout_ortho_4x12_all1u>; mapping = <>; }; };
'''),
    "empty-scene": (True, '''&rgb_matrix {
    /delete-node/ preview_selector; /delete-node/ fixture_overlay;
    /delete-node/ fixture_battery; /delete-node/ fixture_batt_sub;
}; / { behaviors { /delete-node/ rgbs; }; };'''),
    "overlay-wrong-parent": (False, '''/ { wrong_parent { bad_overlay {
    compatible = "keypaw,rgb-overlay-battery"; all-leds;
}; }; };'''),
    "missing-binding-cells": (False, '&rgb_matrix { /delete-property/ #binding-cells; };'),
    "removed-global-alias": (False, '&rgb_matrix { selector = <&preview_selector>; };'),
    "controller-leds": (False, "&rgb_matrix { leds = <0 1>; };"),
    "flat-controller-child": (False, '''&rgb_matrix { flat_effect {
        compatible = "keypaw,rgb-matrix-solid"; #binding-cells = <0>;
    }; };'''),
    "removed-overlays": (False, '&fx_solid { overlays = <&fixture_overlay>; };'),
    "removed-no-overlays": (False, '&fx_solid { no-overlays; };'),
    "removed-private-overlays": (False, '&fixture_overlay { fx_fixture { overlays = <&fixture_battery>; }; };'),
    "removed-private-no-overlays": (False, '&fixture_overlay { fx_fixture { no-overlays; }; };'),
    "matrix-global-default": (True, '&rgb_matrix { initial-brightness = <42>; };'),
    "removed-flat-default": (False, '&rgb_matrix { initial-effect = <0>; };'),
    "removed-no-cycle": (False, "&fx_solid { no-cycle; };"),
    "effect-reference": (False, "&fixture_overlay { effect = <&fx_solid>; };"),
    "missing-child": (False, "&fixture_overlay { /delete-node/ fx_fixture; };"),
    "two-enabled-children": (False, '''
&fixture_overlay { second { compatible = "keypaw,rgb-matrix-solid";
    #binding-cells = <0>; }; };
'''),
    "disabled-sibling": (True, '''
&fixture_overlay { disabled { compatible = "keypaw,rgb-matrix-solid";
    #binding-cells = <0>; status = "disabled"; }; };
'''),
    "shared-selection-gate": (True, '''
&rgb_conditions { selected_preview: selected_preview {
    compatible = "keypaw,rgb-condition-effect-selected";
    selector = <&preview_selector>; effect = <&fx_solid>;
}; };
&fixture_overlay { condition = <&selected_preview>; };
&fixture_battery { condition = <&selected_preview>; };
'''),
    "removed-selector-persist-id": (False, '&preview_selector { persist-id = "preview"; };'),
    "removed-effect-persist-id": (False, '&fx_solid { persist-id = "solid"; };'),
}


DIAGNOSTICS = {
    "startup-priority-99": r"RGB automatic control startup must follow matrix initialization",
    "no-controller": r"DT_HAS_KEYPAW_RGB_MATRIX_ENABLED.*\(=n\)",
    "two-controllers": r"exactly one enabled matrix node",
    "overlay-wrong-parent": r"overlay must be a child of the keypaw,rgb-matrix node",
    "missing-binding-cells": r"#binding-cells.*required|required.*#binding-cells",
    "removed-global-alias": r"['\"]selector['\"].*not declared",
    "controller-leds": r"whole strip|['\"]leds['\"].*not declared",
    "flat-controller-child": r"matrix children must be overlays, not effects",
    "removed-overlays": r"['\"]overlays['\"].*not declared|properties have been removed",
    "removed-no-overlays": r"['\"]no-overlays['\"].*not declared|properties have been removed",
    "removed-private-overlays": r"['\"]overlays['\"].*not declared|properties have been removed",
    "removed-private-no-overlays": r"['\"]no-overlays['\"].*not declared|properties have been removed",
    "removed-flat-default": r"['\"]initial-effect['\"].*not declared|defaults moved",
    "removed-no-cycle": r"['\"]no-cycle['\"].*not declared",
    "effect-reference": r"references are not supported|['\"]effect['\"].*not declared",
    "missing-child": r"exactly one enabled nested effect",
    "two-enabled-children": r"exactly one enabled nested effect",
    "removed-selector-persist-id": r"['\"]persist-id['\"].*not declared in.*binding",
    "removed-effect-persist-id": r"['\"]persist-id['\"].*not declared in.*binding",
}

for effect in ("rain", "starlight"):
    for interval in (0, -1, 65536, 1, 65535):
        name = f"{effect}-interval-{interval}"
        CASES[name] = (1 <= interval <= 65535, f'''
&preview_selector {{ interval_test {{ compatible = "keypaw,rgb-matrix-{effect}";
    #binding-cells = <0>; step-interval-ms = <({interval})>; }}; }};
''')
        DIAGNOSTICS[name] = rf"{effect} step-interval-ms must be in 1\.\.65535"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=CASES, action="append")
    parser.add_argument("--build-dir", type=Path)
    args = parser.parse_args()
    root = (args.build_dir or Path(tempfile.mkdtemp(prefix="rgb-config-"))).resolve()
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
        if name.startswith("period-"):
            assert not re.search(r"warning:[^\n]*(overflow|changes value)", result.stdout), result.stdout[-12000:]
        if not valid:
            output = re.sub(r"\x1b\[[0-9;]*m", "", result.stdout)
            assert re.search(DIAGNOSTICS[name], output), output[-12000:]
        print(f"PASS {name}", flush=True)


if __name__ == "__main__":
    main()
