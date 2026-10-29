#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Production engine/scene contracts, one native process per scenario."""
import argparse
from pathlib import Path
from native_build import MODULE, build, run

SCENARIOS = (
    'periodic', 'inhibited', 'off', 'coalesce', 'starved', 'reentrant',
    'black-one-shot', 'feedback', 'rollover', 'input-during-render',
    'idempotent', 'purge', 'stale', 'resume',
    'slow-render', 'slow-driver', 'black-slow', 'boundary', 'worker-held-lock', 'worker-blocked-input',
    'black-resume', 'stale-early', 'submit-failure', 'finish-failure',
    'settled', 'slow-early', 'fairness', 'ignored-release',
    'static-failure', 'fresh-scene', 'urgent-black', 'coalesced-gates',
    'slow-failure', 'boot-off', 'boot-on', 'boot-inhibited', 'unready',
    'flush-held-lock', 'requests', 'healthy-mutation',
    'urgent-black-success', 'participation-after-failure',
    'natural-frame-failure', 'held-control-lock',
    'pending-control', 'resume-pending',
    'resume-participation', 'scene-entry-freshness', 'control-during-driver',
    'control-at-completion', 'pending-control-black',
    'pending-control-black-one-shot',
    'static-flush', 'cover',
    'brightness', 'idle', 'child-change', 'gate-change',
    'lifecycle-batch',
    'status-only', 'no-active', 'empty-registry', 'lifecycle',
    'output-serialization', 'render-serialization',
    'composition-ab', 'composition-ba', 'solid-brightness',
    'idle-phase', 'sleep', 'sleep-off', 'sleep-inhibited', 'sleep-idle',
    'sleep-failure', 'sleep-after-color-failure', 'sleep-failure-resume',
    'sleep-serialization', 'sleep-render-serialization',
    'boot-idle', 'boot-sleep',
)
FRAGMENT = r'''
&preview_selector { status = "disabled"; };
&rgb_matrix {
    fixture_overlay { status = "disabled"; };
    fixture_battery { status = "disabled"; };
    fixture_batt_sub { status = "disabled"; };
    engine_bottom: engine_bottom {
        compatible = "keypaw,native-engine-overlay"; slot = <0>;
        all-leds; condition = <&engine_condition>;
    };
    engine_mid: engine_mid {
        compatible = "keypaw,native-engine-overlay"; slot = <1>;
        test-behavior; #binding-cells = <0>; label = "eng_mid";
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
CONFIG_KEYPAW_RGB_MATRIX_TICK_MS=16
CONFIG_SYS_CLOCK_TICKS_PER_SEC=1000
CONFIG_TEST=y
CONFIG_IRQ_OFFLOAD=y
CONFIG_LTO=n
"""

VARIANTS = ('normal', 'zero', 'idle-off', 'idle-default', 'idle-dim',
            'idle-equal', 'idle-above', 'max-zero', 'empty', 'no-active', 'boot-off',
            'composition-ab', 'composition-ba', 'solid-brightness')
FIRST = '''
composition_first: composition_first {
    compatible = "keypaw,rgb-overlay"; opacity = <37>; leds = <1>;
    first_child { compatible = "keypaw,native-engine-effect"; slot = <0>; };
};
'''
SECOND = '''
composition_second: composition_second {
    compatible = "keypaw,rgb-overlay"; opacity = <61>; leds = <1 0>;
    second_child { compatible = "keypaw,native-engine-effect"; slot = <1>; };
};
'''
FILTER = '''
composition_filter {
    compatible = "keypaw,native-engine-overlay"; slot = <4>; filter;
    test-behavior; #binding-cells = <0>; label = "eng_filt";
    all-leds;
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
        if variant == 'normal':
            scenarios = tuple(s for s in SCENARIOS if s not in (
                'idle', 'idle-phase', 'no-active', 'boot-off',
                'empty-registry', 'composition-ab', 'composition-ba', 'solid-brightness'))
        elif variant == 'zero':
            scenarios = ('settled',)
        elif variant.startswith('idle-') or variant == 'max-zero':
            scenarios = ('idle',)
        else:
            scenarios = {'empty': ('empty-registry',), 'boot-off': ('boot-off',)}.get(variant, ('no-active',))
        if variant == 'boot-off': fragment += '\n&rgb_matrix { /delete-property/ initial-on; };\n'
        if variant == 'zero': fragment += '\n&engine_strip { chain-length = <0>; }; &rgb_matrix { mapping = <>; };\n'
        if variant == 'idle-off': fragment += '\n&rgb_matrix { idle-brightness = <0>; };\n'
        if variant == 'idle-default': fragment += '\n&rgb_matrix { /delete-property/ idle-brightness; };\n'
        if variant in ('idle-dim', 'idle-equal', 'idle-above', 'max-zero'):
            cap = {'idle-dim': 30, 'idle-equal': 60, 'idle-above': 90, 'max-zero': 50}[variant]
            maximum = 0 if variant == 'max-zero' else 60
            fragment += f'\n&rgb_matrix {{ max-brightness = <{maximum}>; idle-brightness = <{cap}>; }};\n'
            scenarios += ('idle-phase',)
        if variant in ('idle-off', 'idle-default', 'idle-dim'): scenarios += ('sleep-idle',)
        if variant == 'idle-default': scenarios += ('boot-idle',)
        if variant == 'empty': fragment += '\n&engine_bottom { status = "disabled"; }; &engine_mid { status = "disabled"; }; &engine_top { status = "disabled"; }; &engine_cover { status = "disabled"; };\n'
        if variant == 'no-active': fragment += '\n&engine_bottom { condition = <&engine_mid_latch>; };\n'
        if variant.startswith('composition-'):
            fragment += '\n&rgb_matrix {\n' + (SECOND + FIRST if variant.endswith('ba') else FIRST + SECOND) + FILTER + '\n};\n'
            scenarios = (variant,)
        if variant == 'solid-brightness':
            fragment += '''
&rgb_matrix {
    max-brightness = <60>;
    engine_solid_owner {
        compatible = "keypaw,rgb-overlay"; opacity = <37>; leds = <1>;
        engine_solid: engine_solid {
            compatible = "keypaw,rgb-matrix-solid"; color = <0xff0000>;
        };
    };
};
'''
            scenarios = ('solid-brightness',)
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
            for _ in range(args.repeat):
                result = run(args.build_dir, name, scenario)
                if scenario in ('submit-failure', 'finish-failure'):
                    assert result.stdout.count('RGB coordinator queue invariant violated (-5)') == 1, result.stdout
    print('Native engine contracts passed.')


if __name__ == '__main__': main()
