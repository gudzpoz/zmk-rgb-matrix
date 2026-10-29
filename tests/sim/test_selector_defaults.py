#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Native effect initialization, public setters, deferred readiness and reset."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

from native_process import run_logged
from native_build import MODULE, SIM, WEST, WORKSPACE

OVERLAY = '''
&kscan { events = <>; };
&rgb_matrix {
    default_selector: default_selector {
        compatible = "keypaw,rgb-overlay";
        controllable; #binding-cells = <2>; label = "defctl";
        default_ready: ready {
            compatible = "test,rgb-default-effect";
            color = <0x008000>; duration = <0>;
        };
        hole: disabled_member {
            compatible = "test,rgb-default-effect";
            status = "disabled";
        };
        default_explicit: explicit {
            compatible = "test,rgb-default-effect";
            color = <0x000080>; duration = <0xffffffff>;
        };
        default_breathe: breathe {
            compatible = "keypaw,rgb-matrix-breathe";
            mode = "brightness"; color = <0x0000ff>; duration = <2400>;
        };
    };
    default_late_selector: default_late_selector {
        compatible = "keypaw,rgb-overlay";
        controllable; #binding-cells = <2>; label = "latectl";
        default_late: late {
            compatible = "test,rgb-default-effect";
            color = <0x808000>; zephyr,deferred-init;
        };
        default_black: black {
            compatible = "test,rgb-default-effect";
            color = <0x000000>;
        };
        default_white: white {
            compatible = "test,rgb-default-effect";
        };
    };
    default_private_owner: default_private_owner {
        compatible = "keypaw,rgb-overlay";
        default_private: private {
            compatible = "test,rgb-default-effect";
            color = <0x800000>; duration = <0>;
        };
    };
    default_disabled: default_disabled {
        compatible = "keypaw,rgb-overlay"; status = "disabled";
        controllable; #binding-cells = <2>;
        default_disabled_child: child {
            compatible = "test,rgb-default-effect";
            color = <0x800000>; duration = <0>;
        };
    };
};
'''

# Values are paired with independent literal expectations in selector_defaults.c.
BOUNDARIES = [("below", "99"), ("min", "100"), ("max", "10000"),
              ("above", "10001"), ("negative", "(-1)"), ("zero", "0"),
              ("omitted", None)]
OVERLAY += '''
&rgb_matrix {
    boundary_parent_zero: boundary_parent_zero {
        compatible = "keypaw,rgb-overlay";
        controllable; #binding-cells = <2>; label = "bpzero";

        parent_zero: parent_zero {
            compatible = "test,rgb-default-effect";
            duration = <0>;
        };
    };
    child_boundaries {
        compatible = "keypaw,rgb-overlay";
        controllable; #binding-cells = <2>; label = "cbctl";
'''
for name, duration in BOUNDARIES:
    prop = f"duration = <{duration}>;" if duration is not None else ""
    OVERLAY += f'''        child_{name}: child_{name} {{
            compatible = "test,rgb-default-effect"; {prop}
        }};
'''
OVERLAY += '''    };
    private_owner_negative {
        compatible = "keypaw,rgb-overlay";
        private_negative: private_negative {
            compatible = "test,rgb-default-effect"; duration = <(-1)>;
        };
    };
    default_disabled {
        disabled_negative: disabled_negative {
            compatible = "test,rgb-default-effect"; duration = <(-1)>;
        };
    };
};
'''
OVERLAY += '''
&rgb_matrix {
    child_boundaries: child_boundaries {};
    default_selector {
        missing_selected: missing_selected {
            compatible = "test,rgb-missing-defaults"; test-period = <3210>;
            zephyr,deferred-init;
        };
    };
    missing_owner {
        compatible = "keypaw,rgb-overlay";
        missing_private: missing_private {
            compatible = "test,rgb-missing-defaults"; test-period = <3210>;
        };
    };
    missing_owner_wide {
        compatible = "keypaw,rgb-overlay";
        missing_private_wide: missing_private_wide {
            compatible = "test,rgb-missing-defaults"; test-period = <70000>;
        };
    };
    missing_owner_negative {
        compatible = "keypaw,rgb-overlay";
        missing_private_negative: missing_private_negative {
            compatible = "test,rgb-missing-defaults"; test-period = <(-65536)>;
        };
    };
    missing_owner_minus_one {
        compatible = "keypaw,rgb-overlay";
        missing_private_minus_one: missing_private_minus_one {
            compatible = "test,rgb-missing-defaults"; test-period = <(-1)>;
        };
    };
    default_disabled {
        missing_disabled: missing_disabled {
            compatible = "test,rgb-missing-defaults"; test-period = <3210>;
        };
    };
};
'''


def run_variant(root, settings):
    root = root / ("settings" if settings else "no-settings")
    root.mkdir()
    config = root / "config"
    config.mkdir()
    (config / "native_sim.keymap").write_text((SIM / "config/native_sim.keymap").read_text())
    (config / "native_sim.conf").write_text(
        (SIM / "config/native_sim.conf").read_text() +
        "\nCONFIG_ASSERT=y\n"
        "CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS=100\n"
        "CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS=10000\n"
        f"CONFIG_SETTINGS={'y' if settings else 'n'}\n" +
        ("CONFIG_SETTINGS_CUSTOM=y\n" if settings else ""))
    (config / "native_sim.overlay").write_text(
        f'#include "{SIM / "config/native_sim.overlay"}"\n' + OVERLAY)
    plugin = root / "defaults-module"
    (plugin / "zephyr").mkdir(parents=True)
    (plugin / "zephyr/module.yml").write_text(
        "name: rgb-defaults-test\nbuild:\n  cmake: .\n  settings:\n    dts_root: .\n")
    (plugin / "dts/bindings").mkdir(parents=True)
    (plugin / "dts/bindings/test,rgb-default-effect.yaml").write_text(
        'description: Defaults test effect\ncompatible: "test,rgb-default-effect"\n'
        'include: "keypaw,rgb-matrix-effect-common.yaml"\n'
        'properties:\n  zephyr,deferred-init:\n    type: boolean\n')
    (plugin / "dts/bindings/test,rgb-missing-defaults.yaml").write_text(
        'description: Extension without color or duration properties\n'
        'compatible: "test,rgb-missing-defaults"\ninclude: "base.yaml"\n'
        'properties:\n  test-period:\n    type: int\n    required: true\n'
        '  zephyr,deferred-init:\n    type: boolean\n')
    (plugin / "CMakeLists.txt").write_text(
        "zephyr_library()\nzephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)\n"
        f'zephyr_library_sources("{SIM / "selector_defaults.c"}")\n')
    command = shlex.split(WEST) + ["build", "-s", "app", "-d", str(root / "build"),
               "-b", "native_sim//zmk_test_mock", "--", f"-DZMK_CONFIG={config}",
               f"-DZMK_EXTRA_MODULES={MODULE};{SIM / 'module'};{plugin}"]
    result = run_logged(command, cwd=WORKSPACE, text=True, stdout=subprocess.PIPE,
                        env={**os.environ, "ZEPHYR_TOOLCHAIN_VARIANT": "host"},
                        stderr=subprocess.STDOUT, timeout=600, log_path=root / "build.log")
    assert result.returncode == 0, result.stdout[-16000:]
    assert not re.search(r"warning:[^\n]*(overflow|changes value)", result.stdout), result.stdout[-16000:]
    result = run_logged([str(root / "build/zephyr/zmk.exe"), "--stop_at=2", "-no-rt"],
                            cwd=root, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=30, log_path=root / "runtime.log")
    assert result.returncode == 0, result.stdout
    assert "PASS selector defaults" in result.stdout, result.stdout
    print(result.stdout, end="")

def main():
    root = Path(tempfile.mkdtemp(prefix="rgb-selector-defaults-"))
    print(f"ARTIFACTS {root}", flush=True)
    for settings in (False, True):
        run_variant(root, settings)


if __name__ == "__main__":
    main()
