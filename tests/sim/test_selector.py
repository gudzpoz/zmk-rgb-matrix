#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Build selector validation cases and check real engine callbacks and pixels."""
import argparse
from pathlib import Path
import re
import struct
import subprocess
import tempfile

from native_process import run_logged
from native_build import SELECTORS, build

CASES = {
    "empty": ('/delete-node/ disabled; /delete-node/ a_static; /delete-node/ a_motion;',
              r"selector requires at least one enabled nested effect"),
    "all-disabled": ('a_static { status = "disabled"; }; a_motion { status = "disabled"; };',
                     r"selector requires at least one enabled nested effect"),
    "slot-overflow": ('initial-effect = <65536>;', r"selector initial-effect must fit a local slot"),
    "slot-negative": ('initial-effect = <(-1)>;', r"selector initial-effect must fit a local slot"),
}


CONTROL_CASES = {
    "missing-control": ('&rgbs_a { status = "disabled"; };',
                        r"RGB selector requires exactly one enabled canonical control"),
    "duplicate-control": ('''/ { behaviors { rgbs_dup {
        compatible = "keypaw,behavior-rgb-selector"; #binding-cells = <2>;
        selector = <&selector_a>; }; }; };''',
                          r"RGB selector requires exactly one enabled canonical control"),
    "wrong-kind-control": ('''/ { behaviors { badctl {
        compatible = "keypaw,behavior-rgb-selector"; #binding-cells = <2>;
        selector = <&ordinary>; }; }; };''',
                           r"RGB selector control must reference a selector overlay"),
    "disabled-control-target": ('''&rgb_matrix { disabled_selector: disabled_selector {
        compatible = "keypaw,rgb-overlay-selector"; status = "disabled";
        child { compatible = "keypaw,rgb-matrix-solid"; #binding-cells = <0>; };
        }; }; / { behaviors { badctl {
        compatible = "keypaw,behavior-rgb-selector"; #binding-cells = <2>;
        selector = <&disabled_selector>; }; }; };''',
                               r"RGB selector control must reference an enabled selector"),
    "removed-global-alias": ('&rgb_matrix { selector = <&ordinary>; };',
                             r"['\"]selector['\"].*not declared"),
    "oversized-control": ('&rgbs_a { label = "control_name_too_long"; };',
                          r"RGB selector control name must fit the 9-byte split behavior_dev field"),
    "oversized-global": ('&rgb_matrix { label = "global_name_too_long"; };',
                         r"RGB controller name must fit the split behavior_dev field"),
}


# Conditions precede their selector/effect targets in the generated DTS.
SELECTED_CONDITIONS = '''
#include <dt-bindings/keypaw/rgb_matrix.h>
&rgb_conditions {
    selected_a: selected_a {
        compatible = "keypaw,rgb-condition-effect-selected";
        selector = <&selector_a>; effect = <&probe_a>;
    };
    selected_c: selected_c {
        compatible = "keypaw,rgb-condition-effect-selected";
        selector = <&selector_a>; effect = <&probe_c>;
    };
    event_selected: event_selected {
        compatible = "test,rgb-selector-event-condition";
        selector = <&selector_a>; effect = <&probe_a>;
    };
};
'''
SELECTED_GATES = '''
&ordinary { condition = <&selected_a>; local; };
&rgb_matrix {
    shared_gate: shared_gate {
        compatible = "keypaw,rgb-overlay"; local;
        condition = <&selected_a>; leds = <4>;
        white_s { compatible = "keypaw,rgb-matrix-solid";
            #binding-cells = <0>; color = <0xffffff>; };
    };
    central_gate: central_gate {
        compatible = "keypaw,rgb-overlay";
        condition = <&selected_a>; leds = <5>;
        white_c { compatible = "keypaw,rgb-matrix-solid";
            #binding-cells = <0>; color = <0xffffff>; };
    };
    opposite_gate: opposite_gate {
        compatible = "keypaw,rgb-overlay"; local;
        condition = <&selected_c>; leds = <7>;
        cyan { compatible = "keypaw,rgb-matrix-solid";
            #binding-cells = <0>; color = <0x00ffff>; };
    };
    event_gate: event_gate {
        compatible = "keypaw,rgb-overlay"; local;
        condition = <&event_selected>; leds = <6>;
        white_e { compatible = "keypaw,rgb-matrix-solid";
            #binding-cells = <0>; color = <0xffffff>; };
    };
};
'''
SELECTED_CASES = {
    "missing-selector": ('&selected_a { /delete-property/ selector; };',
                         r"['\"]selector['\"].*required|requires.*['\"]selector['\"]"),
    "missing-effect": ('&selected_a { /delete-property/ effect; };',
                       r"['\"]effect['\"].*required|requires.*['\"]effect['\"]"),
    "wrong-selector": ('&selected_a { selector = <&ordinary>; };',
                       r'static assertion failed: "selected-effect condition requires an enabled selector overlay"'),
    "disabled-selector": ('&selector_b { status = "disabled"; }; '
                          '&rgbs_b { status = "disabled"; }; '
                          '&selected_a { selector = <&selector_b>; };',
                          r'static assertion failed: "selected-effect condition requires an enabled selector overlay"'),
    "wrong-effect": ('&selected_a { effect = <&fixture_always>; };',
                     r'static assertion failed: "selected-effect condition requires an enabled effect member of its selector"'),
    "disabled-effect": ('''&selector_a { disabled_probe: disabled_probe {
        compatible = "keypaw,rgb-matrix-solid"; #binding-cells = <0>;
        status = "disabled"; }; };
        &selected_a { effect = <&disabled_probe>; };''',
                        r'static assertion failed: "selected-effect condition requires an enabled effect member of its selector"'),
    "foreign-member": ('&selected_a { effect = <&probe_b>; };',
                       r'static assertion failed: "selected-effect condition requires an enabled effect member of its selector"'),
    "private-member": ('&selected_a { effect = <&private_white>; };',
                       r'static assertion failed: "selected-effect condition requires an enabled effect member of its selector"'),
    "self-gate": ('&selector_a { condition = <&selected_a>; };',
                  r"(?i)cycle in devicetree involving .*selector_a"),
}


def selected_runtime(root, name, fragment="", selectors=SELECTORS, extra_conf=""):
    result = build(root, name, SELECTED_GATES + fragment, runtime=True,
                   selectors=SELECTED_CONDITIONS + selectors, extra_conf=extra_conf)
    assert result.returncode == 0, result.stdout[-16000:]
    capture = root / f"{name}.bin"
    command = [str(root / name / "build/zephyr/zmk.exe"),
               f"--capture={capture}", "--stop_at=5", "-no-rt"]
    print("COMMAND", " ".join(command), flush=True)
    run = run_logged(command, cwd=root, text=True, stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT, log_path=root / f"{name}.log")
    assert run.returncode == 0, run.stdout[-16000:]
    for marker in (
        "PASS selector selected condition initial/shared/local/central gates",
        "PASS selector condition third-party event/refresh barrier/newer publication",
        "PASS selector selected condition hidden/OFF/inhibited notifications",
    ):
        assert marker in run.stdout, run.stdout
    assert "<err>" not in run.stdout, run.stdout
    if name == "selected-zero":
        assert "PASS selector selected condition zero LEDs" in run.stdout, run.stdout
    else:
        check_selected_capture(capture, run.stdout, peripheral=name == "selected-peripheral")
    print(f"PASS {name}", flush=True)


def check_selected_capture(path, log, peripheral=False):
    data = path.read_bytes()
    magic, version, count = struct.unpack_from("<4sHH", data)
    assert (magic, version, count) == (b"KPRC", 1, 48)
    stride, offset = 4 + count * 3, 8 + count * 4
    assert (len(data) - offset) % stride == 0
    frames = [(struct.unpack_from("<I", data, pos)[0], data[pos + 4:pos + stride])
              for pos in range(offset, len(data), stride)]
    phases = dict((name, int(time)) for name, time in
                  re.findall(r"selector phase (\w+) at (\d+)", log))
    for phase, selected in (("condition_initial", True), ("condition_changed", False),
                            ("condition_resumed", True)):
        pixels = max((f for f in frames if f[0] <= phases[phase]), key=lambda f: f[0])[1]
        assert pixels[:3] == bytes((0, 255, 0) if selected else (0, 0, 255)), phase
        for led in (1, 4, 5, 6):
            expected = (255, 255, 255) if selected else ((0, 0, 255) if led == 1 else (255, 0, 0))
            if peripheral and led == 5:
                expected = (255, 0, 0)
            assert pixels[led * 3:led * 3 + 3] == bytes(expected), (phase, led, pixels)
        assert pixels[21:24] == bytes((255, 0, 0) if selected else (0, 255, 255)), phase




def check_capture(path, log):
    data = path.read_bytes()
    magic, version, count = struct.unpack_from("<4sHH", data)
    assert (magic, version, count) == (b"KPRC", 1, 48)
    stride = 4 + count * 3
    offset = 8 + count * 4
    assert (len(data) - offset) % stride == 0
    frames = [(struct.unpack_from("<I", data, pos)[0], data[pos + 4:pos + stride])
              for pos in range(offset, len(data), stride)]
    phases = dict((name, int(time)) for name, time in
                  re.findall(r"selector phase (\w+) at (\d+)", log))
    colors = {
        "initial": ((0, 255, 0), (255, 255, 0)),
        "animated": ((0, 0, 255), (255, 255, 0)),
        "shown": ((0, 255, 0), (0, 255, 255)),
        "on": ((0, 0, 255), (255, 255, 0)),
        "resumed": ((0, 0, 255), (255, 255, 0)),
    }
    for phase, (first, second) in colors.items():
        time = phases[phase]
        pixels = max((frame for frame in frames if frame[0] <= time), key=lambda f: f[0])[1]
        assert pixels[:6] == bytes(first) + bytes((255, 255, 255)), (phase, pixels[:6])
        assert pixels[6:12] == bytes(second) * 2, (phase, pixels[6:12])
        assert pixels[12:] == bytes((255, 0, 0)) * 44, phase
    for phase in ("off", "inhibited"):
        pixels = max((frame for frame in frames if frame[0] <= phases[phase]), key=lambda f: f[0])[1]
        assert not any(pixels), phase
    for phase in ("initial", "shown", "hidden", "off", "inhibited"):
        assert not any(phases[phase] < time < phases[phase] + 190 for time, _ in frames), phase
    assert any(phases["animated"] < time < phases["animated"] + 190 for time, _ in frames)
    print(f"PASS selector composition/demand ({len(frames)} captured frames)")


def main():
    # Keep logs/captures for inspection; all west directories are unique per invocation.
    root = Path(tempfile.mkdtemp(prefix="rgb-selector-"))
    print(f"ARTIFACTS {root}", flush=True)
    for name, (fragment, diagnostic) in CASES.items():
        result = build(root, name, f"&selector_a {{ {fragment} }};")
        assert result.returncode != 0, name
        assert re.search(diagnostic, result.stdout), result.stdout[-12000:]
        print(f"PASS selector configuration {name}", flush=True)
    for name, (fragment, diagnostic) in CONTROL_CASES.items():
        result = build(root, name, fragment)
        assert result.returncode != 0, name
        assert re.search(diagnostic, result.stdout), result.stdout[-12000:]
        print(f"PASS selector configuration {name}", flush=True)
    long_selectors = SELECTORS.replace("selector_a: selector_a {",
                                      'selector_a: ' + 's' * 30 + ' {')
    result = build(root, "long-overlay", "", runtime=True, selectors=long_selectors)
    assert result.returncode == 0, result.stdout[-12000:]
    command = [str(root / "long-overlay/build/zephyr/zmk.exe"), "--stop_at=6", "-no-rt"]
    print("COMMAND", " ".join(command), flush=True)
    run = run_logged(command, cwd=root, text=True, stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT, log_path=root / "long-overlay.log")
    assert run.returncode == 0 and "PASS selector runtime" in run.stdout, run.stdout
    assert "<err>" not in run.stdout, run.stdout
    print("PASS long overlay with short canonical control", flush=True)
    result = build(root, "runtime", "", runtime=True)
    assert result.returncode == 0, result.stdout[-12000:]
    capture = root / "selector.bin"
    command = [str(root / "runtime/build/zephyr/zmk.exe"), f"--capture={capture}",
               "--stop_at=6", "-no-rt"]
    print("COMMAND", " ".join(command), flush=True)
    run = run_logged(command, cwd=root, text=True, stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT, log_path=root / "runtime.log")
    assert run.returncode == 0 and "PASS selector runtime" in run.stdout, run.stdout[-12000:]
    assert "PASS selector public API/notifications/coalescing" in run.stdout, run.stdout
    ordered = ["a_static active=0", "a_motion reset", "a_motion active=1",
               "a_motion input before renders=0", "a_motion render elapsed=0 demand=1"]
    positions = [run.stdout.index(f"selector child {entry}") for entry in ordered]
    assert positions == sorted(positions), run.stdout
    assert "<err>" not in run.stdout, run.stdout
    assert re.search(r"render elapsed=[1-9]\d* demand=1", run.stdout), run.stdout
    print("PASS selector lifecycle/input/retained selection/clock restart")
    check_capture(capture, run.stdout)


    result = build(root, "late-control", '''&rgbs_a {
        compatible = "test,rgb-selector-late-control", "keypaw,behavior-rgb-selector";
        zephyr,deferred-init;
        };''', runtime=True)
    assert result.returncode == 0, result.stdout[-12000:]
    command = [str(root / "late-control/build/zephyr/zmk.exe"), "--stop_at=6", "-no-rt"]
    print("COMMAND", " ".join(command), flush=True)
    run = run_logged(command, cwd=root, text=True, stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT, log_path=root / "late-control.log")
    assert run.returncode == 0, run.stdout[-12000:]
    assert "PASS canonical readiness automatic start" in run.stdout, run.stdout
    assert "PASS selector runtime" in run.stdout, run.stdout
    assert "<err>" not in run.stdout, run.stdout
    print("PASS canonical readiness automatic start", flush=True)

    stage42(root)


def stage42(root):
    for name, (fragment, diagnostic) in SELECTED_CASES.items():
        result = build(root, f"selected-{name}", fragment,
                       selectors=SELECTED_CONDITIONS + SELECTORS)
        assert result.returncode != 0, name
        # Match actual compiler/DT diagnostics, not a source line echoed in a traceback.
        errors = "\n".join(line for line in result.stdout.splitlines()
                           if "error:" in line or "Error" in line or "Exception:" in line)
        assert re.search(diagnostic, errors), result.stdout[-16000:]
        print(f"PASS selected condition configuration {name}", flush=True)
    selected_runtime(root, "selected-runtime")
    selected_runtime(root, "selected-peripheral",
                     '&preview_selector { local; }; &selector_a { local; }; &selector_b { local; };', extra_conf=
                     "CONFIG_ZMK_SPLIT=y\nCONFIG_ZMK_SPLIT_ROLE_CENTRAL=n\n")
    selected_runtime(root, "selected-zero", '''
&rgb_matrix { /delete-property/ strip; mapping = <>; };
&selector_a { /delete-property/ leds; };
&selector_b { /delete-property/ leds; };
&ordinary { /delete-property/ leds; };
&shared_gate { /delete-property/ leds; };
&central_gate { /delete-property/ leds; };
&event_gate { /delete-property/ leds; };
&opposite_gate { /delete-property/ leds; };
''')
    result = build(root, "selected-trigger", SELECTED_GATES + '''
/ { selected_trigger: selected_trigger {
    compatible = "keypaw,rgb-trigger-table";
    reset_selection { condition = <&selected_c>;
        on-enter = <&rgbs_a RGB_RESET_CMD 0>; };
}; };
''', runtime=True, selectors=SELECTED_CONDITIONS + SELECTORS,
                   extra_conf="CONFIG_KEYPAW_RGB_TRIGGERS=y\n")
    assert result.returncode == 0, result.stdout[-16000:]
    run = run_logged([str(root / "selected-trigger/build/zephyr/zmk.exe"),
                      "--stop_at=2", "-no-rt"], cwd=root, text=True,
                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                     log_path=root / "selected-trigger.log")
    assert run.returncode == 0, run.stdout
    assert "PASS selector condition trigger selection during refresh" in run.stdout, run.stdout
    assert "<err>" not in run.stdout, run.stdout
    print("PASS selected condition trigger selection during refresh", flush=True)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage42-only", action="store_true",
                        help="Run only selected-effect condition DT and runtime cases")
    args = parser.parse_args()
    if args.stage42_only:
        root = Path(tempfile.mkdtemp(prefix="rgb-selector-stage42-"))
        print(f"ARTIFACTS {root}", flush=True)
        stage42(root)
    else:
        main()
