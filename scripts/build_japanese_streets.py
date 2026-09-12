"""Original metre-scale street assets informed by the photo-reference register.

Run with Python + Pillow + NumPy. Fonts are rasterized locally, never bundled.
Reference photographs are not sampled or embedded in these game assets.
"""
from pathlib import Path
import json
import math
import numpy as np
from PIL import Image, ImageDraw, ImageFont
import generate_detailed_buildings as base
from build_realistic_town import rotate_append, side_window

ROOT = Path(__file__).resolve().parents[1]
REPORT = ROOT/'artifacts/urban_revision'
FONT = Path('C:/Windows/Fonts/BIZ-UDGothicB.ttc')
HORIZONTAL = ['まちの商店', '大衆食堂', '喫茶ひなた', '駅前歯科', '青葉ビル', '内科医院', '市民会館', '山の薬局']
VERTICAL = ['居酒屋', 'らーめん', 'カラオケ', '焼き鳥', '喫茶店', '診療所', '文具店', '酒と肴']

def make_atlas():
    atlas = Image.new('RGB',(1024,512))
    rng = np.random.default_rng(20260912)
    colors = list(base.COLORS)
    colors[4] = (67,82,86)
    for index in range(32):
        color = colors[index%16]
        tile = Image.fromarray(np.clip(np.asarray(color)[None,None,:]+rng.normal(0,2,(128,128,1)),0,255).astype('uint8'))
        draw = ImageDraw.Draw(tile)
        if index < 16:
            if index in (0,1,2,7):
                for y in range(0,128,16):
                    draw.line((0,y,127,y),fill=tuple(max(0,c-13) for c in color))
                    for x in range((y//16%2)*16,128,32):
                        draw.line((x,y,x,y+16),fill=tuple(max(0,c-10) for c in color))
            elif index in (3,5,13):
                for x in range(4,128,12): draw.line((x,0,x,128),fill=tuple(max(0,c-18) for c in color))
            elif index == 4:
                draw.rectangle((4,6,123,18),fill=(88,102,104))
                draw.rectangle((3,105,123,123),fill=(38,50,52))
        else:
            palette=[(32,72,66),(112,29,23),(226,219,195),(40,60,74),(67,47,33),(223,223,203),(45,47,51),(155,112,39)]
            bg=palette[index%8]; fg=(245,238,218) if sum(bg)<420 else (40,44,42)
            draw.rectangle((0,0,127,127),fill=bg)
            draw.rectangle((4,4,123,123),outline=fg,width=2)
            if index < 24:
                label=HORIZONTAL[index-16]
                font=ImageFont.truetype(str(FONT),min(26,112//len(label)))
                box=draw.textbbox((0,0),label,font=font)
                draw.text(((128-(box[2]-box[0]))/2,57-(box[3]-box[1])/2-box[1]),label,font=font,fill=fg)
                font=ImageFont.truetype(str(FONT),9)
                draw.text((22,89),'営業時間 10:00–22:00',font=font,fill=fg)
            else:
                label=VERTICAL[index-24]
                font=ImageFont.truetype(str(FONT),28)
                for i,char in enumerate(label):
                    box=draw.textbbox((0,0),char,font=font)
                    draw.text(((128-(box[2]-box[0]))/2,8+i*27-box[1]),char,font=font,fill=fg)
        atlas.paste(tile,((index%8)*128,(index//8)*128))
    atlas.save(base.DEST/'japan_street_atlas.png')

def vertical_sign(model,x,y,z,index):
    # UV uses the entire vertical tile rather than the horizontal sign crop.
    model.box(x,y,z,.68,2.25,.16,13)
    points=[(x-.31,y+.03,z-.085),(x+.31,y+.03,z-.085),(x+.31,y+2.22,z-.085),(x-.31,y+2.22,z-.085)]
    model.face(points[::-1],24+index%8,[(0,1),(1,1),(1,0),(0,0)][::-1])

def street_equipment(model,width,depth,variant):
    model.box(width/2-.52,0,-depth/2-.34,.78,1.83,.68,9)
    model.box(width/2-.52,.63,-depth/2-.69,.64,1.02,.025,7)
    for shelf in range(3):
        for bottle in range(4):
            model.box(width/2-.75+bottle*.15,.82+shelf*.23,-depth/2-.72,.075,.13,.04,8+(bottle+variant)%3)
    model.box(-width/2+.55,0,-depth/2-.36,.6,.16,.7,3)
    model.box(-width/2+.55,.16,-depth/2-.36,.7,.5,.65,12)
    # Menu board, meter boxes, rainwater pipe, paired condenser units.
    model.sign(-width/2+1.6,.15,-depth/2-.45,.65,1.15,17+variant%3)
    for y in (.4,3.4): model.ac(width/2-.55,y,depth/2+.1)
    model.service_details(width,depth,5.5)

def city_building(stem,floors,variant,detail=True):
    model=base.Model(stem,'Japanese mixed-use building with tenant signs and service equipment')
    width,depth=(8.0,8.2) if variant%2==0 else (9.1,8.0)
    height=floors*3.05
    model.box(0,0,0,width+.2,.18,depth+.2,6)
    model.box(0,.18,0,width,height,depth,[1,0,7,2][variant])
    for floor in range(floors):
        y=.3+floor*3.05
        model.box(0,y,0,width+.12,.10,depth+.12,6)
        for side in range(4):
            if not detail and side in (1,3): continue
            for x in (-2.5,0,2.5): side_window(model,side,x,y+.65,(depth if side%2==0 else width)/2+.025,1.85,1.68)
        if detail and floor>0 and floor<5:
            vertical_sign(model,-width/2+.42,y+.3,-depth/2-.23,variant+floor)
    model.door(0,-depth/2-.08,1.1)
    for x in (-2.5,2.4):
        model.window(x,.25,-depth/2-.05,2.05,2.0)
        model.sign(x,2.5,-depth/2-.28,2.65,.64,16+(variant+int(x))%8)
        model.box(x,2.35,-depth/2-.42,2.65,.12,.78,8+variant%3)
    # Blade signs perpendicular to the facade remain visible looking along a street.
    if detail:
        panel=base.Model('blade','blade')
        vertical_sign(panel,0,0,0,variant)
        rotate_append(model,panel,math.pi/2,(-width/2-.15,3.1,-depth/2+.6))
        rotate_append(model,panel,-math.pi/2,(-width/2-.15,3.1,-depth/2+.6))
        street_equipment(model,width,depth,variant)
    model.parapet(width+.2,depth+.2,height+.18)
    model.box(width/2-1.1,height+.2,depth/2-1.1,1.8,1.5,1.8,7)
    for x in (-2,0,2):
        if detail: model.ac(x,height+.25,0)
    return model

def shop_row(stem,variant,detail=True):
    model=base.Model(stem,'Two narrow local shopfronts with residences above')
    depth=8.0
    model.box(0,0,0,9.8,.16,depth+.2,6)
    for unit in range(2):
        x=-2.35+unit*4.7
        height=6.0+(unit if variant==0 else 1-unit)*2.7
        model.box(x,.16,0,4.55,height,depth,[0,1,2,3][variant*2+unit])
        model.box(x,height+.16,0,4.7,.18,depth+.15,5)
        model.door(x-1.2,-4.12,.95,3 if unit==0 else 4)
        model.window(x+.6,.32,-4.08,1.85,1.82)
        model.sign(x,2.5,-4.30,4.15,.65,16+(variant*3+unit)%8)
        model.box(x,2.35,-4.42,4.5,.12,.85,8+(variant+unit)%3)
        for floor in range(1,int(height/2.7)):
            for dx in (-1.1,1.1): model.window(x+dx,floor*2.8+.45,-4.05,1.3,1.3)
        if detail:
            vertical_sign(model,x+1.8,3.3,-4.26,variant*2+unit)
            for dx in (-1.2,0,1.2): model.box(x+dx,1.75,-4.32,1.12,.4,.025,8+variant)
            model.ac(x+1,3.15,-4.18)
            for dz in (-2.5,0,2.5): side_window(model,1 if unit==1 else 3,dz,3.4,4.72,1.15,1.0)
    if detail: street_equipment(model,9.2,8,variant)
    return model

def save(model,lod=False):
    destination=base.DEST
    if lod: base.DEST=ROOT/'App/assets/buildings/lod'
    record=model.save()
    mtl=base.DEST/f'{model.stem}.mtl'
    mtl.write_text(mtl.read_text().replace('town_atlas.png',('../commercial/' if lod else '')+'japan_street_atlas.png'))
    base.DEST=destination
    return record

if __name__=='__main__':
    make_atlas()
    records=[]
    for stem,floors,variant in [('office_005',7,0),('office_006',12,1),('shop_007',5,2),('shop_008',7,3)]:
        records.append(save(city_building(stem,floors,variant)))
        save(city_building(stem,floors,variant,False),True)
    for i in range(2):
        stem=f'shop_{i+9:03d}'
        records.append(save(shop_row(stem,i)))
        save(shop_row(stem,i,False),True)
    (REPORT/'japanese_streets.json').write_text(json.dumps(records,indent=2))
    print(json.dumps(records,indent=2))
