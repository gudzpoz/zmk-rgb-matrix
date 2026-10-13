#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Regression tests for bounded processes, complete logs and descendant cleanup."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sim"))
from native_process import BUILD_TIMEOUT, HOST_TIMEOUT, UTILITY_TIMEOUT, run_logged

LOGS = Path(__file__).resolve().parents[1] / "build/process/native-process"


class NativeProcessTests(unittest.TestCase):
    def test_defaults(self):
        self.assertEqual((BUILD_TIMEOUT, UTILITY_TIMEOUT, HOST_TIMEOUT), (600, 30, 15))
        for timeout in (None, 0, -1, float("inf"), float("nan")):
            with self.subTest(timeout=timeout), self.assertRaises(ValueError):
                run_logged([sys.executable, "-c", "pass"],
                           log_path=LOGS / "invalid.log", timeout=timeout)

    def test_success_nonzero_and_checked_failure_text_and_bytes(self):
        for text in (False, True):
            for code, check in ((0, True), (7, False), (7, True)):
                with self.subTest(text=text, code=code, check=check):
                    log = LOGS / f"nested/{text}-{code}-{check}.log"
                    command = [sys.executable, "-c",
                               "import os; os.write(1,b'out\\n'*20000); "
                               f"os.write(2,b'err\\n'*20000); exit({code})"]
                    try:
                        result = run_logged(command, log_path=log, text=text, check=check)
                    except subprocess.CalledProcessError as error:
                        self.assertTrue(check and code)
                        result = error
                    else:
                        self.assertFalse(check and code)
                    expected_out = "out\n" * 20000
                    expected_err = "err\n" * 20000
                    self.assertEqual(result.stdout, expected_out if text else expected_out.encode())
                    self.assertEqual(result.stderr, expected_err if text else expected_err.encode())
                    self.assertEqual(result.returncode, code)
                    self.assertEqual(log.read_bytes(), (expected_out + expected_err).encode())

    def test_input_and_merged_output(self):
        log = LOGS / "merged.log"
        result = run_logged([sys.executable, "-c",
                             "import sys; print(sys.stdin.read()); print('err',file=sys.stderr)"],
                            log_path=log, text=True, input="hello", check=True,
                            stderr=subprocess.STDOUT)
        self.assertIn("hello", result.stdout)
        self.assertIn("err", result.stdout)
        self.assertEqual(log.read_text(), result.stdout)

    def test_arbitrary_bytes(self):
        log = LOGS / "bytes.log"
        result = run_logged([sys.executable, "-c",
                             "import os; os.write(1,bytes(range(256)))"], log_path=log)
        self.assertEqual(result.stdout, bytes(range(256)))
        self.assertEqual(log.read_bytes(), result.stdout)

    def test_timeout_incomplete_text_keeps_raw_bytes(self):
        log = LOGS / "incomplete-text.log"
        with self.assertRaises(subprocess.TimeoutExpired) as caught:
            run_logged([sys.executable, "-c",
                        "import os,time; os.write(1,b'\\xe2\\x82'); time.sleep(30)"],
                       log_path=log, text=True, timeout=0.5)
        self.assertEqual(caught.exception.stdout, b"\xe2\x82")
        self.assertTrue(log.read_bytes().startswith(b"\xe2\x82"))

    def test_timeout_complete_output(self):
        for text in (False, True):
            with self.subTest(text=text):
                log = LOGS / f"timeout-{text}.log"
                with self.assertRaises(subprocess.TimeoutExpired) as caught:
                    run_logged([sys.executable, "-c",
                                "import os,time; os.write(1,b'out\\n'*20000); "
                                "os.write(2,b'err\\n'*20000); time.sleep(30)"],
                               log_path=log, timeout=0.5, text=text, check=True)
                self.assertEqual(caught.exception.stdout, b"out\n" * 20000)
                self.assertEqual(caught.exception.stderr, b"err\n" * 20000)
                self.assertTrue(log.read_bytes().startswith(b"out\n" * 20000 + b"err\n" * 20000))
                self.assertIn(b"TIMEOUT after 0.5s", log.read_bytes())

    @unittest.skipUnless(os.name == "posix" and Path("/proc").exists(), "Linux process groups")
    def test_timeout_kills_descendants_after_leader_exits(self):
        with tempfile.TemporaryDirectory() as directory:
            pidfile = Path(directory) / "pid"
            child = ("import os,signal,time; from pathlib import Path; "
                     "signal.signal(signal.SIGTERM,signal.SIG_IGN); "
                     f"Path({str(pidfile)!r}).write_text(str(os.getpid())); "
                     "print('descendant',flush=True); time.sleep(30)")
            parent = f"import subprocess,sys; subprocess.Popen([sys.executable,'-c',{child!r}])"
            started = time.monotonic()
            pid = None
            try:
                with self.assertRaises(subprocess.TimeoutExpired):
                    run_logged([sys.executable, "-c", parent], timeout=0.7,
                               log_path=LOGS / "descendant.log", text=True)
                self.assertLess(time.monotonic() - started, 3)
                pid = int(pidfile.read_text())
                for _ in range(100):
                    status = Path(f"/proc/{pid}/stat")
                    if not status.exists() or status.read_text().split(") ", 1)[1].startswith("Z"):
                        break
                    time.sleep(0.01)
                else:
                    self.fail("descendant remains alive after timeout")
                self.assertIn("descendant", (LOGS / "descendant.log").read_text())
            finally:
                if pid is None and pidfile.exists():
                    pid = int(pidfile.read_text())
                if pid is not None:
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass


if __name__ == "__main__":
    unittest.main()
