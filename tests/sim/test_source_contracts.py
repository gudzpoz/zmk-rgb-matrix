#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Endpoint/battery production providers against external-service doubles."""
import argparse
from pathlib import Path
from native_build import build, run, native_build_root

SOURCES = {
    'ble_connected': '',
    'ble_open': 'state = "open";',
    'ble_disconnected': 'state = "disconnected";',
    'ble_any': 'state = "any";',
    'ble_physical': 'state = "open"; ignore-routing;',
    'ble_profile': 'state = "any"; profile = <1>;',
    'ble_outside': 'state = "any"; profile = <999>;',
    'usb_connected': 'transport = "usb";',
    'usb_physical': 'transport = "usb"; ignore-routing;',
    'usb_powered': 'transport = "usb"; state = "powered";',
    'usb_disconnected': 'transport = "usb"; state = "disconnected";',
    'any_connected': 'transport = "any";',
    'any_disconnected': 'transport = "any"; state = "disconnected";',
    'battery_low': 'below = <20>;',
    'battery_band': 'above = <19>; below = <21>;',
    'battery_full': 'above = <99>; state = "charging";',
    'battery_charging': 'state = "charging";',
    'battery_discharging': 'state = "discharging";',
}
FIXTURE = '&kscan { events = <>; };\n'
for name, props in SOURCES.items():
    kind = 'battery' if name.startswith('battery') else 'endpoint'
    if kind == 'endpoint' and name != 'ble_open':
        props += ' conditions = <&ble_open>;'
    FIXTURE += f'''&rgb_conditions {{ {name}: {name} {{
        compatible = "keypaw,rgb-condition-{kind}"; {props}
    }}; }};
    &rgb_matrix {{ gate_{name}: gate_{name} {{
        compatible = "keypaw,rgb-overlay"; local; leds = <0>;
        condition = <&{name}>;
        solid {{ compatible = "keypaw,rgb-matrix-solid"; }};
    }}; }};
'''


def run_suite(root, roles=('standalone', 'central', 'peripheral', 'power-only', 'stack-only')):
    print(f'ARTIFACTS {root}', flush=True)
    for role in roles:
        conf = 'CONFIG_KEYPAW_SIM_SOURCE_CONTRACTS=y\n'
        fixture = FIXTURE
        if role in ('central', 'peripheral'):
            conf += 'CONFIG_ZMK_SPLIT=y\nCONFIG_ZMK_BLE=y\nCONFIG_ZMK_SPLIT_BLE=y\n'
            conf += f'CONFIG_ZMK_SPLIT_ROLE_CENTRAL={"y" if role == "central" else "n"}\n'
            fixture += '/ { chosen { zephyr,bt-hci = &source_hci; }; source_hci: source_hci { compatible = "zephyr,bt-hci-userchan"; }; };'
        if role == 'power-only':
            conf += 'CONFIG_KEYPAW_SIM_SOURCE_SOC=n\n'
            for name in ('battery_low', 'battery_band', 'battery_full'):
                fixture += f'&{name} {{ status = "disabled"; }}; &gate_{name} {{ status = "disabled"; }};'
        if role == 'stack-only':
            conf += 'CONFIG_KEYPAW_SIM_SOURCE_USB_HID=n\n'
            for name in ('usb_connected', 'usb_physical'):
                fixture += f'&{name} {{ status = "disabled"; }}; &gate_{name} {{ status = "disabled"; }};'
        result = build(root, role, fixture, selectors='', extra_conf=conf)
        if result.returncode:
            raise AssertionError(result.stdout[-16000:])
        scenarios = ('endpoint-states', 'preference-events', 'battery-states')
        if role == 'peripheral':
            scenarios = ('peripheral-scope',)
        elif role == 'power-only':
            scenarios = ('battery-states',)
        elif role == 'stack-only':
            scenarios = ('endpoint-states',)
            rejection = build(root, 'stack-only-reject-connected', FIXTURE,
                              selectors='', extra_conf=conf)
            assert rejection.returncode != 0
            assert 'endpoint USB connected requires CONFIG_ZMK_USB on host side' in rejection.stdout
            print('PASS stack-only USB connected rejected', flush=True)
        for scenario in scenarios:
            run(root, role, scenario)
            print(f'PASS {role} {scenario}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path)
    args = parser.parse_args()
    root = native_build_root(args.build_dir, 'source-contracts', prefix='rgb-source-contracts-')
    run_suite(root)


if __name__ == '__main__':
    main()
