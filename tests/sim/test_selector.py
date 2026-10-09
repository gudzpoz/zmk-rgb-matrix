#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Build selector validation cases and check real engine callbacks and pixels."""
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

SIM = Path(__file__).resolve().parent
MODULE = SIM.parent.parent
REPO = MODULE.parent.parent
WORKSPACE = Path(os.environ.get("ZMK_WS", REPO / "zmk"))
LOCAL_WEST = REPO / ".venv/bin/west"
WEST = os.environ.get("WEST", str(LOCAL_WEST) if LOCAL_WEST.is_file() else "west")

SELECTORS = '''
&rgb_overlays {
    /delete-node/ fixture_overlay;
    /delete-node/ fixture_battery;
    /delete-node/ fixture_batt_sub;
    selector_a: selector_a {
        compatible = "keypaw,rgb-overlay-selector";
        leds = <0 1>;
        initial-effect = <0>;
        disabled { compatible = "keypaw,rgb-matrix-solid";
            #binding-cells = <0>; status = "disabled"; };
        probe_a: a_static { compatible = "keypaw,rgb-matrix-selector-probe";
            #binding-cells = <0>; color = <0x00ff00>; };
        probe_c: a_motion { compatible = "keypaw,rgb-matrix-selector-probe";
            #binding-cells = <0>; color = <0x0000ff>; animate; };
    };
    selector_b: selector_b {
        compatible = "keypaw,rgb-overlay-selector";
        condition = <&fixture_always>;
        leds = <2 3>;
        initial-effect = <99>;
        initial-duration-ms = <0xffffffff>;
        probe_b: b_static { compatible = "keypaw,rgb-matrix-selector-probe";
            #binding-cells = <0>; color = <0xffff00>; };
        probe_d: b_alt { compatible = "keypaw,rgb-matrix-selector-probe";
            #binding-cells = <0>; color = <0x00ffff>; duration = <0>; };
    };
    ordinary: ordinary {
        compatible = "keypaw,rgb-overlay";
        leds = <1>;
        white { compatible = "keypaw,rgb-matrix-solid"; #binding-cells = <0>; color = <0xffffff>; };
    };
};
&fx_solid_v { no-overlays; };
&kscan { events = <>; };
'''

CASES = {
    "empty": ('/delete-node/ disabled; /delete-node/ a_static; /delete-node/ a_motion;',
              r"selector requires at least one enabled nested effect"),
    "all-disabled": ('a_static { status = "disabled"; }; a_motion { status = "disabled"; };',
                     r"selector requires at least one enabled nested effect"),
    "slot-overflow": ('initial-effect = <65536>;', r"selector initial-effect must fit a local slot"),
    "slot-negative": ('initial-effect = <(-1)>;', r"selector initial-effect must fit a local slot"),
}


def build(root, name, fragment, runtime=False):
    config = root / name / "config"
    config.mkdir(parents=True)
    for filename in ("native_sim.conf", "native_sim.keymap"):
        (config / filename).write_text((SIM / "config" / filename).read_text())
    if runtime:
        with (config / "native_sim.conf").open("a") as output:
            output.write("\nCONFIG_KEYPAW_SIM_SELECTOR_TEST=y\nCONFIG_ASSERT=y\n")
    (config / "native_sim.overlay").write_text(
        f'#include "{SIM / "config/native_sim.overlay"}"\n' + SELECTORS + fragment)
    command = [WEST, "build", "-s", "app", "-d", str(root / name / "build"),
               "-b", "native_sim//zmk_test_mock", "-p", "--",
               f"-DZMK_CONFIG={config}", f"-DZMK_EXTRA_MODULES={MODULE};{SIM / 'module'}"]
    print("COMMAND", " ".join(command), flush=True)
    result = subprocess.run(command, cwd=WORKSPACE, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (root / f"{name}-build.log").write_text(result.stdout)
    return result


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
    result = build(root, "runtime", "", runtime=True)
    assert result.returncode == 0, result.stdout[-12000:]
    capture = root / "selector.bin"
    command = [str(root / "runtime/build/zephyr/zmk.exe"), f"--capture={capture}",
               "--stop_at=4", "-no-rt"]
    print("COMMAND", " ".join(command), flush=True)
    run = subprocess.run(command, cwd=root, text=True, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
    (root / "runtime.log").write_text(run.stdout)
    assert run.returncode == 0 and "PASS selector runtime" in run.stdout, run.stdout[-12000:]
    ordered = ["a_static active=0", "a_motion reset", "a_motion active=1",
               "a_motion input before renders=0", "a_motion render elapsed=0 demand=1"]
    positions = [run.stdout.index(f"selector child {entry}") for entry in ordered]
    assert positions == sorted(positions), run.stdout
    assert "<err>" not in run.stdout, run.stdout
    assert re.search(r"render elapsed=[1-9]\d* demand=1", run.stdout), run.stdout
    print("PASS selector lifecycle/input/retained selection/clock restart")
    check_capture(capture, run.stdout)


if __name__ == "__main__":
    main()
