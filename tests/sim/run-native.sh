#!/usr/bin/env bash
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
set -euo pipefail
cd "$(dirname "$0")/../.."
export TMPDIR="$(pwd)/build/native-artifacts"
mkdir -p "$TMPDIR"
suites=(selector selector_defaults persist_ids scoped_settings scoped_split fixed_scene documented_setup)
if (( $# )); then suites=("$@"); fi
for suite in "${suites[@]}"; do
    echo "=== Native integration: $suite ==="
    timeout --kill-after=10s 30m python3 -u "tests/sim/test_${suite}.py" 2>&1 |
        tee "$TMPDIR/${suite}.log"
done
