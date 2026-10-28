#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real native coordinator contracts. Each scenario boots an isolated process."""
import argparse
from pathlib import Path
import tempfile

from native_build import SELECTORS, build, run

SCENARIOS = (
    'startup', 'dag', 'same-value', 'isr', 'external-timer', 'no-poll',
    'scope', 'unused-scope',
    'chain', 'combinators', 'invalid-scope', 'short-circuit',
    'refresh', 'refresh-interrupted', 'early-refresh', 'finite-self-cascade', 'finite-multi-cascade',
    'trigger-startup', 'trigger-capture', 'trigger-errors',
    'cross-selector', 'third-party', 'selection-burst',
    'trigger-mutation', 'resume-selection', 'external-interruption',
    'overlay-gate',
    'startup-positional', 'startup-auto',
    'startup-unrestored-settings', 'startup-settings-staged',
    'overlay-zero', 'trigger-no-table', 'trigger-actions', 'trigger-unregistered-root',
    'stale-source-refresh', 'pending-source', 'runtime-reload-freshness',
)
FIXTURE = '''
#include <dt-bindings/keypaw/rgb_matrix.h>
&rgb_conditions {
    cc_root: cc_root { compatible = "test,rgb-condition-contract"; };
    cc_left: cc_left { compatible = "test,rgb-condition-contract"; };
    cc_right: cc_right { compatible = "test,rgb-condition-contract"; };
    cc_leaf: cc_leaf { compatible = "test,rgb-condition-contract"; };
    cc_unused: cc_unused { compatible = "test,rgb-condition-contract"; central-only; };
    cc_central: cc_central { compatible = "test,rgb-condition-contract"; central-only; };
    cc_bad: cc_bad { compatible = "test,rgb-condition-contract"; invalid-scope; };
    cc_selected_a: cc_selected_a { compatible = "keypaw,rgb-condition-effect-selected";
        selector = <&selector_a>; effect = <&probe_a>; };
    cc_selected_c: cc_selected_c { compatible = "keypaw,rgb-condition-effect-selected";
        selector = <&selector_a>; effect = <&probe_c>; };
    cc_event: cc_event { compatible = "test,rgb-condition-contract";
        selected-selector = <&selector_a>; selected-effect = <&probe_a>; };
    cc_event_b: cc_event_b { compatible = "test,rgb-condition-contract";
        selected-selector = <&selector_b>; selected-effect = <&probe_b>; };
};
&ordinary { condition = <&cc_root>; local; };
&rgb_matrix {
    cc_shared: cc_shared { compatible = "keypaw,rgb-overlay"; condition = <&cc_selected_a>;
        local; leds = <4>; ccwh1 { compatible = "keypaw,rgb-matrix-solid";
        color = <0xffffff>; }; };
    cc_shared2: cc_shared2 { compatible = "keypaw,rgb-overlay"; condition = <&cc_selected_a>;
        local; leds = <5>; ccwh2 { compatible = "keypaw,rgb-matrix-solid";
        color = <0xffffff>; }; };
    cc_opposite: cc_opposite { compatible = "keypaw,rgb-overlay"; condition = <&cc_selected_c>;
        local; leds = <6>; ccwh3 { compatible = "keypaw,rgb-matrix-solid";
        color = <0xffffff>; }; };
    cc_event_gate: cc_event_gate { compatible = "keypaw,rgb-overlay"; condition = <&cc_event>;
        local; leds = <7>; ccwh4 { compatible = "keypaw,rgb-matrix-solid";
        color = <0xffffff>; }; };
    cc_event_b_gate: cc_event_b_gate { compatible = "keypaw,rgb-overlay";
        condition = <&cc_event_b>; local; leds = <11>;
        ccwh10 { compatible = "keypaw,rgb-matrix-solid"; }; };
    cc_remote: cc_remote { compatible = "keypaw,rgb-overlay"; condition = <&cc_central>;
        leds = <8>; ccwh5 { compatible = "keypaw,rgb-matrix-solid";
        color = <0xffffff>; }; };
    cc_transparent: cc_transparent { compatible = "keypaw,rgb-overlay";
        condition = <&cc_root>; local; all-leds; opacity = <0>;
        ccwh6 { compatible = "keypaw,rgb-matrix-solid"; }; };
    cc_half: cc_half { compatible = "keypaw,rgb-overlay";
        condition = <&cc_root>; local; all-leds; opacity = <50>;
        ccwh8 { compatible = "keypaw,rgb-matrix-solid"; }; };
    cc_empty: cc_empty { compatible = "keypaw,rgb-overlay"; local;
        ccwh7 { compatible = "keypaw,rgb-matrix-solid"; }; };
};
/ { behaviors { ccact: ccact { compatible = "test,rgb-condition-contract-action";
        #binding-cells = <2>; }; };
    cc_triggers { compatible = "keypaw,rgb-trigger-table";
        primary { condition = <&cc_root>; on-enter = <&ccact 10 77>; on-exit = <&ccact 11 77>; };
        left_edge { condition = <&cc_left>;
            on-enter = <&ccact 20 77>; on-exit = <&ccact 21 77>; };
        third { condition = <&cc_right>; on-enter = <&ccact 25 77>; on-exit = <&ccact 26 77>; };
        constant { on-enter = <&ccact 30 77>; };
        shared { condition = <&cc_root>; on-enter = <&ccact 40 77>; on-exit = <&ccact 41 77>; };
        exit_only { condition = <&cc_leaf>; on-exit = <&ccact 50 77>; };
    };
};
'''
CONF = '''
CONFIG_KEYPAW_SIM_CONDITION_CONTRACTS=y
CONFIG_TEST=y
CONFIG_IRQ_OFFLOAD=y
'''
SELECTOR_FIXTURE = SELECTORS.replace('keypaw,rgb-matrix-selector-probe',
                                    'keypaw,rgb-matrix-solid').replace(' animate;', '')


VARIANTS = {
    'startup-positional': ('settings', '', 'CONFIG_SETTINGS=y\nCONFIG_SETTINGS_NONE=y\nCONFIG_SETTINGS_RUNTIME=y\n'),
    'overlay-zero': ('zero', '&rgb_matrix { /delete-property/ strip; mapping = <>; };', ''),
    'trigger-no-table': ('no-table', '/ { /delete-node/ cc_triggers; };', ''),
    'stale-source-refresh': ('refresh-strip', '''
/ { cc_strip: cc_strip { compatible = "test,rgb-condition-contract-strip";
    chain-length = <48>; color-mapping = <LED_COLOR_ID_RED LED_COLOR_ID_GREEN LED_COLOR_ID_BLUE>; }; };
&rgb_matrix { strip = <&cc_strip>; };
''', ''),
    'trigger-unregistered-root': ('unregistered', '''
&rgb_conditions { cc_noreg: cc_noreg { compatible = "test,rgb-condition-contract"; unregistered; }; };
&{/cc_triggers/third} { condition = <&cc_noreg>; };
''', ''),
    'trigger-errors': ('errors', '''
&{/cc_triggers/primary} {
    on-enter = <&ccact 10 77 &ccact 60 77>;
    on-exit = <&ccact 11 77 &ccact 61 77>;
};
''', ''),
    'trigger-actions': ('actions', '''
&{/cc_triggers/primary} {
    on-enter = <&rgb_matrix RGB_OFF &ovl_tog &selector_a RGB_EFS_CMD 1>;
    on-exit = <&rgb_matrix RGB_ON &ovl_tog &selector_a RGB_EFS_CMD 0>;
};
&rgb_matrix { cc_latchgate: cc_latchgate {
    compatible = "keypaw,rgb-overlay"; local; condition = <&fixture_latch>; leds = <10>;
    ccwh9 { compatible = "keypaw,rgb-matrix-solid"; };
}; };
''', ''),
}
VARIANTS['startup-unrestored-settings'] = VARIANTS['startup-positional']
VARIANTS['startup-settings-staged'] = VARIANTS['startup-positional']
VARIANTS['pending-source'] = VARIANTS['stale-source-refresh']
VARIANTS['resume-selection'] = VARIANTS['stale-source-refresh']
VARIANTS['runtime-reload-freshness'] = (
    'settings-refresh-strip', VARIANTS['stale-source-refresh'][1] + '''
&rgb_matrix { cc_observer: cc_observer {
    compatible = "keypaw,native-engine-overlay"; slot = <0>; local; leds = <47>;
}; };
''', 'CONFIG_SETTINGS=y\nCONFIG_SETTINGS_NONE=y\nCONFIG_SETTINGS_RUNTIME=y\n')
VARIANTS.update({
    'dag': ('diamond', '''
&cc_root { conditions = <&cc_left &cc_right>; };
&cc_left { conditions = <&cc_leaf>; };
&cc_right { conditions = <&cc_leaf>; };
''', ''),
    'combinators': ('combinators', '''
&cc_root { conditions = <&cc_all>; };
&cc_left { conditions = <&cc_leaf>; };
&cc_right { conditions = <&cc_leaf>; };
&rgb_conditions {
    cc_all: cc_all {
        compatible = "keypaw,rgb-condition-and";
        conditions = <&cc_left &cc_any>;
    };
    cc_any: cc_any {
        compatible = "keypaw,rgb-condition-or";
        conditions = <&cc_right &cc_leaf>;
    };
};
''', ''),
    'chain': ('chain', '''
&cc_root { conditions = <&cc_left>; };
&cc_left { conditions = <&cc_right>; };
&cc_right { conditions = <&cc_leaf>; };
''', ''),
    'scope': ('scope', '&cc_root { conditions = <&cc_left>; }; &cc_left { conditions = <&cc_central>; };', ''),
    'invalid-scope': ('invalid-scope', '&cc_root { conditions = <&cc_bad>; };', ''),
    'external-timer': ('timer', '&cc_root { conditions = <&cc_leaf>; };', ''),
})
VARIANTS['short-circuit'] = VARIANTS['dag']

DIAGNOSTICS = {
    'invalid-scope': 'RGB condition cc_bad: invalid provider',
}

REJECTIONS = {
    'cycle': ('&cc_root { conditions = <&cc_left>; }; &cc_left { conditions = <&cc_right>; }; &cc_right { conditions = <&cc_left>; };',
              ('cycle in devicetree involving', 'cc_left', 'cc_right')),
    'self': ('&cc_root { conditions = <&cc_root>; };',
             ('condition dependency must precede its consumer',)),
    'disabled': ('&cc_root { conditions = <&cc_leaf>; }; &cc_leaf { status = "disabled"; };',
                 ('condition dependency must be enabled',)),
    'unregistered': ('&cc_root { conditions = <&cc_leaf>; }; &cc_leaf { unregistered; };',
                     ('undefined reference', 'kp_rgb_condition_')),
}


def reject_graphs(root, role, fragment, conf, names, positive_ready):
    if not positive_ready:
        variant, addition, options = VARIANTS['external-timer']
        image = f'{role}-{variant}'
        result = build(root, image, fragment + addition, selectors=SELECTOR_FIXTURE,
                       extra_conf=conf + options)
        if result.returncode:
            raise AssertionError(f'{image} positive control failed:\n{result.stdout[-15000:]}')
        run(root, image, 'external-timer')
    for name in names:
        addition, diagnostics = REJECTIONS[name]
        image = f'{role}-reject-{name}'
        result = build(root, image, fragment + addition, selectors=SELECTOR_FIXTURE, extra_conf=conf)
        if not result.returncode or not all(text in result.stdout for text in diagnostics):
            raise AssertionError(f'{image}: expected rejection {diagnostics!r}:\n{result.stdout[-15000:]}')
        print(f'PASS {image} rejected with expected diagnostic', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--scenario', action='append', choices=SCENARIOS)
    parser.add_argument('--reject', action='append', choices=REJECTIONS)
    parser.add_argument('--repeat', type=int, default=1)
    parser.add_argument('--role', choices=('standalone', 'central', 'peripheral', 'all'), default='standalone')
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error('--repeat must be >= 1')
    root = (args.build_dir or Path(tempfile.mkdtemp(prefix='rgb-condition-contracts-'))).resolve()
    root.mkdir(parents=True, exist_ok=True)
    print(f'ARTIFACTS {root}', flush=True)
    roles = ('standalone', 'central', 'peripheral') if args.role == 'all' else (args.role,)
    for role in roles:
        conf = CONF
        fragment = FIXTURE
        if role != 'standalone':
            conf += 'CONFIG_ZMK_SPLIT=y\nCONFIG_ZMK_BLE=y\nCONFIG_ZMK_SPLIT_BLE=y\n'
            conf += f'CONFIG_ZMK_SPLIT_ROLE_CENTRAL={"y" if role == "central" else "n"}\n'
            fragment += '/ { chosen { zephyr,bt-hci = &cc_hci; }; cc_hci: cc_hci { compatible = "zephyr,bt-hci-userchan"; }; };'
        groups = {}
        for scenario in args.scenario or (() if args.reject else SCENARIOS):
            variant = VARIANTS.get(scenario, ('automatic', '', ''))
            groups.setdefault(variant, []).append(scenario)
        for (variant, addition, options), scenarios in groups.items():
            image = f'{role}-{variant}'
            selectors = SELECTOR_FIXTURE
            result = build(root, image, fragment + addition, selectors=selectors,
                           extra_conf=conf + options)
            if result.returncode:
                raise AssertionError(f'{image} build failed: {root / (image + "-build.log")}\n{result.stdout[-15000:]}')
            for iteration in range(args.repeat):
                for scenario in scenarios:
                    result = run(root, image, scenario)
                    diagnostic = DIAGNOSTICS.get(scenario)
                    if diagnostic and diagnostic not in result.stdout:
                        raise AssertionError(f'{image}/{scenario}: missing diagnostic {diagnostic!r}')
                    if role == 'peripheral' and scenario == 'scope':
                        assert 'central-only source used on peripheral' in result.stdout
                    if role != 'peripheral' and scenario == 'trigger-unregistered-root':
                        assert 'Unregistered RGB condition root cc_noreg' in result.stdout
                    if role != 'peripheral' and scenario == 'trigger-errors':
                        assert 'binding 1' in result.stdout and 'err -5' in result.stdout
                    log = root / f'{image}-{scenario}.log'
                    log.rename(root / f'{image}-{scenario}-{iteration + 1}.log')
                    print(f'PASS {image} {scenario} iteration={iteration + 1}', flush=True)
        if role == roles[0] and (args.reject or not args.scenario):
            reject_graphs(root, role, fragment, conf, args.reject or REJECTIONS,
                          VARIANTS['external-timer'] in groups)


if __name__ == '__main__':
    main()
