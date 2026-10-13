# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Bound native subprocesses and retain output, including on timeout."""
from pathlib import Path
import subprocess


def run_logged(command, *, log_path, timeout=30, **kwargs):
    try:
        result = subprocess.run(command, timeout=timeout, **kwargs)
    except subprocess.TimeoutExpired as error:
        output = error.stdout or b""
        if isinstance(output, bytes):
            output = output.decode(errors="replace")
        Path(log_path).write_text(output + f"\nTIMEOUT after {timeout}s: {command!r}\n")
        raise
    Path(log_path).write_text(result.stdout or "")
    return result
