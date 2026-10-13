#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile actual README/guide snippets using native_sim and the capture module.

Run from any directory; ZMK_WS and WEST use the selector suite's conventions.
Artifacts and timeout logs are retained in the printed temporary directory.
This is a configuration compilation test, not a radio or persistence test.
"""
from pathlib import Path
import re
import subprocess
import tempfile

from native_process import run_logged
from native_build import MODULE, SIM, WEST, WORKSPACE


def snippet(text, heading, index=0):
    section = text.split(heading, 1)[1]
    return re.findall(r"```dts\n(.*?)```", section, re.S)[index]


def main():
    readme = (MODULE / "README.md").read_text()
    guide = (MODULE / "docs/extending.md").read_text()
    scene = snippet(readme, "<!-- documented-setup-scene -->")
    indicators = snippet(readme, "### Optional overlay configurations")
    bindings = snippet(readme, "<!-- documented-setup-bindings -->")
    selectors = snippet(guide, "### Effect selectors")
    guide_bindings = snippet(guide, "### Effect selectors", 1)
    condition = snippet(guide, "#### Selected-effect condition")
    root = Path(tempfile.mkdtemp(prefix="rgb-documented-setup-"))
    print(f"ARTIFACTS {root}", flush=True)
    variants = (
        ("readme", scene + indicators, bindings),
        ("readme-alias", scene + indicators + '\n&kprgb { selector = <&zone>; };\n',
         bindings.replace("&rgbs RGB_EFF", "&kprgb RGB_EFF")),
        ("guide", scene + indicators + selectors + '/ {\n' + condition + '};\n'
         '&caps { condition = <&selected_a>; };\n', guide_bindings),
    )
    for name, overlay, keys in variants:
        config = root / name / "config"
        config.mkdir(parents=True)
        (config / "native_sim.conf").write_text((SIM / "config/native_sim.conf").read_text())
        (config / "native_sim.overlay").write_text(
            f'#include "{SIM / "config/documented_setup.overlay"}"\n' + overlay)
        # Pad only the hardware's unused positions; command cells remain verbatim.
        count = keys.count("&")
        assert 0 < count <= 48
        keys = keys.replace(">;", " " + "&none " * (48 - count) + ">;")
        (config / "native_sim.keymap").write_text(
            '#include <behaviors.dtsi>\n#include <dt-bindings/keypaw/rgb_matrix.h>\n'
            '/ { keymap { compatible = "zmk,keymap"; default_layer {\n'
            + keys + '\n}; }; };\n')
        command = [WEST, "build", "-s", "app", "-d", str(root / name / "build"),
                   "-b", "native_sim//zmk_test_mock", "-p", "--",
                   f"-DZMK_CONFIG={config}",
                   f"-DZMK_EXTRA_MODULES={MODULE};{SIM / 'module'}"]
        print("COMMAND", " ".join(command), flush=True)
        log = root / f"{name}-build.log"
        result = run_logged(command, cwd=WORKSPACE, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=600, log_path=log)
        assert result.returncode == 0, result.stdout[-16000:]
        assert (root / name / "build/zephyr/zmk.exe").is_file()
        print(f"PASS documented configuration {name}", flush=True)


if __name__ == "__main__":
    main()
