"""Stage and verify only the implementation and reproducible evidence for this task."""
from pathlib import Path
import json
import subprocess
import sys

root = Path.cwd()
report = Path('artifacts/performance_phase2_20261006')
git = ['git', '-c', 'safe.directory=' + root.as_posix()]
scope = json.loads((report / 'source_scope.json').read_text(encoding='utf-8'))
paths = list(scope)
files = ['REVIEW.md', 'validation.json', 'source_scope.json', 'traffic_comparison.json',
         'frame_diagnostics.json', 'upload_comparison.json', 'traffic_probe.ps1', 'wide_probe.ps1',
         'measure.py', 'summarize.py', 'frame_analysis.py', 'audit_scope.py',
         'collect_results.py', 'record_validation.py', 'commit_scope.py']
paths += [str(report / name) for name in files]
for label in ['traffic_baseline', 'traffic_final', 'traffic_perf_final', 'wide_perf_final',
              'navigation_baseline', 'navigation_final', 'streaming_previous', 'streaming_final', 'full_tests']:
    for name in ['playtest_frames.csv', 'navigation_frames.csv', 'streaming_frames.csv',
                 'debug.log', 'summary.json', 'state_1.json', 'state_2.json', 'results.json']:
        path = report / label / name
        if path.exists():
            paths.append(str(path))
paths = sorted(set(Path(name).as_posix() for name in paths))
assert all(Path(name).is_file() for name in paths)
if '--stage' in sys.argv:
    assert not subprocess.check_output(git + ['diff', '--cached', '--name-only']), 'Index already contains changes'
    subprocess.run(git + ['add', '--'] + paths, check=True)
    staged = subprocess.check_output(git + ['diff', '--cached', '--name-only'], encoding='utf-8').splitlines()
    assert sorted(staged) == paths, 'Unexpected or missing staged paths'
    subprocess.run(git + ['-c', 'core.whitespace=blank-at-eol,blank-at-eof,space-before-tab,cr-at-eol',
                         'diff', '--cached', '--check'], check=True)
print('Task scope:', len(paths), 'files;', sum(Path(name).stat().st_size for name in paths), 'bytes')
