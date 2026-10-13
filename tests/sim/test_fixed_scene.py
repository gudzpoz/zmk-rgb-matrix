#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Exercise selector-free composition and live gates with and without local LEDs."""
from pathlib import Path
import re
import struct
import subprocess
import tempfile

from test_selector import build

SCENE = '''
&rgb_overlays {
    /delete-node/ preview_selector;
    /delete-node/ fixture_overlay;
    /delete-node/ fixture_battery;
    /delete-node/ fixture_batt_sub;
    status_panel: status_panel {
        compatible = "keypaw,rgb-overlay";
        all-leds;
        condition = <&fixture_latch>;
        status_color { compatible = "keypaw,rgb-matrix-solid";
            #binding-cells = <0>; color = <0x804020>; };
    };
};
/ { behaviors { /delete-node/ rgbs; }; };
&kprgb {
    /delete-property/ selector;
    /delete-property/ initial-on;
    initial-brightness = <10>; max-brightness = <20>; idle-brightness = <5>;
};
&rgb_matrix {
    initial-on;
    initial-brightness = <50>; max-brightness = <75>; idle-brightness = <25>;
};
&kscan { events = <>; };
'''


def main():
    root = Path(tempfile.mkdtemp(prefix="rgb-fixed-scene-"))
    print(f"ARTIFACTS {root}", flush=True)
    for name, extra in (("status", ""), ("zero", '''
&rgb_matrix { /delete-property/ strip; mapping = <>; };
''')):
        result = build(root, name, extra, runtime=True, selectors=SCENE,
                       extra_conf="CONFIG_KEYPAW_SIM_SELECTOR_TEST=n\n"
                                  "CONFIG_KEYPAW_SIM_FIXED_SCENE_TEST=y\n")
        assert result.returncode == 0, result.stdout[-16000:]
        capture = root / f"{name}.bin"
        command = [str(root / name / "build/zephyr/zmk.exe"),
                   f"--capture={capture}", "--stop_at=2", "-no-rt"]
        run = subprocess.run(command, cwd=root, text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=30)
        (root / f"{name}.log").write_text(run.stdout)
        assert run.returncode == 0 and "PASS fixed scene" in run.stdout, run.stdout
        assert "<err>" not in run.stdout, run.stdout
        data = capture.read_bytes()
        if name == "zero":
            assert data == b"", data
            assert "LEDs=0" in run.stdout, run.stdout
            print("PASS fixed scene zero: no submissions/live OFF/inhibited gates", flush=True)
            continue
        magic, version, count = struct.unpack_from("<4sHH", data)
        assert (magic, version, count) == (b"KPRC", 1, 48)
        stride, offset = 4 + count * 3, 8 + count * 4
        assert (len(data) - offset) % stride == 0
        frames = [(struct.unpack_from("<I", data, pos)[0], data[pos + 4:pos + stride])
                  for pos in range(offset, len(data), stride)]
        if name == "status":
            phases = dict((phase, int(time)) for phase, time in
                          re.findall(r"fixed scene phase (\w+) at (\d+)", run.stdout))
            assert set(phases) == {"empty", "status", "off", "inhibited", "resumed", "empty_again"}, phases
            for phase, time in phases.items():
                pixels = max((frame for frame in frames if frame[0] <= time),
                             key=lambda frame: frame[0])[1]
                # HSB-rounded preset is (127, 63, 31); floor(75 * 50 / 100) is 37%.
                expected = bytes((46, 23, 11)) if phase in ("status", "resumed") else bytes(3)
                assert pixels == expected * count, (phase, pixels)
        print(f"PASS fixed scene {name}: matrix defaults/live OFF/inhibited gates", flush=True)


if __name__ == "__main__":
    main()
