"""Original metropolitan facilities in metres. Shared materials and authored three-level LODs."""
from pathlib import Path
import json, math
import numpy as np
import generate_detailed_buildings as base
ROOT=Path(__file__).resolve().parents[1]
DEST=ROOT/'App/assets/buildings/commercial'
class M(base.Model):
    def beam(self,start,end,radius,material,sides=8):
        super().beam(tuple(float(v) for v in start),tuple(float(v) for v in end),radius,material,sides)

def bench(m,x,z):
    m.box(x,.43,z,2.3,.12,.52,3);m.box(x,.64,z+.23,2.3,.55,.09,3)
    for xx in (-.82,.82):m.box(x+xx,0,z,.09,.43,.46,13)

def canopy(m,x,z,w=8,d=3):
    m.box(x,3.0,z,w,.16,d,5)
    for xx in (-w*.42,w*.42):m.box(x+xx,0,z+d*.32,.12,3,.12,13)

def props(m,size,level):
    if level==2:return
    edge=size*.5-1.6
    # Open forecourt with planted corners, benches and bollards. The entrance remains unobstructed.
    for x in (-edge,edge):
        for z in (-edge,edge):
            m.box(x,0,z,2.3,.45,2.3,6);m.box(x,.45,z,1.9,.8,1.9,12)
    if level==0:
        for x in (-size*.3,size*.3):bench(m,x,-edge+1)
        for x in (-3.5,3.5):m.beam((x,0,-edge),(x,.85,-edge),.07,13)
        # Bicycle shelter/racks are shared geometry across the facilities.
        canopy(m,edge-4,-edge+3,6,2.7)
        for x in np.arange(edge-6,edge-1,.8):
            m.beam((x,0,-edge+2.5),(x,.7,-edge+2.5),.025,13)
            m.beam((x,.7,-edge+2.5),(x,.7,-edge+3.2),.025,13)
        m.box(-edge,0,-edge+4,.8,1.8,.6,13);m.box(-edge,.65,-edge+3.68,.6,.85,.025,4)

def facade(m,w,d,y,h,z=0,level=0):
    # Glazed bands and vertical fins remain legible from the road; avoid thousands of window boxes.
    step=3.6 if level==0 else 7.2
    if level==2:return
    for yy in np.arange(y+1.0,y+h-1,step):
        for side in (-1,1):
            m.box(0,yy,z+side*(d/2+.025),w-.8,1.85,.045,4)
            m.box(side*(w/2+.025),yy,z,.045,1.85,d-.8,4)
    if level==0:
        for x in np.arange(-w/2+1,w/2,3.2):
            for side in(-1,1):m.box(x,y,z+side*(d/2+.075),.13,h,.15,13)

def parking(m,size,level):
    # Two separated rows, driving aisle and a pedestrian approach to the front door.
    for z in (-size*.33,-size*.15):
        for x in np.arange(-size*.43,size*.43,3):
            if abs(x)<4:continue
            if level<2:m.box(x,.035,z,.10,.025,5.1,7)
            if level==0:m.box(x+1.45,.055,z+2.1,1.65,.13,.18,6)
    if level<2:
        for x in (-size*.32,size*.32):
            m.beam((x,0,-size*.25),(x,8,-size*.25),.10,13)
            m.box(x,8,-size*.25,2.5,.18,.6,7)
        canopy(m,size*.32,-size*.43,12,3.5)
        m.box(-size*.42,0,-size*.42,.4,10,.4,13)
        m.box(-size*.42,7.8,-size*.42,5.5,3,.5,8)
        m.sign(-size*.42,8.1,-size*.42-.27,4.5,2.3,20)
    if level==0:
        for z in (-size*.33,-size*.15):
            for n,x in enumerate(np.arange(-size*.38,size*.38,6)):
                if abs(x)<5:continue
                m.box(x,.35,z,1.7,.75,4.2,[7,9,8,2,11][n%5])
                m.box(x,1.1,z,1.45,.6,2.3,4)
                for dx in(-.75,.75):
                    for dz in(-1.2,1.2):m.box(x+dx,.15,z+dz,.22,.5,.5,11)

def build(kind,variant,level):
    stem=f'{kind}_{variant:03d}';m=M(stem,kind)
    size={'office_tower':36,'city_hall':44,'shopping_mall':112,'hospital':44,'school':52}[kind]
    m.box(0,0,0,size-.2,.035,size-.2,11 if kind=='shopping_mall' else 6)
    if kind=='office_tower':
        m.box(0,.04,0,29,9,26,7);facade(m,29,26,.04,9,level=level)
        h=96 if variant==1 else 140
        m.box(0,9,1,22,h-9,20,2);facade(m,22,20,9,h-9,1,level)
        m.box(0,h,1,17,3,15,5)
        if level<2:
            canopy(m,0,-14,12,3)
            for x in(-6,0,6):m.box(x,h+3,1,3,1.8,4,13)
        if level==0:
            for x in(-8,8):m.box(x,12,11.2,.3,h-12,.6,7)
        m.beam((8,h+3,5),(8,h+7,5),.09,13,4 if level else 8)
    elif kind=='city_hall':
        m.box(0,.04,4,35,20,23,7);facade(m,35,23,.04,20,4,level)
        m.box(0,20,5,20,3,16,0)
        if level<2:
            canopy(m,0,-9,14,5);m.sign(0,4,-7.6,8,1.5,23)
            for x in(-12,12):m.beam((x,0,-12),(x,8,-12),.04,13);m.box(x+1,6.1,-12,2,1.4,.025,7)
            m.box(13,20,4,4,1.3,6,13)
    elif kind=='shopping_mall':
        m.box(0,.04,23,94,13,49,7);facade(m,94,49,.04,10,23,level)
        m.box(0,13,23,94,.5,49,8);m.box(0,13.5,23,87,1.2,43,5)
        parking(m,size,level)
        if level<2:
            canopy(m,0,-3,22,6);m.sign(0,9,-1.55,16,3,20)
            # Rear loading dock and screened rooftop plant, distinct from the public entrance.
            for x in(-30,-15,0,15,30):m.box(x,.05,48,5,4,.3,13)
        for x in(-27,-9,9,27):m.box(x,14.7,22,8,2.2,10,13)
    elif kind=='hospital':
        m.box(0,.04,4,34,21,25,7);facade(m,34,25,.04,21,4,level)
        m.box(0,21,4,22,3,16,0)
        if level<2:
            canopy(m,0,-11,20,5);m.sign(0,3.8,-8.6,9,1.2,21)
            m.box(-12,16,-8.6,1.1,4,.15,9);m.box(-12,17.4,-8.7,3.7,1.1,.15,9)
            m.box(13,0,-13,2.1,2.2,5,7);m.box(13,.9,-15.52,1.6,.8,.05,4)
    else:
        m.box(0,.04,14,42,11,13,0);facade(m,42,13,.04,11,14,level)
        m.box(16,.04,-6,14,8,21,7);m.box(16,8.04,-6,15,.7,22,5)
        m.box(-6,.04,-7,26,.02,24,14)
        if level<2:
            for x in(-15,3):
                m.beam((x,0,-17),(x,2.4,-17),.045,7);m.beam((x,0,-10),(x,2.4,-10),.045,7)
                m.beam((x,2.4,-17),(x,2.4,-10),.045,7)
            for x in(-24,24):m.box(x,0,0,.10,1.5,48,13)
    props(m,size,level)
    return m

def main():
    manifest_path=ROOT/'App/assets/lod/manifest.json'
    manifest=json.loads(manifest_path.read_text(encoding='utf-8'))
    report=[]
    for kind,n in [('office_tower',2),('city_hall',1),('shopping_mall',1),('hospital',1),('school',1)]:
        for variant in range(1,n+1):
            record={};base.DEST=DEST
            for level in range(3):
                model=build(kind,variant,level)
                source_stem=model.stem
                if level:
                    model.stem+=f'.lod{level}'
                    base.DEST=ROOT/'App/assets/lod/buildings/commercial';base.DEST.mkdir(parents=True,exist_ok=True)
                info=model.save()
                if level:
                    (base.DEST/f'{model.stem}.toml').unlink()
                    p=base.DEST/f'{model.stem}.mtl'
                    p.write_text(p.read_text().replace('town_atlas.png','../../../buildings/commercial/town_atlas.png'))
                    record['levels'].append({'level':level,'path':f'lod/buildings/commercial/{model.stem}.obj','triangles':info['triangles']})
                else:
                    record={'source':f'buildings/commercial/{source_stem}.obj','sourceTriangles':info['triangles'],'levels':[],'boundsMin':info['bounds_min'],'boundsMax':info['bounds_max']}
            manifest=[r for r in manifest if r['source']!=record['source']];manifest.append(record);report.append(record)
    manifest_path.write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    (DEST/'urban_facilities_manifest.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,indent=2))
if __name__=='__main__':main()
