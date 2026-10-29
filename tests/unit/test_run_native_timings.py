#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Host-only tests for run-native suite timing reports."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SIM = Path(__file__).resolve().parents[1] / 'sim'
RUNNER = SIM / 'run-native.sh'


class RunNativeTimingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.bin = Path(self.temp.name) / 'bin'
        self.bin.mkdir()
        self.calls = Path(self.temp.name) / 'calls'
        self.sim = Path(self.temp.name) / 'module/tests/sim'
        self.sim.mkdir(parents=True)
        self.runner = self.sim / 'run-native.sh'
        self.runner.write_bytes(RUNNER.read_bytes())
        self.report_path = self.sim.parents[1] / 'build/native-artifacts/suite-timings.tsv'
        self._command('python3')
        self._command('bash')
        self._command('timeout')

    def _command(self, name):
        path = self.bin / name
        if name == 'timeout':
            script = '#!/bin/sh\nshift 2\n"$@"\n'
        else:
            script = (
                '#!/bin/sh\n'
                f"printf '%s ' {name!r} >> {str(self.calls)!r}\n"
                f"printf '<%s>' \"$@\" >> {str(self.calls)!r}\n"
                f"printf '\\n' >> {str(self.calls)!r}\n"
                'exit "${FAKE_EXIT:-0}"\n'
            )
        path.write_text(script)
        path.chmod(0o755)
    def run_suites(self, *args, **env):
        environment = os.environ.copy()
        environment.update({'PATH': f"{self.bin}:{environment['PATH']}", **env})
        return subprocess.run(['/bin/bash', str(self.runner), *args], cwd=self.sim,
                              env=environment, text=True, capture_output=True, timeout=10)

    def report(self):
        return self.report_path.read_text().splitlines()

    def test_success_forwards_arguments_and_preserves_suite_order(self):
        result = self.run_suites('contracts', 'smoke', '--', '--variant', 'normal')
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = self.report()
        self.assertEqual(lines[0], 'suite\tstatus\texit_code\telapsed_seconds')
        self.assertEqual(lines[1].split('\t')[:3], ['contracts', 'passed', '0'])
        self.assertEqual(lines[2].split('\t')[:3], ['smoke', 'passed', '0'])
        self.assertTrue(all(int(line.split('\t')[3]) >= 0 for line in lines[1:]))
        calls = self.calls.read_text()
        self.assertIn('python3 <-u><tests/sim/test_contracts.py><--variant><normal>', calls)
        self.assertIn('bash <tests/sim/run-smoke.sh><--variant><normal>', calls)

    def test_duplicate_suites_get_distinct_report_rows(self):
        result = self.run_suites('contracts', 'contracts')
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = self.report()
        self.assertEqual([line.split('\t')[:2] for line in lines[1:]],
                         [['contracts', 'passed'], ['contracts', 'passed']])

    def test_failure_records_actual_status_and_leaves_later_suite_not_run(self):
        result = self.run_suites('contracts', 'control_contracts', FAKE_EXIT='7')
        self.assertEqual(result.returncode, 7)
        lines = self.report()
        self.assertEqual(lines[1].split('\t')[:3], ['contracts', 'failed', '7'])
        self.assertEqual(lines[2], 'control_contracts\tnot_run\t\t')

    def test_timeout_status_is_reported(self):
        result = self.run_suites('contracts', FAKE_EXIT='124')
        self.assertEqual(result.returncode, 124)
        self.assertEqual(self.report()[1].split('\t')[:3], ['contracts', 'failed', '124'])

    def test_unknown_suite_fails_before_report_or_commands(self):
        result = self.run_suites('unknown-suite')
        self.assertEqual(result.returncode, 2)
        self.assertIn('unknown native suite', result.stderr)
        self.assertFalse(self.calls.exists())
        self.assertFalse(self.report_path.exists())


if __name__ == '__main__':
    unittest.main()
