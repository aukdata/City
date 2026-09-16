from pathlib import Path
import json,time,shutil
root=Path.cwd();out=root/'artifacts/rural_landscape_ui';command=out/'command.json';state=root/'App/playtest_state.json';seq=100

def send(action,**values):
 global seq
 seq+=1;command.write_text(json.dumps(dict(id=seq,action=action,**values)),encoding='utf-8')
 if action=='exit':return {}
 deadline=time.monotonic()+180
 while time.monotonic()<deadline:
  try:
   s=json.loads(state.read_text(encoding='utf-8-sig'))
   if s.get('commandId')==seq:
    (out/f'actual_{seq}_{action}.json').write_text(json.dumps(s,indent=2),encoding='utf-8');print(seq,action,s.get('selectionKind'),s.get('selectionId'),s.get('fpsGraph'),flush=True);return s
  except (OSError,ValueError):pass
  time.sleep(.25)
 raise RuntimeError('Command timeout: '+action)

s=send('benchmark_view',x=24991.666149494653,z=17395.903864359487,distance=260,yaw=-.7,pitch=.85,speed=0)
time.sleep(4)
send('snapshot');send('select_parcel',x=24991.666149494653,z=17395.903864359487,capture=1)
send('key',code=114,capture=1);send('map_open');send('map_zoom',factor=4);send('map_pan',x=230,y=-145,capture=1);send('key',code=114,capture=1);send('key',code=114,capture=1);send('map_close')
send('key',code=67);send('drive',throttle=1,seconds=4);time.sleep(5);send('snapshot');send('key',code=67)
s=send('benchmark_view',x=14195,y=43,z=26206,distance=130,yaw=-.7,pitch=.72,hour=13,speed=1);time.sleep(5)
s=json.loads(state.read_text(encoding='utf-8-sig'));train=s['railway']['trains'][0];x,y,z=train['position'];send('benchmark_view',x=x,y=y,z=z,distance=130,yaw=-.7,pitch=.72,speed=0);time.sleep(3);send('snapshot');send('select_train',train=train['id'],capture=1)
send('benchmark_view',x=14231.843970940427,y=43,z=26222.885950287728,distance=3500,yaw=-.7,pitch=.72,speed=0);time.sleep(8)
send('exit');time.sleep(2)
shutil.copyfile(root/'App/playtest_frames.csv',out/'final_frames.csv');shutil.copyfile(root/'App/landscape_audit.json',out/'landscape_audit.json');shutil.copyfile(root/'App/road_ground_audit.json',out/'road_ground_audit.json');print('Controlled playtest complete',flush=True)
