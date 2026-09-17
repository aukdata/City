"""Author two Japanese cab-over goods vehicles for the existing Blender asset pipeline.

Run this script, then finish_realistic_town_blender.py -- --work neighborhood_realism.
Geometry is in metres. The game front is +X and tyre contact is Y=0.
"""
from pathlib import Path
import json
import math
import shutil
import numpy as np
from generate_detailed_buildings import Model
from build_civic_transport import loft, wheel
from build_realistic_town import rotate_append

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'artifacts/neighborhood_realism'


def truck(stem, large):
    model = Model(stem, 'Japanese three-axle cargo truck' if large else 'Japanese neighbourhood delivery truck')
    width, length, tyre = (2.45, 11.8, .49) if large else (1.95, 5.9, .36)
    front, rear = -length / 2, length / 2
    cabin_length = 2.05 if large else 1.65
    cabin_height = 2.9 if large else 2.35
    cabin_back = front + cabin_length
    body_base, body_height = (1.22, 2.4) if large else (.93, 1.94)
    model.box(0, tyre * .92, .3, width * .69, .24, length - .65, 11)
    loft(model, [(front, width/2-.11, .57, cabin_height-.12),
                 (front+.23, width/2, .57, cabin_height),
                 (cabin_back, width/2, .62, cabin_height)], 7)
    # Broad tilted windscreen, black surround, paired wipers and cab mirrors.
    lower, upper = cabin_height * .57, cabin_height-.20
    model.face([(-width*.41, lower, front-.006), (width*.41, lower, front-.006),
                (width*.40, upper, front+.15), (-width*.40, upper, front+.15)], 4)
    for side in (-1, 1):
        x = side*(width/2+.018)
        model.face([(x, lower, front+.25),(x, lower, cabin_back-.2),
                    (x, upper, cabin_back-.2),(x, upper, front+.25)][::side], 4)
        model.box(x, lower-.16, cabin_back-.31, .032, .045, .21, 11)
        model.box(side*(width/2-.06), .52, front+.69, .22, .055, .65, 13)
        model.beam((x, lower+.07, front+.2),(x+side*.22, lower+.21, front-.13),.022,11,8)
        model.box(x+side*.23, lower+.10, front-.15,.15,.38,.13,11)
        model.box(x+side*.235, lower+.14, front-.224,.11,.27,.025,13)
        model.beam((side*.11,lower+.055,front-.025),(side*width*.34,lower+.13,front-.025),.013,11,6)
        model.box(side*width*.32,.80,front-.046,.34,.15,.055,7)
        model.box(side*width*.43,.80,front-.05,.10,.15,.06,10)
    for y in (.73,.82,.91):
        model.box(0,y,front-.051,width*.40,.042,.025,11)
    model.box(0,.48,front-.025,width*.94,.19,.16,13)
    model.box(0,.54,front-.111,.33,.17,.018,7)
    cargo_start = cabin_back+.17
    cargo_length = rear-cargo_start-.14
    cargo_center = (cargo_start+rear-.14)/2
    model.box(0,body_base,cargo_center,width-.08,body_height,cargo_length,13)
    model.box(0,body_base+.035,cargo_center,width+.01,.09,cargo_length+.04,7)
    for side in (-1,1):
        x = side*(width/2-.026)
        # Shallow alloy ribs, lower side-impact bar and amber side reflectors.
        for z in np.arange(cargo_start+.18,rear-.2,.30 if large else .26):
            model.box(x,body_base+.15,float(z),.025,body_height-.29,.026,7)
        model.box(side*(width/2-.16),.61,cargo_center,.06,.095,cargo_length*.58,13)
        for z in (cargo_start+.30,rear-.48):
            model.box(x,body_base+.065,z,.023,.06,.16,10)
    # Twin rear doors, rubber seam, vertical lock bars, hinges and tail lift.
    for side in (-1,1):
        model.box(side*width*.245,body_base+.09,rear-.126,width*.47,body_height-.18,.035,7)
        model.beam((side*width*.25,body_base+.20,rear-.08),
                   (side*width*.25,body_base+body_height-.18,rear-.08),.023,13,8)
        model.box(side*width*.24,body_base+.65,rear-.05,.21,.055,.032,11)
        for y in (body_base+.30,body_base+body_height-.4):
            model.box(side*width*.44,y,rear-.074,.14,.11,.027,13)
        model.box(side*width*.33,.65,rear-.018,.28,.14,.06,9)
        model.box(side*width*.34,.83,rear-.026,.12,.065,.025,10)
    model.box(0,body_base+.09,rear-.101,.026,body_height-.18,.028,11)
    model.box(0,.39,rear-.025,width*.83,.14,.18,13)
    model.box(0,.65,rear+.002,.33,.16,.028,7)
    axles = [front+.88, rear-1.03]
    if large:
        axles = [front+1.02, rear-2.10, rear-.90]
    for z in axles:
        model.beam((-width*.42,tyre,z),(width*.42,tyre,z),.11,11,10)
        for side in (-1,1):
            wheel(model,side*(width/2-.10),tyre,z,tyre,.23 if large else .19)
            if z>cabin_back:
                model.box(side*(width/2-.14),.17,z+tyre+.04,.31,.44,.045,11)
    oriented=Model(stem,model.title)
    rotate_append(oriented,model,-math.pi/2)
    return oriented


def main():
    (WORK/'materials').mkdir(parents=True,exist_ok=True)
    for source in (ROOT/'artifacts/civic_transport/materials').glob('*.png'):
        shutil.copyfile(source,WORK/'materials'/source.name)
    records=[]
    for stem,large in [('delivery_truck',False),('cargo_truck',True)]:
        model=truck(stem,large)
        faces=[]
        for points,uv,normal in model.faces:
            u,v=uv.mean(0);col=min(7,int(u*8));row=min(3,int((1-v)*4));material=row*8+col
            local=np.column_stack(((uv[:,0]*1024-col*128-4)/120,((1-uv[:,1])*512-row*128-4)/120))
            faces.append({'p':points.tolist(),'uv':local.tolist(),'mat':material})
        item=dict(stem=stem,title=model.title,asset_dir='vehicles',kind='vehicle',source_triangles=len(faces),faces=faces)
        (WORK/f'{stem}.source.json').write_text(json.dumps(item,separators=(',',':')),encoding='utf-8')
        records.append({key:value for key,value in item.items() if key!='faces'})
    (WORK/'source_manifest.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
    print(json.dumps(records,indent=2))

if __name__=='__main__':
    main()
