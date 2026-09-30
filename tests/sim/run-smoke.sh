#!/usr/bin/env bash
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
#
# Smoke-test the RGB matrix module on native_sim: build the config-smoke
# fixture, run the scripted key scenario, and assert the captured pixel
# invariants (tests/sim/check_capture.py). Also checks that a capture failure
# fails the run instead of producing a silent empty artifact.
#
# Environment:
#   ZMK_WS  west workspace root  (default: <repo>/zmk)
#   WEST    west executable      (default: <repo>/.venv/bin/west, else $PATH)

set -euo pipefail

SIM_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MODULE_ROOT=$(cd "$SIM_DIR/../.." && pwd)
REPO_ROOT=$(cd "$MODULE_ROOT/../.." && pwd)

ZMK_WS=${ZMK_WS:-$REPO_ROOT/zmk}
if [ -n "${WEST:-}" ]; then
    :
elif [ -x "$REPO_ROOT/.venv/bin/west" ]; then
    WEST=$REPO_ROOT/.venv/bin/west
else
    WEST=west
fi

# A separate build dir: Zephyr caches the config dir, and this fixture uses a
# different ZMK_CONFIG than the preview harness.
BUILD_DIR=$SIM_DIR/build-smoke
OUT_DIR=${OUT_DIR:-$SIM_DIR/out-smoke}
DURATION=${DURATION:-7}

if [ ! -d "$ZMK_WS/app" ]; then
    echo "error: no ZMK app at $ZMK_WS/app; set ZMK_WS to a west workspace" >&2
    exit 1
fi

echo "==> building native_sim (smoke fixture)"
(
    cd "$ZMK_WS"
    "$WEST" build -s app -d "$BUILD_DIR" -b native_sim//zmk_test_mock -p -- \
        -DCONFIG_ASSERT=y \
        -DZMK_CONFIG="$SIM_DIR/config-smoke" \
        "-DZMK_EXTRA_MODULES=$MODULE_ROOT;$SIM_DIR/module"
)

EXE=$BUILD_DIR/zephyr/zmk.exe
if [ ! -x "$EXE" ]; then
    echo "error: $EXE was not produced" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

echo "==> running the smoke scenario (${DURATION}s)"
(
    cd "$OUT_DIR"
    "$EXE" --capture=capture.bin "--stop_at=$DURATION" \
        --caps-on=4200 --caps-off=5500 -no-rt >run.log 2>&1
)
python3 "$SIM_DIR/check_capture.py" "$OUT_DIR/capture.bin"

# A capture that cannot be written must fail the run, not leave an empty file.
echo "==> negative: unwritable --capture path"
if "$EXE" --capture=/nonexistent-dir/x.bin --stop_at=1 -no-rt \
    >"$OUT_DIR/neg-open.log" 2>&1; then
    echo "error: an unwritable --capture path exited 0" >&2
    exit 1
fi

# /dev/full accepts the open but fails every write; Linux-only, so skip elsewhere.
if [ -w /dev/full ]; then
    echo "==> negative: write error (/dev/full)"
    if "$EXE" --capture=/dev/full --stop_at=1 -no-rt \
        >"$OUT_DIR/neg-full.log" 2>&1; then
        echo "error: a failing capture write exited 0" >&2
        exit 1
    fi
fi

echo "==> smoke tests passed"
