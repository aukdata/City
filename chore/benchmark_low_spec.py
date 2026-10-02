#!/usr/bin/env python3
"""Sequential, opt-in City rendering comparison. No saves or simulation edits are made."""
from pathlib import Path
import argparse,csv,json,statistics,subprocess,time,shutil,hashlib
p=argparse.ArgumentParser();p.add_argument('--profile',choices=['normal','low-spec'],required=True);p.add_argument('--save',required=True);p.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[1]);p.add_argument('--near-only',action='store_true',help='Measure only the matched150m scene; omit cold full-city warmup');a=p.parse_args()
repo=a.repo.resolve();app=repo/'App';out=repo/'artifacts/low_spec'/a.profile
if out.exists():raise SystemExit(f'Refusing to overwrite {out}; archive it before repeating')
if not (app/'saves'/a.save/'meta.json').exists():raise SystemExit('Specified save is not present')
meta=json.loads((app/'saves'/a.save/'meta.json').read_text(encoding='utf-8-sig'))
if meta.get('timeScale')!=0:raise SystemExit('Benchmark requires an already-paused save (timeScale=0); do not modify save JSON')
for proc in Path('/proc').glob('[0-9]*/cmdline'):
 try:
  args=proc.read_bytes().split(b'\0')
  if args and Path(args[0].decode()).name=='City':raise SystemExit('Another City process is active; no concurrent runs')
 except (OSError,UnicodeError):pass
out.mkdir(parents=True);command=out/'command.json'
def save_hashes():
 return {str(f.relative_to(app/'saves'/a.save)):hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted((app/'saves'/a.save).rglob('*')) if f.is_file()}
original_save=save_hashes()
(out/'save_hashes.json').write_text(json.dumps(original_save,indent=2))
def streaming_signature():
 try:
  lines=(app/'debug.log').read_text(encoding='utf-8-sig').splitlines()
  return tuple(line for line in lines if any(tag in line for tag in ['[Streaming]','[TreeStreaming]','[LandscapeStreaming]']))
 except OSError:return ()
def emit(data):
 tmp=command.with_suffix('.tmp');tmp.write_text(json.dumps(data));tmp.replace(command)
emit({'id':1,'action':'benchmark_view','distance':150,'yaw':-.7,'pitch':.72,'hour':12,'speed':0})
args=['./City','--playtest-commands',str(command)]+(['--low-spec'] if a.profile=='low-spec' else [])+['--load',a.save]
(out/'launch.json').write_text(json.dumps({'args':args,'profile':a.profile,'save':a.save},indent=2))
start=time.time();log=(out/'runtime.log').open('w');memory=(out/'memory.csv').open('w',buffering=1);memory.write('seconds,rss_kb\n')
process=subprocess.Popen(args,cwd=app,stdout=log,stderr=subprocess.STDOUT)
def state():
 try:
  path=app/'playtest_state.json'
  if path.stat().st_mtime<start:return {}
  return json.loads(path.read_text(encoding='utf-8-sig'))
 except (OSError,ValueError):return {}
def wait(predicate,description):
 began=time.monotonic()
 while process.poll() is None:
  s=state()
  try:
   rss=next(l.split()[1] for l in Path(f'/proc/{process.pid}/status').read_text().splitlines() if l.startswith('VmRSS:'))
   memory.write(f'{time.time()-start:.2f},{rss}\n')
  except (OSError,StopIteration):pass
  if predicate(s):return s
  if time.monotonic()-began>1800:raise RuntimeError(f'Timeout: {description}; process {process.pid} left running for inspection')
  time.sleep(.5)
 raise RuntimeError(f'City exited {process.returncode} before {description}')
phases=[];command_id=1
for distance,warm,samples in ([(150,30,30)] if a.near_only else [(150,30,30),(3000,15,20)]):
 if distance!=150:
  command_id+=1;emit({'id':command_id,'action':'benchmark_view','distance':distance,'yaw':-.7,'pitch':.72,'hour':12,'speed':0})
 first=wait(lambda s:s.get('commandId',0)>=command_id,'camera command')
 warmstate=wait(lambda s:s.get('frame',0)>=first['frame']+warm,'warm frames')
 # Faster profiles must not be sampled before asynchronous geometry reaches the same state.
 # Require a full 30 rendered frames with no terrain/tree uploads, in addition to fixed warmup.
 settle_began=time.monotonic()
 while True:
  signature=streaming_signature()
  settled=wait(lambda s:s.get('frame',0)>=warmstate['frame']+30,'stable geometry frames')
  if streaming_signature()==signature:
   warmstate=settled;break
  warmstate=settled
  if time.monotonic()-settle_began>1800:raise RuntimeError('Geometry did not settle; process left running for inspection')
 sample_signature=streaming_signature()
 last=wait(lambda s:s.get('frame',0)>=warmstate['frame']+samples,'sample frames')
 # A quiet interval is only a heuristic for async completion. Reject samples with later uploads.
 if streaming_signature()!=sample_signature:
  raise RuntimeError('Geometry uploads occurred during sample; do not compare this run; process left available for inspection')
 phase={'distance':distance,'first_sample_frame':warmstate['frame'],'last_sample_frame':last['frame'],'start_state':warmstate,'end_state':last}
 phases.append(phase);(out/'phases.json').write_text(json.dumps(phases,indent=2))
 command_id+=1;emit({'id':command_id,'action':'snapshot'});capture=wait(lambda s:s.get('commandId',0)>=command_id,'capture')
 wait(lambda s:s.get('frame',0)>=capture['frame']+15,'capture presentation')
 snapshot=app/f'playtest_command_{command_id}.png'
 if not snapshot.exists():snapshot=app/'Screenshot'/f'playtest_command_{command_id}.png'
 if snapshot.exists():shutil.copy2(snapshot,out/f'view_{distance}.png')
command_id+=1;emit({'id':command_id,'action':'exit'});code=process.wait(timeout=300);memory.close();log.close()
if code!=0:raise SystemExit(f'City returned {code}')
# Archive only after TextWriter/DebugLog have closed and flushed their buffers.
for filename in ['debug.log','playtest_state.json','playtest_frames.csv']:
 shutil.copy2(app/filename,out/filename)
rows=list(csv.DictReader((out/'playtest_frames.csv').open(encoding='utf-8-sig')));summary={}
for phase in phases:
 chosen=[r for r in rows if phase['first_sample_frame']<int(r['frame'])<=phase['last_sample_frame']]
 expected=set(range(phase['first_sample_frame']+1,phase['last_sample_frame']+1))
 if {int(r['frame']) for r in chosen}!=expected or len(chosen)!=len(expected):
  raise RuntimeError(f'Incomplete or duplicate frame coverage for distance {phase["distance"]}')
 metrics={}
 for key in ['renderMs','shadowMs','gpuMs','buildingDrawCalls','cars','visibleCars','logicMs']:
  values=sorted(float(r[key]) for r in chosen);metrics[key]={'median':statistics.median(values),'p95':values[min(len(values)-1,int(len(values)*.95))],'min':values[0],'max':values[-1]}
 summary[str(phase['distance'])]={'frames':len(chosen),'metrics':metrics}
summary['save_unchanged']=save_hashes()==original_save
summary['peak_rss_kb']=max(int(r['rss_kb']) for r in csv.DictReader((out/'memory.csv').open()));summary['elapsed_seconds']=time.time()-start
(out/'summary.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2))
