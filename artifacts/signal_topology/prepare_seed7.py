"""Prepare an isolated App runtime; never launches City or alters existing saves."""
from pathlib import Path
import argparse
import json

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--run-name', default='signal-generation-seed7')
parser.add_argument('--audit', action='store_true', help='Snapshot and exit before main-world rendering')
args = parser.parse_args()
if not args.run_name or Path(args.run_name).name != args.run_name or args.run_name in {'.', '..'}:
    raise SystemExit('run-name must be a single directory name')
repo = Path(__file__).resolve().parents[2]
run = repo.parent / 'build-tools/latest' / args.run_name
app = run / 'App'
if run.exists():
    raise SystemExit(f'Refusing to reuse existing run: {run}')
app.mkdir(parents=True)
for name in ['City', 'assets', 'engine', 'resources', 'shaders']:
    source = repo / 'App' / name
    assert source.exists(), source
    (app / name).symlink_to(source, target_is_directory=source.is_dir())
print(f'cd {app}')
if args.audit:
    print('./City --audit-road-integrity --seed 7 --low-spec --render-distance 100 --new')
    print(f'Expected snapshot: {app / "road_integrity_snapshot"}')
else:
    command = run / 'commands.json'
    command.write_text(json.dumps({'id': 1, 'action': 'benchmark_view', 'distance': 150, 'speed': 0}))
    print(f'./City --playtest-commands {command} --seed 7 --low-spec --render-distance 500 --new')
    print('After commandId 1: write command {"id":2,"action":"rail_save_review"}; save remains inside isolated App/saves/railway_review only.')
    print('After save: keep app open for visual review. To exit write {"id":9,"action":"exit"}.')
