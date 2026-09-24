#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Render a native_sim RGB matrix capture into a GIF.

Reads the file written by the keypaw,led-strip-capture driver (see
tests/sim/module/src/kp_rgb_sim.c) and turns it into an animated GIF, drawing
one square per LED at the position the firmware itself computed for it.

Only the standard library is used; ffmpeg does the GIF encoding. The capture
format is:

    header  "KPRC" | u16 version | u16 led_count | led_count * (u16 x, u16 y)
    frames  u32 uptime_ms | led_count * 3 bytes (r, g, b) in chain order

integers little-endian, matching the host native_sim runs on.
"""

import argparse
import shutil
import struct
import subprocess
import sys
import tempfile

MAGIC = b"KPRC"
VERSION = 1
DEFAULT_FPS = 31.25  # the module's 32 ms tick
DEFAULT_TILE = 32


def read_capture(path):
    with open(path, "rb") as handle:
        data = handle.read()

    if len(data) < 8:
        raise ValueError("capture file is truncated (no header)")

    magic, version, led_count = struct.unpack_from("<4sHH", data, 0)
    if magic != MAGIC:
        raise ValueError(f"bad magic {magic!r}, expected {MAGIC!r}")
    if version != VERSION:
        raise ValueError(f"unsupported capture version {version}")
    if led_count == 0:
        raise ValueError("capture declares zero LEDs")

    offset = 8
    coords = []
    for _ in range(led_count):
        if offset + 4 > len(data):
            raise ValueError("capture file is truncated (layout)")
        coords.append(struct.unpack_from("<HH", data, offset))
        offset += 4

    frame_size = 4 + led_count * 3
    frames = []
    while offset + frame_size <= len(data):
        (timestamp,) = struct.unpack_from("<I", data, offset)
        pixels = data[offset + 4 : offset + 4 + led_count * 3]
        frames.append((timestamp, pixels))
        offset += frame_size

    if not frames:
        raise ValueError("capture file contains no frames")

    trailing = len(data) - offset
    if trailing:
        print(f"warning: ignoring {trailing} trailing bytes", file=sys.stderr)

    return coords, frames


def infer_cell(coords):
    """Smallest positive spacing between LEDs, in layout units.

    Layout units are 100 per key width, but derive it rather than assume, so a
    fixture with a coarser or finer grid still renders sensibly.
    """
    for axis in (0, 1):
        values = sorted({c[axis] for c in coords})
        deltas = [b - a for a, b in zip(values, values[1:]) if b > a]
        if deltas:
            return min(deltas)
    return 100


def infer_fps(frames):
    if len(frames) < 2:
        return DEFAULT_FPS
    deltas = sorted(
        b[0] - a[0] for a, b in zip(frames, frames[1:]) if b[0] > a[0]
    )
    if not deltas:
        return DEFAULT_FPS
    median = deltas[len(deltas) // 2]
    return 1000.0 / median if median > 0 else DEFAULT_FPS


def rasterize(coords, frames, cell, tile, gap):
    """Draw one square per LED, inset by `gap` so the keys stay distinct.

    Without the inset, adjacent LEDs share an edge and a uniform effect renders
    as one solid block rather than a keyboard-shaped grid.
    """
    gap = max(0, min(gap, tile - 1))
    inner = tile - gap
    pad = gap // 2

    cols = max(c[0] // cell for c in coords) + 1
    rows = max(c[1] // cell for c in coords) + 1
    width, height = cols * tile, rows * tile

    # Precompute each LED's pixel rectangle once.
    rects = [
        ((c[0] // cell) * tile + pad, (c[1] // cell) * tile + pad) for c in coords
    ]

    stream = bytearray()
    for _, pixels in frames:
        canvas = bytearray(width * height * 3)
        for led, (px, py) in enumerate(rects):
            r = pixels[led * 3]
            g = pixels[led * 3 + 1]
            b = pixels[led * 3 + 2]
            row = bytes((r, g, b)) * inner
            for y in range(py, py + inner):
                start = (y * width + px) * 3
                canvas[start : start + inner * 3] = row
        stream += canvas

    return width, height, bytes(stream)


def encode_gif(raw_path, width, height, fps, out_path):
    # Single pass: split the stream so palettegen sees every frame before
    # paletteuse quantizes against it. A plain GIF encode looks muddy on the
    # saturated colours these effects produce.
    filter_complex = (
        "[0:v]split[a][b];"
        "[a]palettegen=stats_mode=diff[p];"
        "[b][p]paletteuse=dither=bayer:bayer_scale=3"
    )
    subprocess.run(
        [
            "ffmpeg",
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "rawvideo",
            "-pixel_format",
            "rgb24",
            "-video_size",
            f"{width}x{height}",
            "-framerate",
            f"{fps:.6f}",
            "-i",
            raw_path,
            "-filter_complex",
            filter_complex,
            "-loop",
            "0",
            out_path,
        ],
        check=True,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", help="capture.bin written by the sim")
    parser.add_argument("-o", "--output", required=True, help="GIF to write")
    parser.add_argument(
        "--tile",
        type=int,
        default=DEFAULT_TILE,
        help=f"pixels per LED (default {DEFAULT_TILE})",
    )
    parser.add_argument(
        "--gap",
        type=int,
        default=None,
        help="pixels of dark border around each LED (default: tile / 8)",
    )
    parser.add_argument(
        "--min-distinct",
        type=int,
        default=0,
        metavar="N",
        help="warn when the capture holds fewer than N distinct frames "
        "(0 disables the check). A preview that never changes is usually a "
        "bug, though `solid` and `static` are static by design.",
    )
    parser.add_argument(
        "--fps",
        type=float,
        default=None,
        help="override the frame rate inferred from the frame timestamps",
    )
    args = parser.parse_args()

    if shutil.which("ffmpeg") is None:
        sys.exit("ffmpeg not found on PATH; it is needed to encode the GIF")

    try:
        coords, frames = read_capture(args.capture)
    except (OSError, ValueError) as err:
        sys.exit(f"error: {args.capture}: {err}")

    cell = infer_cell(coords)
    fps = args.fps if args.fps else infer_fps(frames)
    gap = args.gap if args.gap is not None else max(1, args.tile // 8)
    width, height, stream = rasterize(coords, frames, cell, args.tile, gap)

    with tempfile.NamedTemporaryFile(suffix=".rgb") as raw:
        raw.write(stream)
        raw.flush()
        encode_gif(raw.name, width, height, fps, args.output)

    duration = (frames[-1][0] - frames[0][0]) / 1000.0
    distinct = len({pixels for _, pixels in frames})
    print(
        f"{args.output}: {len(frames)} frames, {distinct} distinct, "
        f"{width}x{height}, {fps:.1f} fps, {duration:.1f}s of simulated time"
    )

    if 0 < args.min_distinct and distinct < args.min_distinct:
        print(
            f"warning: {args.output} has only {distinct} distinct frame(s) of "
            f"{len(frames)}; the effect may be static (expected for solid/static)",
            file=sys.stderr,
        )


if __name__ == "__main__":
    main()
