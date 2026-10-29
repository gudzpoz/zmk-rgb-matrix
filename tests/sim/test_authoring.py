#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Compile documented setup snippets and exercise fixed-scene runtime behavior."""
import re
import struct
import subprocess

from native_process import run_logged
from native_build import MODULE, SIM, WEST, WORKSPACE, build, native_build_root

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


def snippet(text, heading, index=0):
    section = text.split(heading, 1)[1]
    return re.findall(r"```dts\n(.*?)```", section, re.S)[index]


def guide_overlay(text):
    section = snippet(text, "## Configuring an overlay")
    # The custom condition provider is supplied by the README's latch example.
    body = section.split("&rgb_matrix {", 1)[1]
    return "&rgb_matrix {" + body.replace("condition = <&cond_rec>;", "condition = <&cond_batt>;")


def build_documented(root):
    readme = (MODULE / "README.md").read_text()
    guide = (MODULE / "docs/extending.md").read_text()
    scene = snippet(readme, "<!-- documented-setup-scene -->")
    indicators = snippet(readme, "### Optional overlay configurations")
    bindings = snippet(readme, "<!-- documented-setup-bindings -->")
    guide_scene = guide_overlay(guide)
    assert re.search(r"zone:\s*zone\s*\{[^}]*\bcontrollable;", scene, re.S)
    assert "&zone " in bindings and "&rgbs " not in bindings
    assert 'compatible = "keypaw,rgb-overlay"' in guide_scene
    assert "condition = <&cond_batt>;" in guide_scene

    variants = (
        ("documented-readme", scene + indicators, bindings),
        ("documented-guide", scene + indicators + guide_scene, bindings),
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


def check_fixed_scene(root):
    cases = (
        ("status", "", False),
        ("api-only-zero", API_ONLY + '''
&rgb_matrix { /delete-property/ strip; mapping = <>; };
''', False),
        ("api-only-split-local", API_ONLY + '&status_panel { local; };', True),
    )
    for name, extra, split_local in cases:
        extra_conf = "CONFIG_KEYPAW_SIM_SELECTOR_TEST=n\nCONFIG_KEYPAW_SIM_FIXED_SCENE_TEST=y\n"
        if split_local:
            extra_conf += "CONFIG_ZMK_SPLIT=y\nCONFIG_ZMK_SPLIT_ROLE_CENTRAL=n\n"
        result = build(root, name, extra, runtime=True, selectors=SCENE,
                       extra_conf=extra_conf)
        assert result.returncode == 0, result.stdout[-16000:]
        config = (root / name / "build/zephyr/.config").read_text()
        assert ("CONFIG_KEYPAW_RGB_TRIGGERS=y" in config) == (name == "api-only-zero"), config
        assert "CONFIG_KEYPAW_RGB_SPLIT_SYNC=y" not in config, config
        assert "CONFIG_KEYPAW_RGB_MATRIX=y" in config, config

        capture = root / f"{name}.bin"
        command = [str(root / name / "build/zephyr/zmk.exe"),
                   f"--capture={capture}", "--stop_at=2", "-no-rt"]
        run = run_logged(command, cwd=root, text=True, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, log_path=root / f"{name}.log")
        assert run.returncode == 0 and "PASS fixed scene" in run.stdout, run.stdout
        assert "<err>" not in run.stdout, run.stdout
        data = capture.read_bytes()
        if name == "api-only-zero":
            assert data == b"", data
            assert "LEDs=0" in run.stdout, run.stdout
            print("PASS fixed scene api-only-zero: no submissions/live OFF/inhibited gates", flush=True)
            continue

        magic, version, count = struct.unpack_from("<4sHH", data)
        assert (magic, version, count) == (b"KPRC", 1, 48)
        stride, offset = 4 + count * 3, 8 + count * 4
        assert (len(data) - offset) % stride == 0
        frames = [(struct.unpack_from("<I", data, pos)[0], data[pos + 4:pos + stride])
                  for pos in range(offset, len(data), stride)]
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


def main():
    root = native_build_root(None, "authoring", prefix="rgb-authoring-")
    print(f"ARTIFACTS {root}", flush=True)
    build_documented(root)
    check_fixed_scene(root)


if __name__ == "__main__":
    main()
