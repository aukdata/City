"""Original Japanese-style EMUs: four-door commuter and three-door regional cars."""
from pathlib import Path
import json, math, shutil
import numpy as np
from PIL import Image, ImageDraw, ImageFont
import generate_detailed_buildings as base
from build_civic_transport import loft, wheel

ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'artifacts/japanese_trains'

def carriage(stem,regional,cab):
    model=base.Model(stem,'Japanese regional EMU' if regional else 'Japanese commuter EMU')
    # Metres, Y up, cab faces +Z. Body is shorter than the 20 m coupler spacing.
    loft(model,[(-9.75,1.32,.92,3.45),(-9.5,1.44,.92,3.62),(9.35,1.44,.92,3.62),(9.75,1.34,1.04,3.45)],13)
    for side in (-1,1):
        model.box(side*1.452,1.42,0,.024,.19,18.7,9 if regional else 8)
        model.box(side*1.453,3.15,0,.026,.08,18.5,9 if regional else 8)
        doors=(-6.2,0,6.2) if regional else (-7.0,-2.35,2.35,7.0)
        for z in doors:
            model.box(side*1.47,1.02,z,.035,2.13,1.32,7)
            for half in (-1,1): model.box(side*1.491,1.91,z+half*.31,.012,.84,.49,4)
            model.box(side*1.502,1.05,z,.008,2.07,.022,11)
            model.box(side*1.50,1.02,z,.08,.06,1.39,13)
        windows=(-8.4,-4.2,-2.1,2.1,4.2,8.3) if regional else (-8.65,-5.35,-3.98,-.8,.8,3.98,5.35,8.65)
        for z in windows:
            model.box(side*1.459,1.91,z,.025,1.11,1.41 if regional else 1.16,11)
            model.box(side*1.479,1.97,z,.012,.98,1.31 if regional else 1.06,4)
        model.box(side*1.49,2.96,-4.8,.014,.18,.75,30 if regional else 31)
    # Gangways, flexible bellows, couplers; no cab on intermediate cars.
    for z in (-9.79,9.79):
        model.box(0,1.0,z,1.02,2.12,.10,11)
        model.box(0,1.06,z+math.copysign(.055,z),.77,2.02,.02,7)
        model.box(0,1.95,z+math.copysign(.07,z),.48,.72,.015,4)
        model.box(0,.62,z,.26,.18,.42,11)
    if cab or regional:
        model.box(0,1.14,9.82,2.65,2.11,.075,11)
        for side in (-1,1):
            model.box(side*.70,2.02,9.87,1.04,.87,.024,4)
            model.box(side*1.03,1.57,9.89,.27,.14,.02,7)
            model.box(side*1.03,1.36,9.89,.16,.09,.02,12)
            model.beam((side*.30,2.04,9.903),(side*.72,2.35,9.903),.014,11)
        model.box(0,2.99,9.87,1.0,.19,.025,30 if regional else 31)
        model.box(0,1.45,9.91,1.52,.13,.02,9 if regional else 8)
        model.box(0,.72,9.73,2.22,.55,.13,5)
    for z in (-6.75,6.75):
        model.box(0,.37,z,2.04,.35,2.60,11)
        for dz in (-.84,.84):
            for side in (-1,1): wheel(model,side*.72,.37,z+dz,.37,.15)
            model.beam((-.71,.37,z+dz),(.71,.37,z+dz),.07,11,12)
    for z in (-3.5,0,3.5): model.box(0,.42,z,1.74,.37,1.9,5)
    for z in (-4.4,4.2):
        model.box(0,3.61,z,1.52,.31,2.6,7)
        for dx in (-.54,-.27,0,.27,.54): model.box(dx,3.94,z,.09,.012,2.14,11)
    if (regional and cab) or (not regional and not cab):
        model.box(0,3.64,-.3,1.06,.1,.9,11)
        for side in (-1,1):
            model.beam((side*.36,3.74,-.7),(side*.36,4.2,.10),.023,13)
            model.beam((side*.36,4.2,.10),(side*.36,4.80,-.50),.019,13)
        model.beam((-.72,4.80,-.5),(.72,4.80,-.5),.027,11)
    return model

def main():
    WORK.mkdir(parents=True,exist_ok=True);(WORK/'materials').mkdir(exist_ok=True)
    for path in (ROOT/'artifacts/civic_transport/materials').glob('*.png'): shutil.copyfile(path,WORK/'materials'/path.name)
    font=ImageFont.truetype('C:/Windows/Fonts/meiryo.ttc',33)
    for index,label in [(30,'普通'),(31,'快速')]:
        tile=Image.new('RGB',(120,120),(9,18,21));draw=ImageDraw.Draw(tile)
        draw.text((60,60),label,font=font,fill=(255,191,64),anchor='mm')
        tile.save(WORK/'materials'/f'{index:02d}.png')
    records=[]
    for regional in (True,False):
        for cab in (True,False):
            stem=('regional' if regional else 'urban')+('_cab' if cab else '_trailer')
            model=carriage(stem,regional,cab);faces=[]
            for p,uv,n in model.faces:
                u,v=uv.mean(0);col=min(7,int(u*8));row=min(3,int((1-v)*4))
                local=np.column_stack(((uv[:,0]*1024-col*128-4)/120,((1-uv[:,1])*512-row*128-4)/120))
                faces.append({'p':p.tolist(),'uv':local.tolist(),'mat':row*8+col})
            item=dict(stem=stem,title=model.title,folder='commercial',asset_dir='railway',kind='train',source_triangles=len(faces),faces=faces)
            (WORK/f'{stem}.source.json').write_text(json.dumps(item,separators=(',',':')),encoding='utf-8')
            records.append({k:v for k,v in item.items() if k!='faces'})
    (WORK/'source_manifest.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
    print('Authored four EMU car meshes')
if __name__=='__main__': main()
