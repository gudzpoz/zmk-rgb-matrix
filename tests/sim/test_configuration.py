#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Build real native_sim devicetrees to exercise ownership validation.

Requires the same workspace/toolchain as run-smoke.sh. Every case has a pristine
build directory; positive controls distinguish rejection from a broken toolchain.
"""
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
    "zero-local-leds": (True, '&rgb_matrix { /delete-property/ strip; mapping = <>; };'),
    "period-zero": (True, '&kprgb { initial-duration-ms = <0>; }; &fx_solid { duration = <0>; };'),
    "period-wide": (True, '''
&kprgb { initial-duration-ms = <70000>; };
&fx_solid { duration = <70000>; };
&fixture_overlay { fx_fixture { duration = <70000>; }; };
'''),
    "period-u32-max": (True, '''
&kprgb { initial-duration-ms = <0xffffffff>; };
&fx_solid { duration = <0xffffffff>; };
&fixture_overlay { fx_fixture { duration = <0xffffffff>; }; };
'''),
    "external-effect": (True, '''
&kprgb { external_example { compatible = "keypaw,rgb-matrix-example";
    #binding-cells = <0>; }; };
'''),
    "arbitrary-effect-parent": (True, '''
/ { arbitrary_effect_parent { nested_effect { compatible = "keypaw,rgb-matrix-rainbow";
    #binding-cells = <0>; duration = <321>; }; }; };
'''),
    "two-child-overlay": (True, '''
&rgb_overlays { fixture_pair: fixture_pair {
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
&rgb_overlays { fixture_pair: fixture_pair {
    compatible = "keypaw,rgb-overlay-pair";
    all-leds;
    first { compatible = "keypaw,rgb-matrix-reactive"; #binding-cells = <0>;
        color = <0xa040ff>; duration = <900>; };
    second { compatible = "keypaw,rgb-matrix-rainbow"; #binding-cells = <0>;
        duration = <2300>; };
}; };
'''),
    "no-controller": (False, '&kprgb { status = "disabled"; };'),
    "two-controllers": (False, '''
/ { behaviors { extra_rgb { compatible = "keypaw,behavior-rgb-matrix";
    #binding-cells = <2>;
    fx { compatible = "keypaw,rgb-matrix-solid"; #binding-cells = <0>; };
}; }; };
'''),
    "controller-leds": (False, "&kprgb { leds = <0 1>; };"),
    "removed-no-cycle": (False, "&fx_solid { no-cycle; };"),
    "effect-reference": (False, "&fixture_overlay { effect = <&fx_solid>; };"),
    "missing-child": (False, "&fixture_overlay { /delete-node/ fx_fixture; };"),
    "two-enabled-children": (False, '''
&fixture_overlay { second { compatible = "keypaw,rgb-matrix-solid";
    #binding-cells = <0>; }; };
'''),
    "duplicate-overlay": (False, '''
&fx_solid { overlays = <&fixture_overlay &fixture_overlay>; };
'''),
    "disabled-sibling": (True, '''
&fixture_overlay { disabled { compatible = "keypaw,rgb-matrix-solid";
    #binding-cells = <0>; status = "disabled"; }; };
'''),
    "mutually-exclusive-lists": (True, '''
&fx_solid { overlays = <&fixture_overlay>; };
&fx_solid_v { overlays = <&fixture_overlay>; };
'''),
}


DIAGNOSTICS = {
    "no-controller": r"exactly one enabled controller",
    "two-controllers": r"exactly one enabled controller",
    "controller-leds": r"whole strip|['\"]leds['\"].*not declared",
    "removed-no-cycle": r"['\"]no-cycle['\"].*not declared",
    "effect-reference": r"references are not supported|['\"]effect['\"].*not declared",
    "missing-child": r"exactly one enabled nested effect",
    "two-enabled-children": r"exactly one enabled nested effect",
    "duplicate-overlay": r"redeclaration of enumerator|redefinition of enumerator",
}

for effect in ("rain", "starlight"):
    for interval in (0, -1, 65536, 1, 65535):
        name = f"{effect}-interval-{interval}"
        CASES[name] = (1 <= interval <= 65535, f'''
&kprgb {{ interval_test {{ compatible = "keypaw,rgb-matrix-{effect}";
    #binding-cells = <0>; step-interval-ms = <({interval})>; }}; }};
''')
        DIAGNOSTICS[name] = rf"{effect} step-interval-ms must be in 1\.\.65535"


def main():
    with tempfile.TemporaryDirectory(prefix="rgb-config-") as directory:
        root = Path(directory)
        for name, (valid, fragment) in CASES.items():
            if name == "external-effect" and not EXAMPLE.is_dir():
                print(f"SKIP external-effect: {EXAMPLE} is not available", flush=True)
                continue
            modules = [str(MODULE), str(SIM / "module")]
            if name == "external-effect":
                modules.append(str(EXAMPLE))
            config = root / name / "config"
            config.mkdir(parents=True)
            for filename in ("native_sim.conf", "native_sim.keymap"):
                (config / filename).write_text((SIM / "config" / filename).read_text())
            (config / "native_sim.overlay").write_text(
                f'#include "{SIM / "config/native_sim.overlay"}"\n' + fragment
            )
            command = [WEST, "build", "-s", "app", "-d", str(root / name / "build"),
                       "-b", "native_sim//zmk_test_mock", "-p", "--",
                       f"-DZMK_CONFIG={config}",
                       f"-DZMK_EXTRA_MODULES={';'.join(modules)}"]
            result = subprocess.run(command, cwd=WORKSPACE, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if (result.returncode == 0) != valid:
                raise AssertionError(f"{name}: expected {'acceptance' if valid else 'rejection'}\n"
                                     + result.stdout[-12000:])
            if name.startswith("period-"):
                assert not re.search(r"warning:[^\n]*(overflow|changes value)", result.stdout), result.stdout[-12000:]
            if not valid:
                output = re.sub(r"\x1b\[[0-9;]*m", "", result.stdout)
                assert re.search(DIAGNOSTICS[name], output), output[-12000:]
            print(f"PASS {name}", flush=True)


if __name__ == "__main__":
    main()
