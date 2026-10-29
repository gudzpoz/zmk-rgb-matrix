#!/usr/bin/env bash
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
set -euo pipefail
exec python3 "$(dirname "${BASH_SOURCE[0]}")/test_smoke.py" "$@"
