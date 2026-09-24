#!/usr/bin/env bash
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
#
# Build the RGB matrix module for native_sim and render one GIF preview per
# devicetree effect node.
#
# The effect list comes from the firmware itself (`zmk.exe --list-effects`), and
# each GIF is named after that node's `display-name`, so filenames cannot drift
# from tests/sim/config/native_sim.overlay. GIFs land flat in tests/sim/out/,
# ready to upload; captures and logs go to tests/sim/out/.work/ (both ignored).
#
# Usage:
#   tests/sim/run-preview.sh [filter ...]
#
# With no filter every effect is rendered. A filter matches an effect's slug as
# a substring, or its index exactly:
#   ./run-preview.sh rainbow        # every rainbow variant
#   ./run-preview.sh ripple 5       # ripple, plus index 5 (breathe-river)
#
# Environment:
#   ZMK_WS    west workspace root       (default: <repo>/zmk)
#   WEST      west executable           (default: <repo>/.venv/bin/west, else $PATH)
#   DURATION  simulated seconds per run (default: 4)
#   TILE      pixels per LED            (default: 32)
#   MIN_DISTINCT  warn below this many distinct frames (default: 3)
#   OUT_DIR   where previews are written

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

BUILD_DIR=$SIM_DIR/build
OUT_DIR=${OUT_DIR:-$SIM_DIR/out}
DURATION=${DURATION:-4}
TILE=${TILE:-32}
# Warn when a preview never changes; a static capture is nearly always a bug.
# `solid` and `static` are legitimately static and will warn.
MIN_DISTINCT=${MIN_DISTINCT:-3}
filters=("$@")

if [ ! -d "$ZMK_WS/app" ]; then
    echo "error: no ZMK app at $ZMK_WS/app; set ZMK_WS to a west workspace" >&2
    exit 1
fi

echo "==> building native_sim (one build serves every effect)"
(
    cd "$ZMK_WS"
    "$WEST" build -s app -d "$BUILD_DIR" -b native_sim//zmk_test_mock -p -- \
        -DCONFIG_ASSERT=y \
        -DZMK_CONFIG="$SIM_DIR/config" \
        "-DZMK_EXTRA_MODULES=$MODULE_ROOT;$SIM_DIR/module"
)

EXE=$BUILD_DIR/zephyr/zmk.exe
if [ ! -x "$EXE" ]; then
    echo "error: $EXE was not produced" >&2
    exit 1
fi

# The firmware owns the list, so these names always match what the engine
# actually registered. It writes to a file rather than stdout because the POSIX
# console shim treats '\n' as a flush trigger instead of content.
mkdir -p "$OUT_DIR/.work"
warn_log=$OUT_DIR/.work/warnings.log
: >"$warn_log"
effect_list=$OUT_DIR/.work/effects.tsv
"$EXE" "--list-effects=$effect_list" >/dev/null 2>&1 || true

if [ ! -s "$effect_list" ]; then
    echo "error: '$EXE --list-effects' produced no effect list" >&2
    exit 1
fi

mapfile -t effect_lines < <(awk -F'\t' 'NF >= 2 && $1 ~ /^[0-9]+$/' "$effect_list")
if [ "${#effect_lines[@]}" -eq 0 ]; then
    echo "error: $effect_list has no usable records" >&2
    exit 1
fi

slugify() {
    printf '%s' "$1" |
        tr '[:upper:]' '[:lower:]' |
        sed -e 's/[^a-z0-9]\+/-/g' -e 's/^-//' -e 's/-$//'
}

declare -A slug_owner=()
indices=()
slugs=()
names=()

for line in "${effect_lines[@]}"; do
    idx=${line%%$'\t'*}
    name=${line#*$'\t'}
    slug=$(slugify "$name")

    if [ -z "$slug" ]; then
        echo "error: effect $idx has no usable display-name ('$name')" >&2
        exit 1
    fi
    if [ -n "${slug_owner[$slug]:-}" ]; then
        echo "error: effect ${slug_owner[$slug]} and '$name' both slugify to '$slug'" >&2
        exit 1
    fi
    slug_owner[$slug]=$idx

    indices+=("$idx")
    slugs+=("$slug")
    names+=("$name")
done

selected() {
    local slug=$1 idx=$2 want
    if [ "${#filters[@]}" -eq 0 ]; then
        return 0
    fi
    for want in "${filters[@]}"; do
        if [ "$want" = "$idx" ] || [[ $slug == *"$want"* ]]; then
            return 0
        fi
    done
    return 1
}

mkdir -p "$OUT_DIR"
rendered=0

for i in "${!indices[@]}"; do
    idx=${indices[$i]}
    slug=${slugs[$i]}
    name=${names[$i]}
    selected "$slug" "$idx" || continue

    # One working directory per effect, so --capture=capture.bin stays relative
    # and a failed run keeps its own log.
    work_dir=$OUT_DIR/.work/$slug
    mkdir -p "$work_dir"

    echo "==> $slug  [$idx] $name"
    (
        cd "$work_dir"
        "$EXE" "--effect=$idx" --capture=capture.bin \
            "--stop_at=$DURATION" -no-rt >run.log 2>&1
    )

    if [ ! -s "$work_dir/capture.bin" ]; then
        echo "error: $slug captured nothing; see $work_dir/run.log" >&2
        exit 1
    fi

    python3 "$SIM_DIR/render_gif.py" "$work_dir/capture.bin" \
        -o "$OUT_DIR/$slug.gif" --tile "$TILE" \
        --min-distinct "$MIN_DISTINCT" 2>>"$warn_log"
    rendered=$((rendered + 1))
done

if [ "$rendered" -eq 0 ]; then
    echo "error: no effect matched: ${filters[*]}" >&2
    echo "       available: ${slugs[*]}" >&2
    exit 1
fi

if grep -q '^warning:' "$warn_log" 2>/dev/null; then
    echo
    echo "==> warnings:"
    grep '^warning:' "$warn_log" | sed 's/^/    /'
fi

echo "==> $rendered previews in $OUT_DIR"
