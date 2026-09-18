#!/usr/bin/env python3
"""Run City checks with complete local logs and a bounded JSON summary (Windows)."""

import argparse
import csv
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parent.parent
MSBUILD = Path('C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe')
RUNS = ROOT / 'artifacts/local_runs/checks'
DETAIL_LIMIT = 5


def write_json(path, value):
    """Keep full results on disk; only the summary is printed."""
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def fingerprint(path):
    if not path.exists():
        return None
    info = path.stat()
    return info.st_mtime_ns, info.st_size


def copy_fresh(source, destination, previous):
    """An unchanged result must never stand in for this invocation's result."""
    current = fingerprint(source)
    if current is None or current == previous:
        raise ValueError(f'No fresh result: {source}')
    shutil.copy2(source, destination)
    return destination


def summarize_build(paths):
    diagnostics = {'errors': set(), 'warnings': set()}
    pattern = re.compile(r'\b(fatal error|error|warning)\s+[A-Z]+\d+\b|(?:警告|エラー)\s+[A-Z]+\d+', re.I)
    for path in paths:
        if not path.exists():
            continue
        for line in path.read_text(encoding='utf-8-sig', errors='replace').splitlines():
            match = pattern.search(line)
            if match:
                key = 'warnings' if re.search(r'warning|警告', match[0], re.I) else 'errors'
                diagnostics[key].add(line.strip())
    result = {}
    for key, values in diagnostics.items():
        result[key] = len(values)
        if values:
            result[key + '_details'] = [value[:400] for value in sorted(values)[:DETAIL_LIMIT]]
    return result


def summarize_tests(path):
    data = json.loads(path.read_text(encoding='utf-8-sig'))
    tests = data['tests']
    if not isinstance(tests, list) or not tests:
        raise ValueError('No tests selected; check --filter')
    if any(test['status'] not in ('passed', 'failed') for test in tests):
        raise ValueError('Unknown test status')
    failed = [test for test in tests if test['status'] == 'failed']
    counts = {'total': len(tests), 'passed': len(tests) - len(failed), 'failed': len(failed)}
    if any(data[key] != value for key, value in counts.items()):
        raise ValueError('Test counts do not match individual results')
    counts['failure_details'] = [
        {'name': test['name'][:160], 'reasons': [str(reason)[:300] for reason in test['failures'][:3]]}
        for test in failed[:DETAIL_LIMIT]
    ]
    return counts


def stats(values):
    ordered = sorted(values)
    return {'mean': round(statistics.fmean(ordered), 3),
            'p95': round(ordered[math.ceil(len(ordered) * 0.95) - 1], 3),
            'max': round(ordered[-1], 3)}


def summarize_frames(path, mode=None):
    """CPU time and frame interval remain separate; p95 uses nearest rank."""
    with path.open(encoding='utf-8-sig', newline='') as stream:
        reader = csv.DictReader(stream)
        fields = reader.fieldnames or []
        rows = list(reader)
    if not rows:
        raise ValueError('No frame samples')
    if mode == 'streaming':
        if [int(row['frame']) for row in rows] != list(range(720)):
            raise ValueError('Streaming benchmark did not complete its 720 samples')
    if mode == 'navigation':
        observed = {(int(row['phase']), int(row['frame'])) for row in rows}
        expected = {(phase, frame) for phase in range(6) for frame in range(180)}
        if observed != expected or (int(rows[-1]['phase']), int(rows[-1]['frame'])) != (5, 179):
            raise ValueError('Navigation benchmark did not complete all six phases')
    required = ['cpuMs', 'frameMs'] if mode == 'streaming' else ['cpuMs'] if mode else ['frameMs']
    if not all(key in fields for key in required):
        raise ValueError(f'Missing required CSV columns: {required}')
    metrics = {}
    for key in ('cpuMs', 'frameMs', 'renderMs', 'logicMs', 'gpuMs'):
        if key not in fields:
            continue
        values = [float(row[key]) for row in rows]
        valid = [value for value in values if math.isfinite(value) and value >= 0]
        if key in required and len(valid) != len(rows):
            raise ValueError(f'Invalid timing samples in {key}')
        if valid:
            metrics[key] = stats(valid)
            if len(valid) != len(rows):
                metrics[key]['excluded'] = len(rows) - len(valid)
    result = {'samples': len(rows), 'milliseconds': metrics}
    if mode == 'navigation':
        result['phase_cpu_ms'] = {
            str(phase): stats([float(row['cpuMs']) for row in rows if int(row['phase']) == phase])
            for phase in range(6)
        }
    return result


def stop_owned_process(process):
    """Only stop the process tree started by this runner, including compiler children."""
    if process.poll() is None:
        if os.name == 'nt':
            subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           creationflags=subprocess.CREATE_NO_WINDOW, timeout=15, check=False)
        if process.poll() is None:
            process.kill()
        process.wait(timeout=15)


def run_process(command, cwd, log, timeout):
    flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    with log.open('wb') as output:
        process = subprocess.Popen(command, cwd=cwd, stdout=output, stderr=subprocess.STDOUT,
                                   creationflags=flags)
        try:
            return process.wait(timeout=timeout)
        finally:
            stop_owned_process(process)


def check_idle():
    """Do not overwrite runtime files or measure alongside an existing game/test."""
    command = "$p = @(Get-Process -Name 'City','City(debug)','Test','Test(debug)' -ErrorAction SilentlyContinue); if ($p.Count) { exit 1 }"
    result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', command],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            creationflags=subprocess.CREATE_NO_WINDOW, timeout=30, check=False)
    if result.returncode:
        raise RuntimeError('City/Test is already running, or the process check failed; wait or close it first')


def execute(args, directory, summary):
    debug = '(debug)' if args.configuration == 'Debug' else ''
    app = ROOT / ('Test/App' if args.action == 'test' else 'App')
    result_file = None
    if args.action == 'build':
        command = [str(MSBUILD), 'City.sln', '-target:' + args.target,
                   '-p:Configuration=' + args.configuration, '-p:Platform=x64',
                   '-p:PreferredToolArchitecture=x64', '-m:1', '-verbosity:minimal', '-noLogo',
                   '-fileLogger', '-fileLoggerParameters:LogFile=' + str(directory / 'build.log') + ';Verbosity=normal;Encoding=UTF-8']
        cwd = ROOT
        summary['target'] = args.target
    elif args.action == 'test':
        command = [str(app / ('Test' + debug + '.exe'))]
        if args.filter:
            command += ['--filter', args.filter]
        result_file = app / 'TestResults/results.json'
        cwd = app
        summary['filter'] = args.filter or '(all)'
    else:
        command = [str(app / ('City' + debug + '.exe')), '--benchmark-' + args.mode,
                   '--seed', str(args.seed), '--new']
        result_file = app / (args.mode + '_frames.csv')
        cwd = app
        summary.update(mode=args.mode, seed=args.seed)
    if not Path(command[0]).is_file():
        raise FileNotFoundError(command[0])
    summary['configuration'] = args.configuration
    write_json(directory / 'command.json', {'argv': command, 'cwd': str(cwd)})
    previous = fingerprint(result_file) if result_file else None
    debug_previous = fingerprint(app / 'debug.log')
    perf_offset = (app / 'perf.log').stat().st_size if (app / 'perf.log').exists() else 0
    try:
        summary['exit_code'] = run_process(command, cwd, directory / 'console.log', args.timeout)
    finally:
        if args.action != 'build':
            # Preserve fresh diagnostics even on crashes/timeouts; perf.log is append-only.
            if fingerprint(app / 'debug.log') not in (None, debug_previous):
                shutil.copy2(app / 'debug.log', directory / 'debug.log')
            if (app / 'perf.log').exists():
                with (app / 'perf.log').open('rb') as source, (directory / 'perf.log').open('wb') as target:
                    source.seek(perf_offset if source.seek(0, 2) >= perf_offset else 0)
                    shutil.copyfileobj(source, target)
            if result_file and fingerprint(result_file) not in (None, previous):
                shutil.copy2(result_file, directory / result_file.name)
    if args.action == 'build':
        summary.update(summarize_build([directory / 'build.log', directory / 'console.log']))
        success = summary['exit_code'] == 0 and summary['errors'] == 0
    else:
        saved = copy_fresh(result_file, directory / result_file.name, previous)
        if args.action == 'test':
            summary.update(summarize_tests(saved))
            success = summary['exit_code'] == 0 and summary['failed'] == 0
        else:
            summary.update(summarize_frames(saved, args.mode))
            success = summary['exit_code'] == 0
    summary['status'] = 'passed' if success else 'failed'
    return 0 if success else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='action', required=True)
    build = commands.add_parser('build', help='Build only; no game/test launch')
    build.add_argument('--target', choices=['City', 'Test'], default='City')
    test = commands.add_parser('test', help='Run the existing Test executable; build separately')
    test.add_argument('--filter', default='')
    playtest = commands.add_parser('playtest', help='Run an auto-exiting in-game benchmark')
    playtest.add_argument('--mode', choices=['streaming', 'navigation'], default='streaming')
    playtest.add_argument('--seed', type=int, default=42)
    for command in (build, test, playtest):
        command.add_argument('--configuration', choices=['Release', 'Debug'], default='Release')
        command.add_argument('--timeout', type=int, default=900, help='Seconds; owned process tree is stopped on timeout')
    summarize = commands.add_parser('summarize', help='Summarize existing playtest CSV; does not certify a new run')
    summarize.add_argument('input', type=Path)
    summarize.add_argument('--mode', choices=['streaming', 'navigation', 'manual'], default='manual')
    args = parser.parse_args(argv)
    if args.action == 'summarize':
        try:
            result = summarize_frames(args.input, None if args.mode == 'manual' else args.mode)
            result.update(status='summarized', source=str(args.input.resolve()))
            print(json.dumps(result, ensure_ascii=False, indent=2))
            return 0
        except (OSError, ValueError, KeyError, TypeError) as error:
            print(json.dumps({'status': 'failed', 'reason': str(error)[:600]}, ensure_ascii=False))
            return 1
    if args.timeout <= 0:
        parser.error('--timeout must be positive')
    RUNS.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix=time.strftime('%Y%m%d-%H%M%S-') + args.action + '-', dir=RUNS))
    summary = {'action': args.action, 'status': 'failed', 'artifacts': str(directory)}
    started = time.monotonic()
    lock = RUNS / '.runner.lock'
    acquired = False
    exit_code = 1
    try:
        if os.name != 'nt':
            raise RuntimeError('Launch with Windows Python (MSBuild and Siv3D require Windows)')
        try:
            stream = lock.open('x', encoding='utf-8')
        except FileExistsError:
            raise RuntimeError(f'Another check owns {lock}; wait for it to finish') from None
        with stream:
            acquired = True
            stream.write(str(os.getpid()))
        check_idle()
        exit_code = execute(args, directory, summary)
    except subprocess.TimeoutExpired:
        summary.update(status='timeout', reason='Time limit exceeded; no successful completion recorded')
        exit_code = 124
    except KeyboardInterrupt:
        summary.update(status='interrupted', reason='Interrupted; no successful completion recorded')
        exit_code = 130
    except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
        summary['reason'] = str(error)[:600]
    finally:
        if acquired:
            lock.unlink()
        summary['elapsed_seconds'] = round(time.monotonic() - started, 2)
        write_json(directory / 'summary.json', summary)
        print(json.dumps(summary, ensure_ascii=False, indent=2))
    return exit_code


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    sys.exit(main())
