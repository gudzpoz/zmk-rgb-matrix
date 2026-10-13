#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Exercise scoped RGB repair through the real native_sim central transport API."""
from pathlib import Path
import subprocess
import tempfile

from native_process import run_logged
from test_selector import SELECTORS, build

SOLID_SELECTORS = (SELECTORS.replace("keypaw,rgb-matrix-selector-probe",
                                    "keypaw,rgb-matrix-solid")
                   .replace(" animate;", "")
                   .replace("condition = <&preview_visible>;", "condition = <&fixture_always>;") + '''
&rgb_overlays { /delete-node/ preview_selector; };
&rgb_conditions { /delete-node/ preview_visible; };
/ { behaviors { /delete-node/ rgbs; }; };
''')
CONF = """CONFIG_KEYPAW_SIM_SELECTOR_TEST=n
CONFIG_KEYPAW_SIM_SPLIT_TEST=y
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


def runtime(root, name, fragment):
    result = build(root, name, '''/ { chosen { zephyr,bt-hci = &test_hci; };
        test_hci: bt_hci { compatible = "zephyr,bt-hci-userchan"; }; };''' + fragment,
                   runtime=True, selectors=SOLID_SELECTORS, extra_conf=CONF)
    assert result.returncode == 0, result.stdout[-20000:]
    command = [str(root / name / "build/zephyr/zmk.exe"),
               "--stop_at=16", "-no-rt"]
    print("COMMAND", " ".join(command), flush=True)
    run = run_logged(command, cwd=root, text=True, stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT, log_path=root / f"{name}.log")
    assert run.returncode == 0, run.stdout[-20000:]
    for marker in (
        "off/inhibited reconnect endpoints/qualified ordering",
        "same-member parameter repair/independent lanes/frozen retries",
        "rapid changes/retry convergence",
        "disconnect/reconnect discards in-flight snapshot",
        "outside-mutex transport/local errors",
        "reset repairs inactive members after frozen retry",
        "global behavior repairs frozen power/brightness",
        "global reset repairs all selectors/members",
        "transport acceptance only",
    ):
        assert f"PASS scoped split {marker}" in run.stdout, run.stdout
    # BLE is compiled only to provide two-source capacity; no radio is attached.
    errors = [line for line in run.stdout.splitlines() if "<err>" in line]
    assert all("HCI driver is not ready" in line or
               "BLUETOOTH FAILED (-19)" in line for line in errors), run.stdout
    print(f"PASS scoped split native {name} (local acceptance, not BLE delivery)", flush=True)


def main():
    root = Path(tempfile.mkdtemp(prefix="rgb-scoped-split-c1c2s6-"))
    print(f"ARTIFACTS {root}", flush=True)
    runtime(root, "transport", "")
    runtime(root, "transport-zero", '''
&rgb_matrix { /delete-property/ strip; mapping = <>; };
&selector_a { /delete-property/ leds; };
&selector_b { /delete-property/ leds; };
&ordinary { /delete-property/ leds; };
''')


if __name__ == "__main__":
    main()
