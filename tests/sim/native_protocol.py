# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Shared actual-DTS selector layout and central transport configuration."""
from native_build import SELECTORS

SOLID_SELECTORS = (SELECTORS.replace("keypaw,rgb-matrix-selector-probe",
                                    "keypaw,rgb-matrix-solid")
                   .replace(" animate;", "")
                   .replace("condition = <&preview_visible>;", "") + '''
&rgb_matrix { /delete-node/ preview_selector; };
&rgb_conditions { /delete-node/ preview_visible; };
/ { behaviors { /delete-node/ rgbs; }; };
''')
CONF = """CONFIG_KEYPAW_SIM_SELECTOR_TEST=n
CONFIG_KEYPAW_SIM_SPLIT_TEST=y
CONFIG_TEST=y
CONFIG_IRQ_OFFLOAD=y
CONFIG_ZMK_SPLIT=y
CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y
CONFIG_ZMK_BLE=y
CONFIG_ZMK_SPLIT_BLE=y
CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS=2
CONFIG_BT_MAX_CONN=3
CONFIG_BT_MAX_PAIRED=3
CONFIG_KEYPAW_RGB_SPLIT_SYNC=y
CONFIG_KEYPAW_RGB_SPLIT_SYNC_POLL_MS=25
CONFIG_KEYPAW_RGB_SPLIT_SYNC_SETTLE_MS=30
CONFIG_KEYPAW_RGB_SPLIT_SYNC_CLOCK_MS=1000
"""
