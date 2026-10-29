"""Collect completed checks and the source scope without rerunning the game."""
from pathlib import Path
import json
import shutil
import subprocess
import hashlib

root = Path(__file__).parent
runs = Path('artifacts/local_runs/checks')
ids = {
    'city_build': '20261006-200707-build-3wzb55ns',
    'test_build': '20261006-201923-build-5a_0hst8',
    'performance_tests': '20261006-200212-test-mm79goaz',
    'navigation': '20261006-201144-playtest-rpxw_hfo',
    'streaming': '20261006-201503-playtest-76tem_nb',
}
full = []
for path in runs.glob('*-test-*/summary.json'):
    data = json.loads(path.read_text(encoding='utf-8-sig'))
    if (path.parent.name > ids['test_build'] and data.get('status') == 'passed'
            and data.get('filter') == '(all)' and data.get('passed') == 330):
        full.append(path)
assert full, 'Final full test run has not completed successfully'
path = max(full)
ids['full_tests'] = path.parent.name
results = {}
for name, run in ids.items():
    summary = json.loads((runs / run / 'summary.json').read_text(encoding='utf-8-sig'))
    assert summary['status'] == 'passed', (name, summary)
    results[name] = {'run_id': run, 'summary': summary}
out = root / 'full_tests'
out.mkdir(exist_ok=True)
for source in (runs / ids['full_tests']).rglob('*'):
    if source.is_file() and source.name in ['summary.json', 'results.json', 'results.xml']:
        shutil.copy2(source, out / source.name)
subprocess.run(['python', str(root / 'audit_scope.py')], check=True)
scope = json.loads((root / 'source_scope.json').read_text(encoding='utf-8'))
save = Path('App/saves/refactoring_20261006')
fixture = {str(p.relative_to(save)): hashlib.sha256(p.read_bytes()).hexdigest()
           for p in save.rglob('*') if p.is_file()}
record = {
    'date': '2026-10-06',
    'baseline_commit': 'cbaed46',
    'checks': results,
    'conditions': {
        'resolution': [1920, 1080], 'seed': 42, 'configuration': 'Release',
        'traffic': {'hour': 8, 'speed': 1, 'distances': [650, 3000],
                    'steady_frames_per_view': 1200, 'warmup_frames_per_view': 1200},
        'paused': {'hour': 13, 'speed': 0, 'distances': [650, 3000, 6000, 12000],
                   'steady_frames_per_view': 240, 'warmup_frames_per_view': 240},
        'final_fixed_view_build': ids['city_build'],
        'diagnostics': {'traffic_baseline': 'full', 'traffic_final': 'full',
                        'traffic_perf_final': 'performanceOnly', 'wide_perf_final': 'performanceOnly'},
        'fixture_path': str(save), 'fixture_sha256': fixture,
    },
    'source_sha256': scope,
}
(root / 'validation.json').write_text(json.dumps(record, ensure_ascii=False, indent=2), encoding='utf-8')
print('Recorded checks:', ', '.join(ids))
