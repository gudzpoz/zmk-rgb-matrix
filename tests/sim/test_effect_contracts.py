#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Native production effect devices, raw dispatch and owner-local contracts."""
import argparse
import os
from pathlib import Path

from native_build import MODULE, SIM, WORKSPACE, build, run, native_build_root

SCENARIOS = CASES = (
    "effects-variants", "effects-targets", "effects-phase", "effects-random",
    "effects-feedback", "effects-instances", "effects-dispatch", "effects-overlay",
    "effects-brightness", "effects-private", "effects-enums", "effects-battery", "effects-spatial",
)
CONF = """
CONFIG_KEYPAW_SIM_EFFECT_CONTRACTS=y
CONFIG_ZMK_BATTERY_REPORTING=y
CONFIG_UBSAN=y
CONFIG_TEST=y
CONFIG_IRQ_OFFLOAD=y
"""


def fragment(zero=False, example=True):
    nodes = []

    def effect(name, kind, properties=""):
        nodes.append(f'{name}: {name} {{ compatible = "keypaw,rgb-matrix-{kind}"; '
                     f'color = <0x00ffff>; duration = <1000>; {properties} }};')

    # Cross products match the old fixture's ordinal arithmetic, not a subset of previews.
    for palette in ("rainbow", "solid"):
        for direction in ("out", "in", "dual", "bloom"):
            for basis in ("x", "y", "radial", "pinwheel", "spiral", "chevron", "flag"):
                effect(f"effects_rbow_{palette}_{direction}_{basis}", "rainbow",
                       f'basis = "{basis}"; direction = "{direction}"; palette = "{palette}";')
    for index in range(8):
        effect(f"effects_star_{index}", "starlight", "step-interval-ms = <32>; " +
               " ".join(f"{prop};" for bit, prop in enumerate(("smooth", "dual-hue", "dual-sat"))
                        if index & (1 << bit)))
    for channel in ("sat", "val"):
        for shape in ("linear", "pinwheel", "spiral"):
            effect(f"effects_band_{channel}_{shape}", "band", f'channel = "{channel}"; shape = "{shape}";')
    for mode in ("brightness", "river", "hue", "pendulum", "wave"):
        effect(f"effects_breathe_{mode}", "breathe", f'mode = "{mode}"; hue-amplitude = <45>;')
    for axis in ("none", "vertical", "horizontal"):
        effect(f"effects_solid_{axis}", "solid", f'axis = "{axis}";')
    for interval in (1, 32, 65535):
        for mode in ("pixel", "flow", "drops", "jellybean", "fractal"):
            effect(f"effects_rain_{mode}_{interval}", "rain",
                   f'mode = "{mode}"; step-interval-ms = <{interval}>;')
        for index in range(8):
            if interval == 32:
                continue
            effect(f"effects_star_{interval}_{index}", "starlight",
                   f'step-interval-ms = <{interval}>; ' +
                   " ".join(f"{prop};" for bit, prop in enumerate(("smooth", "dual-hue", "dual-sat"))
                            if index & (1 << bit)))
    effect("effects_reactive_a", "reactive", 'multi; background-brightness = <0>;')
    effect("effects_reactive_b", "reactive", 'multi; background-brightness = <0>;')
    effect("effects_reactive_disc_zero", "reactive",
           'spread = "disc"; spread-radius = <0>; background-brightness = <0>;')
    effect("effects_reactive_nexus_zero", "reactive",
           'spread = "nexus"; spread-radius = <0>; background-brightness = <0>;')
    effect("effects_heat_default", "heatmap", 'decrease-delay-ms = <25>; increase-step = <32>;')
    effect("effects_heat_explicit_150", "heatmap", 'spread = <150>; decrease-delay-ms = <25>; increase-step = <32>;')
    effect("effects_heat_no_spread", "heatmap", 'spread = <0>; decrease-delay-ms = <25>; increase-step = <32>;')
    effect("effects_heat_zero_delay", "heatmap", 'spread = <0>; decrease-delay-ms = <0>; increase-step = <32>;')
    effect("effects_digital", "digital-rain")
    if example:
        # Required by the separately owned example's unmigrated binding.
        effect("effects_example", "example", '#binding-cells = <0>; tail-length = <200>;')
    nodes += [
        'effects_probe_a: effects_probe_a { compatible = "test,rgb-effects-probe"; };',
        'effects_probe_b: effects_probe_b { compatible = "test,rgb-effects-probe"; };',
        'effects_optional: effects_optional { compatible = "test,rgb-effects-probe"; optional-callbacks; };',
        'effects_fallback: effects_fallback { compatible = "keypaw,rgb-matrix-solid"; color = <0x07111f>; duration = <0>; };',
    ]
    for compatible in ("first", "second"):
        for index, mode in enumerate(("pulse", "idle", "sweep")):
            nodes.append(f'effects_enum_{compatible}_{index}: effects_enum_{compatible}_{index} '
                         f'{{ compatible = "test,rgb-effects-enum-{compatible}"; mode = "{mode}"; }};')
    result = "/ { effects_private {\n" + "\n".join(nodes) + "\n}; };\n"
    result += """
&kscan { events = <>; };
&rgb_matrix {
    effects_owner: effects_owner {
        compatible = "keypaw,rgb-overlay"; leds = <1 3>; opacity = <37>;
        effects_owned: effects_owned { compatible = "test,rgb-effects-probe"; };
    };
    effects_optional_owner: effects_optional_owner {
        compatible = "keypaw,rgb-overlay"; leds = <1 3>; opacity = <100>;
        effects_optional_child: effects_optional_child {
            compatible = "test,rgb-effects-probe"; optional-callbacks;
        };
    };
    effects_real_owner: effects_real_owner {
        compatible = "keypaw,rgb-overlay"; leds = <1 3>; opacity = <100>;
        effects_owned_reactive: effects_owned_reactive {
            compatible = "keypaw,rgb-matrix-reactive";
            color = <0xffffff>; duration = <1000>; background-brightness = <0>; multi;
        };
    };
    effects_battery: effects_battery {
        compatible = "keypaw,rgb-overlay-battery"; leds = <2 3>;
        color = <0xc94965>; background-color = <0x07111f>;
    };
    effects_battery_reverse: effects_battery_reverse {
        compatible = "keypaw,rgb-overlay-battery"; leds = <2 3>; reverse;
        color = <0xc94965>; background-color = <0x07111f>;
    };
};
"""
    if zero:
        result += "&rgb_matrix { /delete-property/ strip; mapping = <>; };\n"
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--scenario", choices=SCENARIOS)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--example-module", type=Path)
    parser.add_argument("--no-example", action="store_true")
    args = parser.parse_args()
    if args.example_module and args.no_example:
        parser.error("--example-module and --no-example are mutually exclusive")
    if args.repeat < 1:
        parser.error("--repeat must be positive")
    root = native_build_root(args.build_dir, "effect-contracts", prefix="rgb-effects-native-")
    print(f"Artifacts: {root}", flush=True)
    example = args.example_module or MODULE.parent / "zmk-rgb-effect-example"
    if args.example_module and not (example / "zephyr/module.yml").is_file():
        parser.error("example module must contain zephyr/module.yml")
    have_example = not args.no_example and (example / "zephyr/module.yml").is_file()
    if not have_example:
        reason = "Explicitly disabled" if args.no_example else "Unavailable"
        print(f"{reason} external example module: {example}; all built-in variants remain required.", flush=True)
    flags = os.environ.get("EXTRA_LDFLAGS", "")
    os.environ["EXTRA_LDFLAGS"] = (flags + " -Wl,--wrap=z_impl_sys_rand_get "
                                  "-Wl,--wrap=zmk_battery_state_of_charge").strip()
    previous_modules = os.environ.get("EXTRA_ZEPHYR_MODULES")
    modules = [MODULE, SIM / "module", WORKSPACE / "app/module", WORKSPACE / "app/keymap-module"]
    if have_example:
        modules.append(example)
    os.environ["EXTRA_ZEPHYR_MODULES"] = ";".join(str(path) for path in modules)
    try:
        for image, zero in (("effects", False), ("effects-zero", True)):
            scenarios = tuple(scenario for scenario in ((args.scenario,) if args.scenario else SCENARIOS)
                              if not zero or scenario != "effects-enums")
            if not scenarios:
                continue
            result = build(root, image, fragment(zero, have_example), selectors="", extra_conf=CONF)
            if result.returncode:
                raise AssertionError(f"Native build failed: {root / (image + '-build.log')}\n{result.stdout}")
            for _ in range(args.repeat):
                for scenario in scenarios:
                    run(root, image, scenario, timeout=30)
                    print(f"PASS {image} {scenario}", flush=True)
    finally:
        if previous_modules is None:
            os.environ.pop("EXTRA_ZEPHYR_MODULES", None)
        else:
            os.environ["EXTRA_ZEPHYR_MODULES"] = previous_modules
        if flags:
            os.environ["EXTRA_LDFLAGS"] = flags
        else:
            os.environ.pop("EXTRA_LDFLAGS", None)


if __name__ == "__main__":
    main()
