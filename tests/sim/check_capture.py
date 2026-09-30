#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Assert the smoke-test pixel invariants on a native_sim capture.

The scenario is scripted in tests/sim/config-smoke/native_sim.overlay and run by
tests/sim/run-smoke.sh. The file format is shared with the GIF renderer, so
read_capture() is reused rather than duplicated.

Pixel index == LED index == chain index: the fixture's `mapping` is the identity
0..47 and the capture driver writes pixels in chain order.

The checks are ordered rather than time-windowed: event and render timing shift
with the simulated clock, but the sequence of states cannot.
"""

import sys

from render_gif import read_capture

RED = (255, 0, 0)
BLACK = (0, 0, 0)
GREEN = (0, 255, 0)
BLUE = (0, 0, 255)
# Reactive idles at its 10% background over white: hsb b=10, s=0 -> 10*255/100.
REACTIVE_IDLE = (25, 25, 25)
FLASH_LED = 24  # RC(2,0) -> position 24


def tri(pixels, led):
    return (pixels[led * 3], pixels[led * 3 + 1], pixels[led * 3 + 2])


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)


def is_uniform(pixels, n, value):
    return all(tri(pixels, i) == value for i in range(n))


def max_channel(pixels, led):
    return max(tri(pixels, led))


class Capture:
    def __init__(self, frames, n):
        self.frames = frames
        self.n = n

    def uniform(self, value):
        return lambda f: is_uniform(f[1], self.n, value)

    def led_below(self, led, value):
        return lambda f: max_channel(f[1], led) < value

    def find(self, start, pred):
        """Index of the first frame at or after `start` matching `pred`, or -1."""
        for i in range(start, len(self.frames)):
            if pred(self.frames[i]):
                return i
        return -1


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: check_capture.py <capture.bin>")

    coords, frames = read_capture(sys.argv[1])
    capture = Capture(frames, len(coords))

    # Boot: the initial effect is solid red at full brightness.
    check(
        is_uniform(frames[0][1], capture.n, RED),
        f"boot frame is not red: {tri(frames[0][1], 0)}",
    )

    i_off = capture.find(1, capture.uniform(BLACK))
    check(i_off > 0, "no all-black frame after the RGB_OFF press")

    i_on = capture.find(i_off + 1, capture.uniform(RED))
    check(i_on > i_off, "no red frame after the RGB_ON press")

    i_idle = capture.find(i_on + 1, capture.uniform(REACTIVE_IDLE))
    check(i_idle > i_on, f"no {REACTIVE_IDLE} frame after selecting reactive")

    # The key press flashes that key's LED at (near) full white.
    peak, i_peak = 0, -1
    for i in range(i_idle, len(frames)):
        value = max_channel(frames[i][1], FLASH_LED)
        if value > peak:
            peak, i_peak = value, i
    check(peak > 200, f"reactive flash on LED {FLASH_LED} too dim (peak {peak})")

    i_decay = capture.find(i_peak + 1, capture.led_below(FLASH_LED, 100))
    check(i_decay > i_peak, f"LED {FLASH_LED} never decayed after the flash")

    i_layer = capture.find(i_decay + 1, capture.uniform(GREEN))
    check(i_layer > i_decay, "no green frame while layer 1 is held")

    # Caps Lock is a condition with no layer event, so only the trigger table can
    # react to it (the engine samples the table every tick). Its rising edge
    # recolours solid blue and its on-exit restores red when Caps Lock clears.
    i_blue = capture.find(i_layer + 1, capture.uniform(BLUE))
    check(i_blue > i_layer, "no blue frame after Caps Lock turned on")

    i_red2 = capture.find(i_blue + 1, capture.uniform(RED))
    check(i_red2 > i_blue, "no red frame after Caps Lock turned off")

    print(
        f"smoke ok: {len(frames)} frames, {capture.n} LEDs; "
        "off/on/reactive/flash/decay/layer/caps-lock all asserted"
    )


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, ValueError, OSError) as err:
        sys.exit(f"error: {err}")
