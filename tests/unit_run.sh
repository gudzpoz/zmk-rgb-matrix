#!/usr/bin/env bash
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
#
# Runs the host-side Python tests in tests/unit/test_*.py: codec, subprocess,
# and preview timing. They do not require a ZMK workspace or Zephyr SDK.
# Native_sim integration and ztest coverage run separately through
# tests/sim/run-native.sh.

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
