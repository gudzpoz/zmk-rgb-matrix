#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Keep partial native logs when a process exceeds its wall-clock deadline."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sim"))
from native_process import run_logged


def main():
    with tempfile.TemporaryDirectory() as directory:
        log = Path(directory) / "child.log"
        result = run_logged([sys.executable, "-c", "print('complete'); exit(7)"],
                            log_path=log, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
        assert result.returncode == 7 and log.read_text() == "complete\n"
        try:
            run_logged([sys.executable, "-c",
                        "import time; print('partial', flush=True); time.sleep(30)"],
                       log_path=log, timeout=0.5, text=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
        except subprocess.TimeoutExpired:
            assert log.read_text().startswith("partial\n")
            assert "TIMEOUT after 0.5s" in log.read_text()
        else:
            raise AssertionError("deadline was not enforced")
    print("PASS native subprocess deadline and partial log retention")


if __name__ == "__main__":
    main()
