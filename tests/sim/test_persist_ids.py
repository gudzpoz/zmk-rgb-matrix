#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Validate selector persistence identities in real DTS and settings-disabled startup."""
from pathlib import Path
import re
import subprocess
import tempfile

from test_selector import SELECTORS, build

SELECTOR_IDS = SELECTORS.replace("keypaw,rgb-matrix-selector-probe",
                                 "keypaw,rgb-matrix-solid").replace(" animate;", "")

def main():
    root = Path(tempfile.mkdtemp(prefix="rgb-persist-ids-"))
    print(f"ARTIFACTS {root}", flush=True)
    cases = {
        "scoped-duplicates": ('''
&selector_a { persist-id = "main-a"; disabled { persist-id = "same"; }; };
&selector_b { persist-id = "main_a"; };
&probe_a { persist-id = "same"; };
&probe_b { persist-id = "same"; };
''', None),
        "path-boundary": ('''
&selector_a { persist-id = "sssssssssssss"; };
&probe_a { persist-id = "eeeeeeeeeeeeee"; };
''', None),
        "selection-only-long-id": (
            '&selector_a { persist-id = "' + 's' * 30 + '"; };', None),
        "duplicate-selector": (
            '&selector_a { persist-id = "same"; }; &selector_b { persist-id = "same"; };',
            "must be globally unique"),
        "duplicate-member": (
            '&probe_a { persist-id = "same"; }; &probe_c { persist-id = "same"; };',
            "persistence IDs invalid"),
        "empty-id": ('&selector_a { persist-id = ""; };', "persistence IDs invalid"),
        "slash-id": ('&probe_a { persist-id = "bad/id"; };', "persistence IDs invalid"),
        "separator-id": ('&probe_a { persist-id = "bad=id"; };', "persistence IDs invalid"),
        "selector-path-overflow": (
            '&selector_a { persist-id = "' + 's' * 31 + '"; };', "persistence IDs invalid"),
        "parameter-path-overflow": ('''
&selector_a { persist-id = "ssssssssssssss"; };
&probe_a { persist-id = "eeeeeeeeeeeeee"; };
''', "persistence IDs invalid"),
    }
    for name, (fragment, diagnostic) in cases.items():
        result = build(root, name, fragment, selectors=SELECTOR_IDS)
        assert result.returncode == 0, result.stdout[-16000:]
        run = subprocess.run(
            [str(root / name / "build/zephyr/zmk.exe"), "--stop_at=1", "-no-rt"],
            cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=30)
        (root / f"{name}.log").write_text(run.stdout)
        assert run.returncode == 0, run.stdout
        if diagnostic:
            assert diagnostic in run.stdout, run.stdout
        else:
            assert "<err>" not in run.stdout, run.stdout
        print(f"PASS persistence IDs {name}", flush=True)

    for target in ("selector_a", "probe_a"):
        name = f"overlong-{target}"
        result = build(root, name, f'&{target} {{ persist-id = "' + 'x' * 32 + '"; };', selectors=SELECTOR_IDS)
        assert result.returncode != 0, result.stdout[-16000:]
        errors = "\n".join(line for line in result.stdout.splitlines() if "error:" in line)
        assert re.search(r"KP_RGB_PERSIST_MAX_ID_LENGTH", errors), result.stdout[-16000:]
        print(f"PASS persistence IDs {name}", flush=True)


if __name__ == "__main__":
    main()
