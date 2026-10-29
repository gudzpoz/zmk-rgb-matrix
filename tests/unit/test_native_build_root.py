#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Root selection for retained native harness artifacts."""
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sim"))
from native_build import native_build_root


class NativeBuildRootTests(unittest.TestCase):
    def test_explicit_path_precedes_environment_and_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            explicit = base / "explicit"
            marker = explicit / "keep.txt"
            marker.parent.mkdir()
            marker.write_text("retained")
            with patch.dict(os.environ, {"RGB_NATIVE_BUILD_ROOT": str(base / "env")}):
                selected = native_build_root(explicit, "suite", prefix="unused-")
            self.assertEqual(selected, explicit.resolve())
            self.assertEqual(marker.read_text(), "retained")
            self.assertFalse((base / "env").exists())

    def test_environment_root_isolated_by_suite_and_retained(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory) / "artifacts"
            with patch.dict(os.environ, {"RGB_NATIVE_BUILD_ROOT": str(base)}):

                first = native_build_root(None, "contracts", prefix="unused-")
                second = native_build_root(None, "smoke", prefix="unused-")
                self.assertEqual(first, (base / "contracts").resolve())
                self.assertEqual(second, (base / "smoke").resolve())
                self.assertTrue(first.is_dir())
                self.assertTrue(second.is_dir())
                self.assertNotEqual(first, second)
                marker = first / "keep.txt"
                marker.write_text("retained")
                self.assertEqual(marker.read_text(), "retained")
                reused = native_build_root(None, "contracts", prefix="unused-")
                self.assertEqual(reused, first)
                self.assertEqual((reused / "keep.txt").read_text(), "retained")

    def test_default_temporary_roots_preserve_prefix_and_parent(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.dict(os.environ, {}, clear=True):
                parent = Path(directory) / "native-artifacts"
                first = native_build_root(None, "geometry", prefix="geometry-", parent=parent)
                second = native_build_root(None, "geometry", prefix="geometry-", parent=parent)
                self.assertNotEqual(first, second)
                self.assertEqual(first.parent, parent.resolve())
                self.assertEqual(second.parent, parent.resolve())
                self.assertTrue(first.name.startswith("geometry-"))
                marker = first / "build.log"
                marker.write_text("retained")
                self.assertEqual(marker.read_text(), "retained")
    def test_legacy_default_is_used_without_environment(self):
        with tempfile.TemporaryDirectory() as directory:
            default = Path(directory) / "legacy"
            with patch.dict(os.environ, {}, clear=True):
                selected = native_build_root(None, "suite", default=default)
            self.assertEqual(selected, default.resolve())
            self.assertFalse(selected.exists())


if __name__ == "__main__":
    unittest.main()
