#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Exercise current settings commit/startup, scoped saves and independent retries."""
from pathlib import Path
import subprocess
import tempfile

from native_process import run_logged
from test_selector import SELECTORS, build

SELECTORS = (SELECTORS.replace('keypaw,rgb-matrix-selector-probe', 'keypaw,rgb-matrix-solid')
             .replace(' animate;', '') + '''
&selector_a { persist-id = "a"; };
&selector_b { persist-id = "b"; };
&probe_a { persist-id = "shared"; };
&probe_b { persist-id = "shared"; };
&probe_c { persist-id = "alt"; };
&probe_d { persist-id = "alt"; };
''')
CONF = '''CONFIG_KEYPAW_SIM_SELECTOR_TEST=n
CONFIG_SETTINGS=y
CONFIG_SETTINGS_CUSTOM=y
CONFIG_KEYPAW_SIM_SETTINGS_TEST=y
CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE=20
'''


def main():
    root = Path(tempfile.mkdtemp(prefix="rgb-scoped-settings-"))
    print(f"ARTIFACTS {root}", flush=True)
    for name, fragment, config in (
        ("auto", "", ""),
        ("manual", "", "CONFIG_KEYPAW_RGB_CONTROL_AUTO_START=n\n"),
        ("late-control", '''&rgbs_a { compatible = "test,rgb-selector-late-control", "keypaw,behavior-rgb-selector"; zephyr,deferred-init; };''',
         "CONFIG_KEYPAW_RGB_CONTROL_AUTO_START=n\n"),
    ):
        result = build(root, name, fragment, runtime=True, selectors=SELECTORS,
                       extra_conf=CONF + config)
        assert result.returncode == 0, result.stdout[-20000:]
        run = run_logged([str(root / name / "build/zephyr/zmk.exe"),
                          "--stop_at=4", "-no-rt"], cwd=root, text=True,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         log_path=root / f"{name}.log")
        assert run.returncode == 0, run.stdout[-20000:]
        for marker in (
            "restored before startup notification",
            "legacy records ignored",
            "independent save despite persistent failure",
            "addressed save/reset/reload isolation",
            "global reset/reload",
        ):
            assert f"PASS scoped settings {marker}" in run.stdout, run.stdout
        if name != "auto":
            assert "PASS scoped settings subtree/manual startup" in run.stdout, run.stdout
        errors = [line for line in run.stdout.splitlines() if "<err>" in line]
        assert all(
            "set-value failure. key: keypaw/rgb_matrix/state error(-22)" in line
            or (name == "late-control" and "canonical control is not ready" in line)
            for line in errors), run.stdout
        print(f"PASS scoped settings native {name}", flush=True)


if __name__ == "__main__":
    main()
