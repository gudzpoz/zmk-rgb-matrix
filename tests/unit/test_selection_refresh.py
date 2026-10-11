#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Compose production selection publication, condition control and refresh barriers.

The C fixture replaces only platform/event boundaries and scene/trigger consumers.
It deliberately does not model strip output, retries, or input delivery.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

from test_singleton import function, structure

ROOT = Path(__file__).resolve().parents[2]
CASES = ("initial-shared", "scoped-listener", "third-party", "coalesced",
         "reentrant", "trigger-mutation", "resume-held", "external-interruption",
         "external-unused-interruption")


def run_tests():
    matrix = (ROOT / "src/rgb_matrix.c").read_text()
    selector = (ROOT / "src/rgb_selector_api.c").read_text()
    provider = (ROOT / "src/conditions/effect_selected.c").read_text()
    public = (ROOT / "include/zmk/rgb_matrix.h").read_text()
    api = public[public.index("enum kp_rgb_condition_scope {"):public.index(
        "#include <zmk/rgb_matrix_condition_internal.h>")]
    private = (ROOT / "include/zmk/rgb_matrix_condition_internal.h").read_text()
    private = re.sub(r'^#(?:include|pragma)[^\n]*\n', '', private, flags=re.M)
    coordinator = (ROOT / "src/rgb_conditions.c").read_text()
    coordinator = re.sub(r'^#include[^\n]*\n', '', coordinator, flags=re.M)
    refresh = "\n".join(function(matrix, name) for name in (
        "kp_rgb_control_refresh_locked", "kp_rgb_control_refresh_hold_locked",
        "kp_rgb_control_refresh_release_locked", "kp_rgb_conditions_refreshed"))
    # Observe real coordinator completion attempts before the real barrier accepts them.
    refresh = refresh.replace("void kp_rgb_conditions_refreshed(",
                              "void apply_completion(")
    publication = selector[selector.index("static struct kp_rgb_selector *notification_head;"):
                           selector.index("int zmk_rgb_selector_select(")]
    invalidate = provider[provider.index("#define INVALIDATE(inst)"):
                          provider.index("#define DEFINE(inst)")]
    selected = (function(provider, "sample")
                + "\n" + invalidate + "\n" + function(provider, "listener"))
    fixture = (ROOT / "tests/unit/selection_refresh_harness.c").read_text()
    source = (fixture.replace("/* API */", api + private + structure(
        (ROOT / "src/rgb_selector_internal.h").read_text(), "kp_rgb_selector") + structure(provider, "selected_cfg"))
        .replace("/* REFRESH */", refresh).replace("/* COORDINATOR */", coordinator)
        .replace("/* PUBLICATION */", publication).replace("/* PROVIDER */", selected))
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory)
        c = path / "selection_refresh.c"
        c.write_text(source)
        for central in (0, 1):
            binary = path / "selection_refresh"
            subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
                "-std=c11", "-Wall", "-Wextra", "-Werror",
                f"-DCONFIG_ZMK_SPLIT_ROLE_CENTRAL={central}", str(c),
                "-o", str(binary)], check=True)
            for case in CASES:
                subprocess.run([str(binary), case], check=True, timeout=10)


if __name__ == "__main__":
    run_tests()
