#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Host tests for timestamp-aware capture previews; no firmware build needed."""

import importlib.util
import pathlib
import shutil
import struct
import subprocess
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "sim" / "render_gif.py"
SPEC = importlib.util.spec_from_file_location("render_gif", SCRIPT)
preview = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(preview)
RED, GREEN, BLUE = b"\xff\x00\x00", b"\x00\xff\x00", b"\x00\x00\xff"


class PreviewTimingTests(unittest.TestCase):
    def pixels(self, frames, **kwargs):
        return [pixels for _, pixels in preview.resample_frames(frames, **kwargs)]

    def test_irregular_timestamps_hold_until_next_sample(self):
        frames = [(100, RED), (125, GREEN), (180, BLUE)]
        self.assertEqual(self.pixels(frames, end_ms=200),
                         [RED] * 3 + [GREEN] * 5 + [BLUE] * 2)
        self.assertEqual(self.pixels(frames, fps=50, end_ms=200),
                         [RED] * 2 + [GREEN] * 2 + [BLUE])

    def test_one_frame_final_duration(self):
        self.assertEqual(self.pixels([(500, RED)]), [RED])
        self.assertEqual(self.pixels([(500, RED)], end_ms=600), [RED] * 10)
        self.assertEqual(self.pixels([(500, RED)], end_ms=500), [RED])
        self.assertEqual(self.pixels([(500, RED)], end_ms=511), [RED] * 2)
        self.assertEqual(self.pixels([(500, RED)], fps=25), [RED])

    def test_default_end_holds_last_interval(self):
        self.assertEqual(self.pixels([(0, RED), (25, BLUE)]),
                         [RED] * 3 + [BLUE])

    def test_equal_timestamps_last_wins(self):
        self.assertEqual(self.pixels([(0, RED), (0, GREEN), (10, RED), (10, BLUE)]),
                         [GREEN, BLUE])

    def test_invalid_timing(self):
        for fps in [0, -1, float("nan"), float("inf"), -float("inf")]:
            with self.subTest(fps=fps), self.assertRaises(ValueError):
                preview.resample_frames([(0, RED)], fps=fps)
        for end in [9, float("nan"), float("inf")]:
            with self.subTest(end=end), self.assertRaises(ValueError):
                preview.resample_frames([(10, RED)], end_ms=end)
        with self.assertRaises(ValueError):
            preview.resample_frames([(10, RED), (9, BLUE)])

    def test_capture_api_and_streaming_rasters(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = pathlib.Path(directory) / "capture.bin"
            capture.write_bytes(struct.pack("<4sHHHH", b"KPRC", 1, 1, 0, 0)
                                + struct.pack("<I", 20) + RED)
            coords, frames = preview.read_capture(capture)
        self.assertEqual(coords, [(0, 0)])
        self.assertEqual(frames, [(20, RED)])
        width, height, images = preview.rasterize(
            coords, preview.resample_frames(frames, end_ms=50), 100, 1, 0)
        self.assertEqual((width, height), (1, 1))
        self.assertIs(iter(images), images)
        self.assertEqual([bytes(image) for image in images], [RED] * 3)

    @unittest.skipUnless(shutil.which("ffmpeg") and shutil.which("ffprobe"),
                         "ffmpeg and ffprobe are optional")
    def test_encoded_duration(self):
        with tempfile.TemporaryDirectory() as directory:
            for name, frames, end_ms in [
                ("held", [(100, RED), (225, BLUE)], 500),
                ("single", [(100, RED)], 500),
                ("default", [(100, RED)], None),
            ]:
                with self.subTest(name=name):
                    raw = pathlib.Path(directory) / (name + ".rgb")
                    gif = pathlib.Path(directory) / (name + ".gif")
                    width, height, images = preview.rasterize(
                        [(0, 0)], preview.resample_frames(frames, end_ms=end_ms),
                        100, 4, 0)
                    with raw.open("wb") as handle:
                        for image in images:
                            handle.write(image)
                    preview.encode_gif(str(raw), width, height, 100, str(gif))
                    duration = float(subprocess.check_output([
                        "ffprobe", "-v", "error", "-min_delay", "0",
                        "-show_entries", "format=duration", "-of",
                        "default=noprint_wrappers=1:nokey=1", str(gif)], text=True))
                    self.assertAlmostEqual(duration, 0.01 if end_ms is None else 0.4,
                                           delta=0.001)


if __name__ == "__main__":
    unittest.main()
