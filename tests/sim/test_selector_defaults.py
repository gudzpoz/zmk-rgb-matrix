#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Native effect initialization, public setters, deferred readiness and reset."""
from pathlib import Path
import shlex
import subprocess
import tempfile

from native_process import run_logged
from native_build import MODULE, SIM, WEST, WORKSPACE

OVERLAY = '''
&kscan { events = <>; };
&rgb_overlays {
    default_selector: default_selector {
        compatible = "keypaw,rgb-overlay-selector";
        initial-duration-ms = <2345>;
        default_ready: ready {
            compatible = "test,rgb-default-effect"; #binding-cells = <0>;
            color = <0x008000>; duration = <0>;
        };
        default_explicit: explicit {
            compatible = "test,rgb-default-effect"; #binding-cells = <0>;
            color = <0x000080>; duration = <0xffffffff>;
        };
    };
    default_late_selector: default_late_selector {
        compatible = "keypaw,rgb-overlay-selector";
        initial-duration-ms = <0>;
        default_late: late {
            compatible = "test,rgb-default-effect"; #binding-cells = <0>;
            color = <0x808000>; zephyr,deferred-init;
        };
    };
    default_private_owner: default_private_owner {
        compatible = "keypaw,rgb-overlay";
        default_private: private {
            compatible = "test,rgb-default-effect"; #binding-cells = <0>;
            color = <0x800000>; duration = <0>;
        };
    };
    default_disabled: default_disabled {
        compatible = "keypaw,rgb-overlay-selector"; status = "disabled";
        initial-duration-ms = <2345>;
        default_disabled_child: child {
            compatible = "test,rgb-default-effect"; #binding-cells = <0>;
            color = <0x800000>; duration = <0>;
        };
    };
};
/ { behaviors {
    defctl { compatible = "keypaw,behavior-rgb-selector";
        #binding-cells = <2>; selector = <&default_selector>; };
    latectl { compatible = "keypaw,behavior-rgb-selector";
        #binding-cells = <2>; selector = <&default_late_selector>; };
}; };
'''


def run_variant(root, settings):
    root = root / ("settings" if settings else "no-settings")
    root.mkdir()
    config = root / "config"
    config.mkdir()
    (config / "native_sim.keymap").write_text((SIM / "config/native_sim.keymap").read_text())
    (config / "native_sim.conf").write_text(
        (SIM / "config/native_sim.conf").read_text() +
        "\nCONFIG_KEYPAW_RGB_CONTROL_AUTO_START=n\n"
        "CONFIG_ASSERT=y\n"
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
        'include: [zero_param.yaml, "keypaw,rgb-matrix-effect-common.yaml"]\n'
        'properties:\n  zephyr,deferred-init:\n    type: boolean\n')
    (plugin / "CMakeLists.txt").write_text(
        "zephyr_library()\nzephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)\n"
        f'zephyr_library_sources("{SIM / "selector_defaults.c"}")\n')
    command = shlex.split(WEST) + ["build", "-s", "app", "-d", str(root / "build"),
               "-b", "native_sim//zmk_test_mock", "--", f"-DZMK_CONFIG={config}",
               f"-DZMK_EXTRA_MODULES={MODULE};{SIM / 'module'};{plugin}"]
    result = run_logged(command, cwd=WORKSPACE, text=True, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, timeout=600, log_path=root / "build.log")
    assert result.returncode == 0, result.stdout[-16000:]
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
