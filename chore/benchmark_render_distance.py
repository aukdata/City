#!/usr/bin/env python3
"""Sequential render-distance control comparison; no save or simulation mutations."""
from pathlib import Path
import argparse,csv,json,statistics,subprocess,time,shutil,hashlib
p=argparse.ArgumentParser();p.add_argument('--profile',choices=['normal','low-spec'],required=True);p.add_argument('--save',required=True);p.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[1]);p.add_argument('--cutoff',type=float,default=500);p.add_argument('--ui-marker',type=Path);a=p.parse_args()
if not 100<=a.cutoff<=20000:p.error('--cutoff must be100–20000meters')
if a.ui_marker and a.ui_marker.exists():raise SystemExit('Stale UI marker; archive previous run before proceeding')
repo=a.repo.resolve();app=repo/'App';out=repo/'artifacts/render_distance'/a.profile
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
args=['./City','--playtest-commands',str(command),'--render-distance',str(a.cutoff)]+(['--low-spec'] if a.profile=='low-spec' else [])+['--load',a.save]
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
initial=wait(lambda s:s.get('commandId',0)>=1,'initial fixed camera')
if initial.get('renderDistanceMeters')!=a.cutoff:raise RuntimeError('CLI render distance was not applied at startup')
(out/'startup_state.json').write_text(json.dumps(initial,indent=2))
for index,(distance,warm,samples) in enumerate([(0,30,30),(a.cutoff,15,30),(0,15,30)]):
 command_id+=1;emit({'id':command_id,'action':'command','text':f'/render distance {distance}'})
 first=wait(lambda s:s.get('commandId',0)>=command_id,'camera command')
 warmstate=wait(lambda s:s.get('frame',0)>=first['frame']+warm,'warm frames')
 # Drain asynchronous geometry once. Later settings reuse this same camera/cache;
 # reject any new uploads instead of paying another cold-warmup interval.
 if index==0:
  settle_began=time.monotonic()
  while True:
   signature=streaming_signature()
   settled=wait(lambda s:s.get('frame',0)>=warmstate['frame']+30,'stable geometry frames')
   if streaming_signature()==signature:
    warmstate=settled;break
   warmstate=settled
   if time.monotonic()-settle_began>1800:raise RuntimeError('Geometry did not settle; process left running for inspection')
 elif streaming_signature()!=completed_geometry:
  raise RuntimeError('Changing render distance unexpectedly uploaded geometry; do not compare this run')
 sample_signature=streaming_signature();sample_started=time.time()-start
 last=wait(lambda s:s.get('frame',0)>=warmstate['frame']+samples,'sample frames')
 # A quiet interval is only a heuristic for async completion. Reject samples with later uploads.
 if streaming_signature()!=sample_signature:
  raise RuntimeError('Geometry uploads occurred during sample; do not compare this run; process left available for inspection')
 completed_geometry=streaming_signature()
 phase={'phase':index,'distance':distance,'sample_start_seconds':sample_started,'sample_end_seconds':time.time()-start,'first_sample_frame':warmstate['frame'],'last_sample_frame':last['frame'],'start_state':warmstate,'end_state':last}
 phases.append(phase);(out/'phases.json').write_text(json.dumps(phases,indent=2))
 command_id+=1;capture_started=time.time();emit({'id':command_id,'action':'snapshot'})
 wait(lambda s:s.get('commandId',0)>=command_id,'capture command')
 candidates=[app/f'playtest_command_{command_id}.png',app/'Screenshot'/f'playtest_command_{command_id}.png']
 wait(lambda s:any(f.exists() and f.stat().st_mtime>=capture_started for f in candidates),'capture presentation')
 snapshot=next(f for f in candidates if f.exists() and f.stat().st_mtime>=capture_started)
 shutil.copy2(snapshot,out/f'phase_{index}_distance_{distance}.png')
if a.ui_marker:
 (out/'awaiting_ui.json').write_text(json.dumps({'pid':process.pid,'commandId':command_id,'marker':str(a.ui_marker)},indent=2))
 print('AWAITING_DISTANCE_UI',flush=True)
 wait(lambda s:a.ui_marker.exists(),'native targeting review')
command_id=max(command_id,int(state().get('commandId',0)))+1;emit({'id':command_id,'action':'exit'});code=process.wait(timeout=300);memory.close();log.close()
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
 for key in ['renderMs','shadowMs','gpuMs','buildingDrawCalls','buildingsSubmitted','treesSubmitted','renderDistanceMeters','cars','visibleCars','logicMs']:
  values=sorted(float(r[key]) for r in chosen);metrics[key]={'median':statistics.median(values),'p95':values[min(len(values)-1,int(len(values)*.95))],'min':values[0],'max':values[-1]}
 summary[str(phase['phase'])]={'frames':len(chosen),'metrics':metrics}
for phase in phases:
 if phase['start_state'].get('renderDistanceMeters')!=phase['distance'] or phase['end_state'].get('renderDistanceMeters')!=phase['distance']:
  raise RuntimeError('Render-distance command did not persist for the whole sample')
summary['reset_counts_match']=all(summary['0']['metrics'][key]['median']==summary['2']['metrics'][key]['median'] for key in ['buildingDrawCalls','buildingsSubmitted','treesSubmitted'])
summary['save_unchanged']=save_hashes()==original_save
summary['peak_rss_kb']=max(int(r['rss_kb']) for r in csv.DictReader((out/'memory.csv').open()));summary['elapsed_seconds']=time.time()-start
(out/'summary.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2))
