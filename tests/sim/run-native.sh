#!/usr/bin/env bash
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
set -euo pipefail

SIM_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MODULE_ROOT=$(cd "$SIM_DIR/../.." && pwd)
cd "$MODULE_ROOT"

export ZEPHYR_TOOLCHAIN_VARIANT=host
ARTIFACT_ROOT="$MODULE_ROOT/build/native-artifacts"

all_suites=(
    selector selector_defaults scoped_settings scoped_split fixed_scene documented_setup
    contracts condition_contracts control_contracts effect_contracts singleton_settings_split
    static_metadata geometry configuration composite overlay_pair ztest led_strip_composite smoke
)

suites=()
forward_args=()
if (($# == 0)); then
    suites=("${all_suites[@]}")
else
    while (($#)); do
        if [[ "$1" == "--" ]]; then
            shift
            forward_args=("$@")
            break
        fi
        suites+=("$1")
        shift
    done
fi

# Validate the full selection before creating logs or starting any work.
for suite in "${suites[@]}"; do
    known=false
    for candidate in "${all_suites[@]}"; do
        if [[ "$suite" == "$candidate" ]]; then
            known=true
            break
        fi
    done
    if [[ "$known" != true ]]; then
        printf 'error: unknown native suite: %s\nAvailable suites: %s\n' \
            "$suite" "${all_suites[*]}" >&2
        exit 2
    fi
    if ((${#forward_args[@]})); then
        case "$suite" in
            selector_defaults|fixed_scene|documented_setup|configuration|composite|overlay_pair)
                printf 'error: native suite %s does not accept forwarded arguments\n' "$suite" >&2
                exit 2
                ;;
        esac
    fi
done
if ((${#suites[@]} == 0)); then
    echo "error: select at least one native suite" >&2
    exit 2
fi
mkdir -p "$ARTIFACT_ROOT"
export TMPDIR="$ARTIFACT_ROOT"

run_with_log() {
    local suite=$1
    shift
    local stamp log
    stamp=$(date -u +%Y%m%dT%H%M%S.%N)
    log="$ARTIFACT_ROOT/${stamp}-$$-${suite}.log"
    while [[ -e "$log" ]]; do
        stamp="${stamp}x"
        log="$ARTIFACT_ROOT/${stamp}-$$-${suite}.log"
    done
    echo "=== Native integration: $suite (log: $log) ==="
    if timeout --kill-after=10s 30m "$@" 2>&1 | tee "$log"; then
        :
    else
        status=$?
        printf 'FAIL: %s (exit %s; log: %s)\n' "$suite" "$status" "$log" >&2
        return "$status"
    fi
}

for suite in "${suites[@]}"; do
    case "$suite" in
        led_strip_composite)
            run_with_log "$suite" python3 -u tests/run-led-strip-composite.py \
                --build-dir "$MODULE_ROOT/build/led_strip_composite" "${forward_args[@]}"
            ;;
        smoke)
            run_with_log "$suite" bash tests/sim/run-smoke.sh "${forward_args[@]}"
            ;;
        ztest)
            run_with_log "$suite" python3 -u tests/sim/test_ztest.py \
                --build-dir "$MODULE_ROOT/build/ztest" \
                --artifact-dir "$ARTIFACT_ROOT/ztest" "${forward_args[@]}"
            ;;
        condition_contracts)
            run_with_log "$suite" python3 -u "tests/sim/test_${suite}.py" \
                --role all "${forward_args[@]}"
            ;;
        *)
            run_with_log "$suite" python3 -u "tests/sim/test_${suite}.py" \
                "${forward_args[@]}"
            ;;
    esac
done
