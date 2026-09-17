"""Original urban/rural service plots and lived-in country houses, in metres.

The complete lot is modelled: parking aisles, entrances, pumps and outbuildings
remain at their real dimensions. Run before finish_realistic_town_blender.py.
"""
from pathlib import Path
import json
import math
import shutil
import numpy as np
from PIL import Image, ImageDraw, ImageFont
import generate_detailed_buildings as base
from build_realistic_town import rotate_append, side_window
from build_civic_transport import car

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'artifacts/roadside_life'
M = base.Model


def parking_bay(m, x, z, accessible=False):
    for dx in (-1.3, 1.3):
        m.box(x + dx, .101, z, .09, .01, 5.0, 7)
    m.box(x, .11, z + 2.1, 1.6, .12, .16, 6)
    if accessible:
        m.box(x, .102, z, 2.35, .008, 4.7, 8)
        for dx, dz, w, d in ((0, -.5, .14, 1), (.4, 0, .9, .14), (.8, .3, .14, .7)):
            m.box(x+dx, .113, z+dz, w, .008, d, 7)


def lot(m, width, depth):
    m.box(0, 0, 0, width, .1, depth, 11)
    # Side/rear curbs; the road-facing boundary has two 6m entrances.
    for x in (-width/2+.12, width/2-.12):
        m.box(x, .1, 0, .24, .18, depth, 6)
    m.box(0, .1, depth/2-.12, width, .18, .24, 6)
    m.box(0, .1, -depth/2+.12, max(1, width-15), .18, .24, 6)
    for x in (-width/2+3.5, width/2-3.5):
        m.box(x, .101, -depth/2+.3, 5.6, .015, .38, 13)
    for x in (-width/2+1, width/2-1):
        m.beam((x, .1, 1), (x, 5.8, 1), .07, 13)
        m.box(x, 5.8, .55, .45, .12, 1.0, 7)


def shop_shell(m, width, depth, center_z, floors=1):
    shell = M('shell', '')
    shell.box(0, .1, 0, width, 3.65*floors, depth, 7)
    shell.parapet(width+.1, depth+.1, 3.65*floors+.1)
    front = -depth/2-.035
    for x in np.arange(-width/2+1.1, width/2-.5, 1.65):
        shell.window(x, .45, front, 1.48, 2.1)
    shell.door(0, front-.03, 1.65)
    shell.box(0, 2.8, front-.25, width+.2, .18, .8, 13)
    shell.box(0, 3.03, front-.27, width+.2, .42, .2, 8)
    shell.sign(0, 3.11, front-.385, 5.6, .23, 16)
    for floor in range(1, floors):
        for x in np.arange(-width/2+1.2, width/2-.4, 2.3):
            shell.window(x, 1+floor*3.65, front, 1.5, 1.6)
    for x in (-width/2+1, width/2-1):
        shell.ac(x, 3.65*floors+.35, depth/2-1)
    rotate_append(m, shell, 0, (0, 0, center_z))
    return center_z-depth/2


def convenience(rural):
    stem = 'convenience_roadside_001' if rural else 'convenience_urban_001'
    m = M(stem, 'Roadside convenience store, 12 bays' if rural else 'Urban convenience store with upstairs tenancy')
    if rural:
        lot(m, 36, 32)
        front = shop_shell(m, 23, 10, 10)
        m.box(0, .1, front-1, 24, .08, 2, 6)
        for x in np.arange(-14.3, 14.4, 2.6):
            parking_bay(m, x, 1.25, abs(x+14.3)<.1)
        for x in (-9.1, 6.5):
            rotate_append(m, car('parked', 'kei' if x<0 else 'sedan'), math.pi, (x, .11, 1.25))
        m.beam((15, .1, -12), (15, 7.4, -12), .13, 13)
        m.sign(15, 5.6, -12, 3.5, 1.65, 16)
        m.sign(15, 4.5, -12, 3.0, .7, 19)
    else:
        m.box(0, 0, 0, 14, .1, 14, 6)
        front = shop_shell(m, 13.4, 10.8, 1.0, 3)
        # A walk-in city shop: bicycles and bins instead of an oversized forecourt.
        for x in (-5.5, -4.6, -3.7):
            m.beam((x, .1, -5.9), (x, .75, -5.9), .025, 13)
            m.beam((x, .75, -5.9), (x, .75, -5.2), .025, 13)
        m.sign(5.8, 4.3, -4.7, 1.0, 1.3, 16)
    for x in (-1.9, 1.9):
        m.beam((x, .1, front-.85), (x, 1, front-.85), .055, 13)
    for x in (-4.1, -3.4):
        m.box(x, .1, front-.55, .6, .85, .5, 7)
        m.box(x, .74, front-.815, .4, .12, .02, 11)
    m.box(4.3, .1, front-.4, .85, 1.8, .7, 9)
    m.box(4.3, .75, front-.76, .65, .9, .02, 4)
    return m


def fuel_station(rural):
    stem = 'fuel_roadside_001' if rural else 'fuel_urban_001'
    m = M(stem, 'Rural self-service station with wash bay' if rural else 'Compact urban filling station')
    width, depth = (44, 38) if rural else (24, 24)
    lot(m, width, depth)
    office = M('office', '')
    office.box(0, .1, 0, 12 if rural else 9, 3.2, 6, 7)
    office.parapet(12.2 if rural else 9.2, 6.2, 3.3)
    office.door(-2.2, -3.04)
    office.window(1.1, .65, -3.035, 3, 1.6)
    office.sign(0, 2.5, -3.05, 5, .48, 17)
    office.ac(2, 3.5, 1)
    rotate_append(m, office, 0, (-6 if rural else -4, 0, depth/2-4))
    canopy_width = 27 if rural else 19
    canopy_z = -2.5 if rural else -3
    m.box(0, 5.2, canopy_z, canopy_width, .36, 13 if rural else 11, 7)
    m.box(0, 5.26, canopy_z-(6.5 if rural else 5.5), canopy_width, .27, .1, 9)
    m.sign(0, 5.29, canopy_z-(6.56 if rural else 5.56), 5.5, .2, 17)
    for x in ((-9, 0, 9) if rural else (-5, 5)):
        m.box(x, .1, canopy_z, 1.45, .18, 5.5, 6)
        m.box(x, .28, canopy_z+1.1, .25, 4.92, .25, 13)
        m.box(x, .28, canopy_z-.8, .9, 1.65, .7, 7)
        m.box(x, 1.1, canopy_z-1.16, .68, .55, .03, 11)
        for side, material in ((-1, 9), (1, 10)):
            points = [(x+side*.49,1.7,canopy_z-.8),(x+side*.72,1.1,canopy_z-.8),
                      (x+side*.73,.57,canopy_z-.8),(x+side*.50,.75,canopy_z-.8)]
            for a, b in zip(points, points[1:]):
                m.beam(a, b, .035, 11, 6)
            m.box(x+side*.48, .75, canopy_z-.8, .08, .27, .12, material)
        m.box(x, 5.16, canopy_z-2.5, .35, .025, 1.5, 7)
    m.beam((width/2-1.5,.1,-depth/2+1.5),(width/2-1.5,7,-depth/2+1.5),.13,13)
    m.sign(width/2-1.5, 5.0, -depth/2+1.5, 2.5, 1.65, 18)
    if rural:
        for x in (8, 11, 14, 17):
            parking_bay(m, x, 14)
        # Drive-through wash frame, service island and tyre rack.
        for x in (14, 19):
            m.box(x,.1,6,.35,3.5,.55,9)
        m.box(16.5,3.6,6,5.4,.5,.65,7)
        m.sign(16.5,3.68,5.66,4.5,.3,20)
        m.box(19,.1,-6,.8,1.5,.6,7)
        rotate_append(m, car('parked','sedan'),0,(10,.11,-3))
    else:
        m.box(7,.1,8,4.5,.15,5.5,6)
        m.box(7,.25,10,4.5,2.8,.3,13)
        m.sign(7,2.2,9.8,3.5,.45,20)
    return m


def rural_house(variant):
    m = M(f'rural_house_{variant+1:03d}', ['Tile-roof farmhouse with barn', 'Timber country house with storehouse', 'Modern rural home with carport'][variant])
    m.box(0, 0, 0, 20, .12, 20, 6)
    m.box(0,.12,0,19.7,.015,19.7,14)
    house = M('house','')
    height = 3.15 if variant==1 else 5.7
    house.box(0,0,0,11,.3,8,6)
    house.box(0,.3,0,10.5,height,7.5,3 if variant==1 else 0)
    house.gable(11.5,8.7,height+.3,2.05 if variant!=2 else 1.2)
    if variant!=2:
        for z in np.arange(-4.25,4.3,.42):
            house.beam((-5.74,height+.31,z),(0,height+2.35,z),.045,5,6)
            house.beam((0,height+2.35,z),(5.74,height+.31,z),.045,5,6)
    for floor in range(1 if variant==1 else 2):
        for x in (-3.6,0,3.6):
            house.window(x,.85+floor*2.7,-3.785,2.1,1.45)
    house.door(-1.65,-3.81,1.5,3 if variant!=2 else 13)
    house.box(0,.3,-4.25,10.5,.18,.9,3)
    house.box(0,2.8,-4.3,11,.12,1.6,5)
    for x in (-5,5):
        house.box(x,.3,-4.7,.13,2.5,.13,3)
    for side in (1,3):
        for x in (-2.2,2.2):
            side_window(house,side,x,1.1,5.3,1.5,1.25)
    house.service_details(10.5,7.5,height)
    rotate_append(m,house,0,(-2.2,.12,3.5))
    # Keep a clear 4m gate and gravel drive to the outbuilding.
    for x in (-7.2,6):
        m.box(x,.13,-9.6,5 if x<0 else 7.2,.65,.24,6)
    for z in (-6,-2,2,6):
        m.box(-9.6,.13,z,.45,1.1,3.7,12)
    m.box(6.6,.13,4,5.2,.15,8.5,6)
    if variant==2:
        for x in (4.3,8.9):
            for z in (0,7.8):
                m.box(x,.28,z,.12,2.55,.12,13)
        m.box(6.6,2.83,4,5.3,.13,8.5,13)
    else:
        barn=M('barn','')
        barn.box(0,0,0,4.8,3.3,5.8,2 if variant==0 else 7)
        barn.gable(5.2,6.2,3.3,.8)
        barn.box(0,.1,-2.93,3.5,2.8,.08,3)
        for x in np.arange(-1.7,1.8,.3):
            barn.box(x,.1,-2.98,.045,2.8,.035,13)
        rotate_append(m,barn,0,(6.6,.28,4.5))
    rotate_append(m,car('parked','kei'),math.pi,(6.4,.15,-3.6))
    m.box(-6.8,.14,-5.5,3.5,.15,3.2,14)
    for z in (-6.5,-5.7,-4.9):
        for x in (-8,-7.2,-6.4,-5.6):
            m.beam((x,.29,z),(x,.65,z),.2,12,6)
    m.box(-3.9,.13,-9.4,.4,1.2,.35,6)
    m.box(-3.9,1.1,-9.55,.38,.27,.25,13)
    return m


def main():
    (WORK/'materials').mkdir(parents=True,exist_ok=True)
    for path in (ROOT/'artifacts/realistic_town/materials').glob('*.png'):
        shutil.copyfile(path,WORK/'materials'/path.name)
    for index in range(32):
        path=WORK/'materials'/f'{index:02d}.png'
        if not path.exists():
            noise=np.random.default_rng(index).normal(0,2,(120,120,1))
            color=np.asarray(base.COLORS[index%16])[None,None,:]
            Image.fromarray(np.clip(color+noise,0,255).astype('uint8')).save(path)
    for index,label in enumerate(['くらしマート','ひなた石油','セルフ 24H','P 24時間','洗車・整備'],16):
        tile=Image.new('RGB',(120,120),(26,66,61) if index in (16,19) else (140,33,24))
        draw=ImageDraw.Draw(tile)
        font=ImageFont.truetype('C:/Windows/Fonts/BIZ-UDGothicB.ttc',max(12,112//len(label)))
        box=draw.textbbox((0,0),label,font=font)
        draw.text(((120-box[2])/2,48-box[1]),label,font=font,fill=(244,241,221))
        tile.save(WORK/'materials'/f'{index:02d}.png')
    models=[convenience(False),convenience(True),fuel_station(False),fuel_station(True)]
    models += [rural_house(i) for i in range(3)]
    records=[]
    for model in models:
        faces=[]
        for points,uv,_ in model.faces:
            u,v=uv.mean(0); col=min(7,int(u*8)); row=min(3,int((1-v)*4))
            local=np.column_stack(((uv[:,0]*1024-col*128-4)/120,((1-uv[:,1])*512-row*128-4)/120))
            faces.append(dict(p=points.tolist(),uv=local.tolist(),mat=row*8+col))
        folder='residential' if model.stem.startswith('rural_house') else 'commercial'
        item=dict(stem=model.stem,title=model.title,folder=folder,kind='building',source_triangles=len(faces))
        (WORK/f'{model.stem}.source.json').write_text(json.dumps(dict(item,faces=faces),separators=(',',':')),encoding='utf-8')
        records.append(item)
    (WORK/'source_manifest.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
    print('Authored',len(models),'urban/rural site assets')


if __name__=='__main__':
    main()
