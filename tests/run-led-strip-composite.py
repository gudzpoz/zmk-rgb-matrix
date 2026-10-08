#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT

import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys


MODULE = Path(__file__).resolve().parents[1]
APP = MODULE / "tests" / "led_strip_composite"
REPO = MODULE.parents[1]
CONTROL = '''/ {
    spec_child: spec-child {
        compatible = "test,composite-mock-strip";
        chain-length = <2>;
    };
    spec_bad: spec-composite {
        compatible = "zmk,led-strip-composite";
        strips = <&spec_child>;
        chain-length = <2>;
    };
};
'''
CASES = {
    "missing-length": (
        "&spec_bad { /delete-property/ chain-length; };",
        r"'chain-length'.*(required|missing)|required.*'chain-length'",
    ),
    "missing-strips": (
        "&spec_bad { /delete-property/ strips; };",
        r"'strips'.*(required|missing)|required.*'strips'",
    ),
    "empty-strips": (
        "&spec_bad { strips = <>; };",
        r"composite strips must not be empty|strips.*(empty|nonempty|non-empty)",
    ),
    "zero-length": (
        "&spec_bad { chain-length = <0>; };",
        r"composite chain-length must be positive",
    ),
    "negative-length": (
        "&spec_bad { chain-length = <(-1)>; };",
        r"composite chain-length must be positive",
    ),
    "zero-child-length": (
        "&spec_child { chain-length = <0>; };",
        r"composite child chain-length must be positive",
    ),
    "negative-child-length": (
        "&spec_child { chain-length = <(-1)>; };",
        r"composite child chain-length must be positive",
    ),
    "sum-mismatch": (
        "&spec_bad { chain-length = <3>; };",
        r"composite chain-length must equal the sum of its strips",
    ),
    "overflowing-sum": (
        '''/ { spec_extra: spec-extra {
            compatible = "test,composite-mock-strip";
            chain-length = <2147483647>;
        }; spec_tail: spec-tail {
            compatible = "test,composite-mock-strip";
            chain-length = <4>;
        }; };
        &spec_child { chain-length = <2147483647>; };
        &spec_bad { strips = <&spec_child &spec_extra &spec_tail>; };''',
        r"composite chain-length must equal the sum of its strips",
    ),
    "disabled-child": (
        '&spec_child { status = "disabled"; };',
        r"composite strips must be enabled",
    ),
    "self-reference": (
        "&spec_bad { strips = <&spec_bad>; };",
        r"composite strip must not reference itself|cycle in devicetree",
    ),
    "duplicate-child": (
        "&spec_bad { strips = <&spec_child &spec_child>; chain-length = <4>; };",
        r"redeclaration of enumerator.*composite_child_",
    ),
    "cycle": (
        '''/ { spec_other: spec-other {
            compatible = "zmk,led-strip-composite";
            strips = <&spec_bad>;
            chain-length = <2>;
        }; };
        &spec_bad { strips = <&spec_other>; };''',
        r"cycle in devicetree involving.*spec-(composite|other)|pasting.*dts_ord_.*valid preprocessing token",
    ),
}


def execute(command, cwd, log, timeout=180):
    try:
        result = subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        output = error.stdout or b""
        if isinstance(output, bytes):
            output = output.decode(errors="replace")
        log.write_text(output)
        raise RuntimeError(f"timeout (not an expected rejection): {log}") from error
    log.write_text(result.stdout)
    return result


def config_has(text, name, value):
    return f"CONFIG_{name}={value}\n" in text


def main():
    parser = argparse.ArgumentParser(description="Pristine standalone composite LED strip tests")
    parser.add_argument("--only", choices=("all", "runtime", "negative"), default="all")
    parser.add_argument("--build-dir", type=Path, default=APP / "build")
    args = parser.parse_args()
    workspace = Path(os.environ.get("ZMK_WS", REPO / "zmk")).resolve()
    zephyr = workspace / "zephyr"
    west = shlex.split(os.environ.get("WEST", str(REPO / ".venv/bin/west")
                                   if (REPO / ".venv/bin/west").exists() else "west"))
    if not (zephyr / "CMakeLists.txt").is_file() or not (workspace / ".west").is_dir():
        raise RuntimeError(f"ZMK_WS must be an initialized west workspace: {workspace}")
    if not (MODULE / "src/drivers/led_strip_composite.c").is_file():
        raise RuntimeError("Production src/drivers/led_strip_composite.c is missing")
    if not west or shutil.which(west[0]) is None:
        raise RuntimeError(f"WEST command is unavailable: {west}")
    root = args.build_dir.resolve()
    root.mkdir(parents=True, exist_ok=True)

    for scratch in (False, True):
        variant = root / ("scratch-on" if scratch else "scratch-off")
        variant.mkdir(parents=True, exist_ok=True)
        conf = variant / "scratch.conf"
        conf.write_text(f"CONFIG_COMPOSITE_TEST_RGB_SCRATCH={'y' if scratch else 'n'}\n")

        def build(name, mutation=""):
            overlay = variant / f"{name}.overlay"
            overlay.write_text(CONTROL + mutation + "\n")
            build_dir = variant / name
            command = west + ["build", "-p", "always", "-b", "native_sim/native/64",
                              "-s", str(APP), "-d", str(build_dir), "--",
                              f"-DZEPHYR_BASE={zephyr}",
                              f"-DZEPHYR_EXTRA_MODULES={MODULE}",
                              f"-DDTC_OVERLAY_FILE={APP / 'app.overlay'};{overlay}",
                              f"-DEXTRA_CONF_FILE={conf}"]
            log = variant / f"{name}.build.log"
            result = execute(command, workspace, log)
            return result, build_dir, log

        result, build_dir, log = build("positive-control")
        if result.returncode:
            raise RuntimeError(f"positive control build failed (not a rejection): {log}\n"
                               + result.stdout[-5000:])
        config = (build_dir / "zephyr/.config").read_text()
        for name, value in (("LED_STRIP_COMPOSITE", "y"),
                            ("LED_STRIP_COMPOSITE_INIT_PRIORITY", "91")):
            if not config_has(config, name, value):
                raise RuntimeError(f"expected {name}={value}: {build_dir / 'zephyr/.config'}")
        if config_has(config, "KEYPAW_RGB_MATRIX", "y"):
            raise RuntimeError("standalone control unexpectedly enables the RGB engine")
        if config_has(config, "LED_STRIP_RGB_SCRATCH", "y") != scratch:
            raise RuntimeError(f"RGB scratch layout not selected correctly: {conf}")
        print(f"PASS pristine positive control scratch={scratch}", flush=True)

        if args.only != "negative":
            log = variant / "runtime.log"
            result = execute([str(build_dir / "zephyr/zephyr.exe")], workspace, log, 30)
            if result.returncode or "PROJECT EXECUTION SUCCESSFUL" not in result.stdout:
                raise RuntimeError(f"runtime tests failed: {log}\n" + result.stdout[-5000:])
            print(f"PASS runtime ztests scratch={scratch}", flush=True)

        if args.only != "runtime":
            for name, (mutation, diagnostic) in CASES.items():
                result, rejected_build, log = build(name, mutation)
                if result.returncode == 0:
                    raise RuntimeError(f"invalid fixture unexpectedly built: {name} ({log})")
                if not re.search(diagnostic, result.stdout, re.IGNORECASE):
                    raise RuntimeError(f"wrong rejection for {name}: expected {diagnostic!r}; {log}\n"
                                       + result.stdout[-5000:])
                # Zephyr 4.1 can emit cyclic nodes with ordinal -1 instead of a DTS error.
                if name == "cycle" and "cycle in devicetree" not in result.stdout:
                    generated = (rejected_build / "zephyr/include/generated/zephyr/devicetree_generated.h").read_text()
                    for node in ("spec_composite", "spec_other"):
                        if not re.search(rf"^#define DT_N_S_{node}_ORD -1$", generated, re.MULTILINE):
                            raise RuntimeError(f"cycle rejection lacks the expected invalid ordinal for {node}: {log}")
                print(f"PASS expected rejection {name} scratch={scratch}", flush=True)
    print(f"PASS composite LED strip tests; logs: {root}")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
