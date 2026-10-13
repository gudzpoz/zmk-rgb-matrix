# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Bound native subprocess groups and retain captured output in caller-owned logs."""
import math
import os
from pathlib import Path
import signal
import subprocess

BUILD_TIMEOUT = 600
UTILITY_TIMEOUT = 30
HOST_TIMEOUT = 15


def run_logged(command, *, log_path, timeout=UTILITY_TIMEOUT, **kwargs):
    """Like subprocess.run, with persistent stdout/stderr logs and group timeout cleanup.

    Output defaults to separate PIPEs; stderr=STDOUT is also supported. Other
    output redirections are rejected so logs cannot silently omit output. Logs
    contain stdout followed by stderr when captured separately. Timeout errors
    retain the complete drained output; check=True errors are raised after logging.
    """
    if timeout is None or not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("timeout must be finite and positive")
    check = kwargs.pop("check", False)
    data = kwargs.pop("input", None)
    capture_output = kwargs.pop("capture_output", False)
    if capture_output and ("stdout" in kwargs or "stderr" in kwargs):
        raise ValueError("capture_output cannot be combined with stdout/stderr")
    kwargs.setdefault("stdout", subprocess.PIPE)
    kwargs.setdefault("stderr", subprocess.PIPE)
    if kwargs["stdout"] != subprocess.PIPE or kwargs["stderr"] not in (
            subprocess.PIPE, subprocess.STDOUT):
        raise ValueError("run_logged requires captured stdout and stderr")
    if data is not None:
        if "stdin" in kwargs:
            raise ValueError("input cannot be combined with stdin")
        kwargs["stdin"] = subprocess.PIPE
    if kwargs.get("start_new_session", True) is not True or kwargs.get("process_group") is not None:
        raise ValueError("run_logged requires its own process session")
    kwargs["start_new_session"] = True
    log = Path(log_path)
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_bytes(b"")

    text = kwargs.pop("text", False)
    text = kwargs.pop("universal_newlines", False) or text
    encoding = kwargs.pop("encoding", None)
    errors = kwargs.pop("errors", None)
    text = text or encoding is not None or errors is not None
    encoding = encoding or "utf-8"
    errors = errors or "strict"
    if text and data is not None:
        data = data.encode(encoding, errors)

    def decoded(output):
        if output is None or not text:
            return output
        return output.decode(encoding, errors).replace("\r\n", "\n").replace("\r", "\n")

    with subprocess.Popen(command, **kwargs) as process:
        try:
            stdout, stderr = process.communicate(data, timeout=timeout)
        except subprocess.TimeoutExpired:
            # The leader may already have exited while descendants still hold pipes.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            stdout, stderr = process.communicate()
            log.write_bytes((stdout or b"") + (stderr or b"")
                            + f"\nTIMEOUT after {timeout}s: {command!r}\n".encode())
            raise subprocess.TimeoutExpired(command, timeout, output=stdout,
                                            stderr=stderr) from None
        log.write_bytes((stdout or b"") + (stderr or b""))
        result = subprocess.CompletedProcess(command, process.returncode,
                                             decoded(stdout), decoded(stderr))
        if check:
            result.check_returncode()
        return result
