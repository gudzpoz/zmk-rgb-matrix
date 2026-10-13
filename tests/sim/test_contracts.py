#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Production engine/scene contracts, one native process per scenario."""
import argparse
from pathlib import Path
from native_build import MODULE, build, run

SCENARIOS = (
    'periodic', 'inhibited', 'off', 'coalesce', 'starved', 'reentrant',
    'black-retry', 'feedback', 'color-failure', 'rollover', 'input-during-render',
    'idle-policy', 'idempotent', 'intent', 'purge', 'stale', 'resume',
    'slow-render', 'slow-driver', 'black-slow', 'boundary', 'lock-retry',
    'black-resume', 'stale-early', 'submit-failure', 'finish-failure',
    'settled', 'slow-early', 'fairness', 'ignored-release', 'source-invalidate', 'callback-invalidate',
    'color-backoff', 'latest-scene', 'urgent-black', 'coalesced-gates',
    'slow-failure', 'boot-off', 'boot-on', 'boot-inhibited', 'unready',
    'retry-lock', 'requests', 'healthy-mutation', 'mutation-before-lock',
    'urgent-black-success', 'participation-backoff',
    'refresh-barrier', 'stale-refresh', 'selection-holds', 'resume-held',
    'resume-participation',
    'static-bottom', 'multi-prefix', 'invalidate', 'animating-bottom', 'cover',
    'brightness', 'idle', 'child-change', 'gate-change', 'registry-holes',
    'status-only', 'no-active', 'empty-registry', 'lifecycle',
    'output-serialization', 'render-serialization',
    'composition-ab', 'composition-ba', 'filters',
)
FRAGMENT = r'''
&preview_selector { status = "disabled"; };
&rgbs { status = "disabled"; };
&kprgb { /delete-property/ selector; };
&rgb_overlays {
    fixture_overlay { status = "disabled"; };
    fixture_battery { status = "disabled"; };
    fixture_batt_sub { status = "disabled"; };
    engine_bottom: engine_bottom {
        compatible = "keypaw,native-engine-overlay"; slot = <0>;
        all-leds; condition = <&engine_condition>;
    };
    engine_mid: engine_mid {
        compatible = "keypaw,native-engine-overlay"; slot = <1>;
        leds = <0>; condition = <&engine_mid_latch>;
    };
    engine_top: engine_top {
        compatible = "keypaw,native-engine-overlay"; slot = <2>;
        leds = <1>; condition = <&engine_top_latch>;
    };
    engine_cover: engine_cover {
        compatible = "keypaw,native-engine-overlay"; slot = <3>;
        all-leds; condition = <&engine_cover_latch>;
    };
};
&rgb_conditions {
    engine_condition: engine_condition { compatible = "keypaw,native-engine-condition"; };
    engine_mid_latch: engine_mid_latch {
        compatible = "keypaw,rgb-condition-latch";
        emid: emid { compatible = "keypaw,behavior-rgb-overlay-toggle"; #binding-cells = <0>; };
    };
    engine_top_latch: engine_top_latch {
        compatible = "keypaw,rgb-condition-latch";
        etop: etop { compatible = "keypaw,behavior-rgb-overlay-toggle"; #binding-cells = <0>; };
    };
    engine_cover_latch: engine_cover_latch {
        compatible = "keypaw,rgb-condition-latch";
        ecover: ecover { compatible = "keypaw,behavior-rgb-overlay-toggle"; #binding-cells = <0>; };
    };
};
/ { engine_strip: engine_strip {
    compatible = "keypaw,native-engine-strip"; chain-length = <48>;
    color-mapping = <LED_COLOR_ID_RED LED_COLOR_ID_GREEN LED_COLOR_ID_BLUE>;
}; };
&rgb_matrix { strip = <&engine_strip>; idle-brightness = <50>; };
&kscan { events = <>; };
'''
CONF = """
CONFIG_KEYPAW_SIM_ENGINE_CONTRACTS=y
CONFIG_KEYPAW_RGB_CONTROL_AUTO_START=n
CONFIG_KEYPAW_RGB_MATRIX_TICK_MS=16
CONFIG_SYS_CLOCK_TICKS_PER_SEC=1000
CONFIG_TEST=y
CONFIG_IRQ_OFFLOAD=y
CONFIG_LTO=n
"""

VARIANTS = ('normal', 'zero', 'idle-off', 'idle-dim', 'empty', 'no-active', 'boot-off', 'composition-ab', 'composition-ba')
FIRST = '''
composition_first: composition_first {
    compatible = "keypaw,rgb-overlay"; opacity = <37>; leds = <1>;
    condition = <&fixture_always>;
    first_child { compatible = "keypaw,native-engine-effect"; slot = <0>; };
};
'''
SECOND = '''
composition_second: composition_second {
    compatible = "keypaw,rgb-overlay"; opacity = <61>; leds = <1 0>;
    condition = <&fixture_always>;
    second_child { compatible = "keypaw,native-engine-effect"; slot = <1>; };
};
'''
FILTER = '''
composition_filter {
    compatible = "keypaw,native-engine-overlay"; slot = <4>; filter;
    all-leds; condition = <&fixture_always>;
};
'''

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=MODULE / 'build/native-contracts')
    parser.add_argument('--variant', choices=VARIANTS)
    parser.add_argument('--scenario', choices=SCENARIOS)
    parser.add_argument('--repeat', type=int, default=1)
    parser.add_argument('--no-build', action='store_true')
    args = parser.parse_args()
    if args.repeat < 1: parser.error('--repeat must be positive')
    for variant in ((args.variant,) if args.variant else VARIANTS):
        fragment, conf = FRAGMENT, CONF
        scenarios = SCENARIOS if variant == 'normal' else ('settled', 'purge', 'resume') if variant == 'zero' else ('idle-policy', 'intent') if variant.startswith('idle') else ('empty-registry',) if variant == 'empty' else ('boot-off',) if variant == 'boot-off' else ('no-active',)
        if variant == 'normal': scenarios = tuple(s for s in scenarios if s not in ('idle-policy', 'intent', 'idle', 'no-active', 'boot-off', 'empty-registry', 'composition-ab', 'composition-ba', 'filters'))
        if variant == 'boot-off': fragment += '\n&rgb_matrix { /delete-property/ initial-on; };\n'
        if variant == 'zero': fragment += '\n&engine_strip { chain-length = <0>; }; &rgb_matrix { mapping = <>; };\n'
        if variant == 'idle-off': conf += 'CONFIG_KEYPAW_RGB_MATRIX_AUTO_OFF_IDLE=y\n'
        if variant == 'idle-dim': scenarios += ('idle',)
        if variant == 'empty': fragment += '\n&engine_bottom { status = "disabled"; }; &engine_mid { status = "disabled"; }; &engine_top { status = "disabled"; }; &engine_cover { status = "disabled"; };\n'
        if variant == 'no-active': fragment += '\n&engine_bottom { condition = <&engine_mid_latch>; };\n'
        if variant.startswith('composition-'):
            fragment += '\n&rgb_overlays {\n' + (SECOND + FIRST if variant.endswith('ba') else FIRST + SECOND) + FILTER + '\n};\n'
            scenarios = (variant, 'filters') if variant.endswith('ab') else (variant,)
        if args.scenario:
            if args.scenario not in scenarios:
                if args.variant: parser.error('scenario is not available in the selected variant')
                continue
            scenarios = (args.scenario,)
        name = 'engine-' + variant
        if not args.no_build:
            result = build(args.build_dir, name, fragment, selectors='', extra_conf=conf)
            if result.returncode: raise AssertionError(result.stdout)
        for scenario in scenarios:
            for _ in range(args.repeat): run(args.build_dir, name, scenario)
    print('Native engine contracts passed.')


if __name__ == '__main__': main()
