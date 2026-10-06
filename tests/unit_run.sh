#!/usr/bin/env bash
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
#
# Runs the zmk-rgb-matrix host unit tests (tests/unit/test_*.py).
#
# Each script exercises pure helpers or production-extracted engine functions
# by compiling them on the host with the system C toolchain, so no Zephyr SDK
# is required. The native_sim ztest suite (tests/unit/CMakeLists.txt) is a
# separate, heavier step (needs a ZMK checkout + west) and is intentionally not
# part of this script.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UNIT_DIR="${SCRIPT_DIR}/unit"

cd "${UNIT_DIR}" || exit 1

fail=0
for t in test_*.py; do
  [ -f "$t" ] || continue
  printf '=== %s ===\n' "$t"
  if python3 "./$t"; then
    echo "PASS: $t"
  else
    echo "FAIL: $t (exit $?)"
    fail=1
  fi
done

if [ "$fail" -ne 0 ]; then
  echo "Some unit tests failed."
  exit 1
fi

echo "All unit tests passed."
