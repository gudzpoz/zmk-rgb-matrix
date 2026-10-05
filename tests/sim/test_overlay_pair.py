#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Run the two-child overlay fixture and assert its callback behavior."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

SIM = Path(__file__).resolve().parent
MODULE = SIM.parent.parent
REPO = MODULE.parent.parent
WORKSPACE = Path(os.environ.get("ZMK_WS", REPO / "zmk"))
LOCAL_WEST = REPO / ".venv/bin/west"
WEST = os.environ.get("WEST", str(LOCAL_WEST) if LOCAL_WEST.is_file() else "west")

PAIR = '''
&rgb_overlays { fixture_pair: fixture_pair {
    compatible = "keypaw,rgb-overlay-pair";
    condition = <&fixture_always>;
    all-leds;
    first { compatible = "keypaw,rgb-matrix-reactive"; #binding-cells = <0>;
        color = <0x20ff40>; duration = <650>; };
    second { compatible = "keypaw,rgb-matrix-rainbow"; #binding-cells = <0>;
        duration = <1800>; };
}; };
'''


def main():
    with tempfile.TemporaryDirectory(prefix="rgb-pair-") as directory:
        root = Path(directory)
        config = root / "config"
        config.mkdir()
        for filename in ("native_sim.conf", "native_sim.keymap"):
            shutil.copyfile(SIM / "config" / filename, config / filename)
        (config / "native_sim.overlay").write_text(
            f'#include "{SIM / "config/native_sim.overlay"}"\n' + PAIR
        )
        build = root / "build"
        command = [WEST, "build", "-s", "app", "-d", str(build),
                   "-b", "native_sim//zmk_test_mock", "-p", "--",
                   f"-DZMK_CONFIG={config}",
                   f"-DZMK_EXTRA_MODULES={MODULE};{SIM / 'module'}"]
        subprocess.run(command, cwd=WORKSPACE, check=True)

        capture = root / "pair.bin"
        run = subprocess.run(
            [str(build / "zephyr/zmk.exe"), "--effect=0", f"--capture={capture}",
             "--stop_at=4", "-no-rt"],
            cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            check=True,
        )
        log = run.stdout
        assert capture.is_file() and capture.stat().st_size > 1000, "no useful capture"
        assert "rgb-pair restart both" in log, "owner did not restart both child clocks"
        for child in ("first", "second"):
            assert re.search(rf"rgb-pair {child} demand=[01]", log), \
                f"child {child} was never rendered\n{log[-8000:]}"
        assert "rgb-pair second demand=1" in log, "continuous child did not request animation"
        assert "rgb-pair first demand=1" in log, "finite child did not animate after input"
        assert "rgb-pair first demand=0" in log, "finite child never settled"
        print("PASS two-child overlay runtime")


if __name__ == "__main__":
    main()
