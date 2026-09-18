"""Regression checks for trustworthy summaries; no game or MSBuild dependency."""

import csv
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import run_check


class SummaryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)

    def json_file(self, data):
        path = self.directory / 'results.json'
        path.write_text(json.dumps(data), encoding='utf-8-sig')
        return path

    def csv_file(self, fields, rows):
        path = self.directory / 'frames.csv'
        with path.open('w', encoding='utf-8-sig', newline='') as output:
            writer = csv.writer(output)
            writer.writerow(fields)
            writer.writerows(rows)
        return path

    def test_failure_count_is_complete_but_details_are_bounded(self):
        tests = [{'name': str(i), 'status': 'failed', 'failures': ['reason' * 200] * 8} for i in range(12)]
        summary = run_check.summarize_tests(self.json_file({'total': 12, 'passed': 0, 'failed': 12, 'tests': tests}))
        self.assertEqual(summary['failed'], 12)
        self.assertEqual(len(summary['failure_details']), 5)
        self.assertEqual(len(summary['failure_details'][0]['reasons']), 3)
        self.assertEqual(len(summary['failure_details'][0]['reasons'][0]), 300)

    def test_zero_tests_and_inconsistent_totals_are_rejected(self):
        with self.assertRaises(ValueError):
            run_check.summarize_tests(self.json_file({'total': 0, 'passed': 0, 'failed': 0, 'tests': []}))
        with self.assertRaises(ValueError):
            run_check.summarize_tests(self.json_file({'total': 1, 'passed': 1, 'failed': 0,
                                                    'tests': [{'name': 'bad', 'status': 'failed', 'failures': []}]}))

    def test_stale_results_are_not_accepted(self):
        source = self.json_file({'old': True})
        with self.assertRaises(ValueError):
            run_check.copy_fresh(source, self.directory / 'copy.json', run_check.fingerprint(source))
        run_check.copy_fresh(source, self.directory / 'copy.json', None)
        self.assertEqual(source.read_bytes(), (self.directory / 'copy.json').read_bytes())

    def test_build_diagnostics_are_deduplicated_and_bounded(self):
        log = self.directory / 'build.log'
        warning = 'file.cpp(10): warning C4100: unused parameter'
        log.write_text('\n'.join([warning] * 3 + ['LINK : fatal error LNK1168: locked']), encoding='utf-8')
        summary = run_check.summarize_build([log, log])
        self.assertEqual((summary['errors'], summary['warnings']), (1, 1))

    def test_frame_and_cpu_times_are_not_confused(self):
        path = self.csv_file(['frame', 'cpuMs', 'frameMs'], [(i, 2, 16) for i in range(720)])
        result = run_check.summarize_frames(path, 'streaming')
        self.assertEqual(result['samples'], 720)
        self.assertEqual(result['milliseconds']['cpuMs']['mean'], 2)
        self.assertEqual(result['milliseconds']['frameMs']['p95'], 16)

    def test_incomplete_and_nonfinite_frames_are_rejected(self):
        path = self.csv_file(['frame', 'cpuMs', 'frameMs'], [(0, 2, 16)])
        with self.assertRaises(ValueError):
            run_check.summarize_frames(path, 'streaming')
        path = self.csv_file(['frameMs'], [('nan',)])
        with self.assertRaises(ValueError):
            run_check.summarize_frames(path)

    def test_navigation_includes_wait_frames_and_requires_all_phases(self):
        rows = [(phase, frame, 3) for phase in range(6) for frame in range(180)]
        rows.insert(61, (0, 60, 6))
        path = self.csv_file(['phase', 'frame', 'cpuMs'], rows)
        summary = run_check.summarize_frames(path, 'navigation')
        self.assertEqual(summary['samples'], 1081)
        self.assertEqual(len(summary['phase_cpu_ms']), 6)
        path = self.csv_file(['phase', 'frame', 'cpuMs'], rows[1:])
        with self.assertRaises(ValueError):
            run_check.summarize_frames(path, 'navigation')

    def test_nearest_rank_percentile(self):
        self.assertEqual(run_check.stats(list(range(1, 101))), {'mean': 50.5, 'p95': 95, 'max': 100})

    def test_child_failure_and_output_are_preserved(self):
        log = self.directory / 'process.log'
        code = run_check.run_process([sys.executable, '-c', 'print("diagnostic"); raise SystemExit(7)'],
                                     self.directory, log, 10)
        self.assertEqual(code, 7)
        self.assertIn('diagnostic', log.read_text())

    def test_timeout_stops_owned_child(self):
        original_popen = subprocess.Popen
        children = []
        def launch(*args, **kwargs):
            child = original_popen(*args, **kwargs)
            children.append(child)
            return child
        with patch.object(run_check.subprocess, 'Popen', launch):
            with self.assertRaises(subprocess.TimeoutExpired):
                run_check.run_process([sys.executable, '-c', 'import time; time.sleep(60)'],
                                      self.directory, self.directory / 'process.log', 0.1)
        self.assertIsNotNone(children[0].poll())

    @unittest.skipUnless(sys.platform == 'win32', 'Windows launcher')
    def test_competing_runner_keeps_existing_lock(self):
        lock = self.directory / '.runner.lock'
        lock.write_text('another process', encoding='utf-8')
        output = io.StringIO()
        with patch.object(run_check, 'RUNS', self.directory), contextlib.redirect_stdout(output):
            code = run_check.main(['test'])
        self.assertEqual(code, 1)
        self.assertEqual(lock.read_text(encoding='utf-8'), 'another process')
        self.assertIn('Another check owns', json.loads(output.getvalue())['reason'])

    def test_nonzero_exit_overrides_passing_test_results(self):
        app = self.directory / 'Test/App'
        (app / 'TestResults').mkdir(parents=True)
        (app / 'Test.exe').touch()
        output = self.directory / 'run'
        output.mkdir()
        def failed_run(*_):
            run_check.write_json(app / 'TestResults/results.json',
                                 {'total': 1, 'passed': 1, 'failed': 0,
                                  'tests': [{'name': 'case', 'status': 'passed', 'failures': []}]})
            return 7
        args = type('Args', (), {'action': 'test', 'configuration': 'Release', 'filter': '', 'timeout': 10})()
        summary = {}
        with patch.object(run_check, 'ROOT', self.directory), patch.object(run_check, 'run_process', failed_run):
            code = run_check.execute(args, output, summary)
        self.assertEqual((code, summary['status'], summary['exit_code']), (1, 'failed', 7))


if __name__ == '__main__':
    unittest.main()
