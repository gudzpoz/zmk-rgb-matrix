#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Exercise selector-free composition and live gates with and without local LEDs."""
from pathlib import Path
import re
import struct
import subprocess
import tempfile

from native_process import run_logged
from native_build import build

SCENE = '''
&rgb_matrix {
    /delete-node/ preview_selector;
    /delete-node/ fixture_overlay;
    /delete-node/ fixture_battery;
    /delete-node/ fixture_batt_sub;
    status_panel: status_panel {
        compatible = "keypaw,rgb-overlay";
        all-leds;
        condition = <&fixture_latch>;
        status_color: status_color { compatible = "keypaw,rgb-matrix-solid";
            color = <0x804020>; };
    };
};
/ { behaviors { /delete-node/ rgbs; }; };
&rgb_matrix {
    initial-on;
    initial-brightness = <50>; max-brightness = <75>; idle-brightness = <25>;
};
&kscan { events = <>; };
'''

API_ONLY = '''
/ { status_triggers {
    compatible = "keypaw,rgb-trigger-table";
    status_edge {
        condition = <&fixture_latch>;
        on-enter = <&none>;
    };
}; };
'''


def main():
    root = Path(tempfile.mkdtemp(prefix="rgb-fixed-scene-"))
    print(f"ARTIFACTS {root}", flush=True)
    for name, extra in (("status", ""), ("zero", '''
&rgb_matrix { /delete-property/ strip; mapping = <>; };
'''), ("api-only", API_ONLY),
                        ("api-only-split-local", API_ONLY + '&status_panel { local; };'),
                        ("api-only-zero", API_ONLY + '''
&rgb_matrix { /delete-property/ strip; mapping = <>; };
''')):
        result = build(root, name, extra, runtime=True, selectors=SCENE,
                       extra_conf="CONFIG_KEYPAW_SIM_SELECTOR_TEST=n\n"
                                  "CONFIG_KEYPAW_SIM_FIXED_SCENE_TEST=y\n" +
                                  ("CONFIG_KEYPAW_RGB_TRIGGERS=y\n" if name.startswith("api-only") else "") +
                                  ("CONFIG_ZMK_SPLIT=y\nCONFIG_ZMK_SPLIT_ROLE_CENTRAL=n\n" if "split" in name else ""))
        assert result.returncode == 0, result.stdout[-16000:]
        if name.startswith("api-only"):
            config = (root / name / "build/zephyr/.config").read_text()
            if "split" not in name:
                assert "CONFIG_KEYPAW_RGB_TRIGGERS=y" in config, config
            assert "CONFIG_KEYPAW_RGB_MATRIX=y" in config, config
        capture = root / f"{name}.bin"
        command = [str(root / name / "build/zephyr/zmk.exe"),
                   f"--capture={capture}", "--stop_at=2", "-no-rt"]
        run = run_logged(command, cwd=root, text=True, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, log_path=root / f"{name}.log")
        assert run.returncode == 0 and "PASS fixed scene" in run.stdout, run.stdout
        assert "<err>" not in run.stdout, run.stdout
        data = capture.read_bytes()
        if name in ("zero", "api-only-zero"):
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
        if name in ("status", "api-only", "api-only-split-local"):
            phases = dict((phase, int(time)) for phase, time in
                          re.findall(r"fixed scene phase (\w+) at (\d+)", run.stdout))
            assert set(phases) == {"empty", "status", "off", "inhibited", "resumed", "empty_again"}, phases
            for phase, time in phases.items():
                pixels = max((frame for frame in frames if frame[0] <= time),
                             key=lambda frame: frame[0])[1]
                # Preset B50 gives RGB127/63/31; cap75 * master50 gives scale37.
                expected = bytes((46, 23, 11)) if phase in ("status", "resumed") else bytes(3)
                assert pixels == expected * count, (phase, pixels)
        print(f"PASS fixed scene {name}: matrix defaults/live OFF/inhibited gates", flush=True)


if __name__ == "__main__":
    main()
