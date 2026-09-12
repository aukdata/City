"""Author 17 additional buildings and refine the first seven for Blender baking.

No game files are replaced by this stage: JSON is the editable construction
source consumed by finish_realistic_town_blender.py.
"""
from pathlib import Path
import json
import math
import numpy as np
import generate_detailed_buildings as base

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT/'artifacts/realistic_town'
Model = base.Model

def rotate_append(model, part, angle, offset=(0,0,0)):
    c,s=math.cos(angle),math.sin(angle)
    matrix=np.array([[c,0,s],[0,1,0],[-s,0,c]])
    for points,uv,normal in part.faces:
        model.faces.append((points@matrix.T+offset, uv, normal@matrix.T))

def side_window(model, side, x, y, face_distance, width=1.1, height=1.1):
    part=Model('window','window')
    part.window(x,y,-face_distance,width,height)
    rotate_append(model,part,side*math.pi/2)

def rails(model, x, y, z, width, depth=.72, material=13):
    for dx in np.linspace(-width/2,width/2,max(3,int(width/.6)+1)):
        model.beam((x+dx,y,z-depth),(x+dx,y+1.03,z-depth),.018,material,6)
    for height in (.12,1.03):
        model.beam((x-width/2,y+height,z-depth),(x+width/2,y+height,z-depth),.023,material,6)
    for dx in (-width/2,width/2):
        model.beam((x+dx,y+1.03,z),(x+dx,y+1.03,z-depth),.023,material,6)

def balcony(model,x,y,z,width=2.5,solid=False):
    model.box(x,y,z-.39,width,.13,.85,6)
    rails(model,x,y+.13,z,width-.12)
    if solid:
        model.box(x,y+.2,z-.75,width-.15,.63,.055,7)
    model.window(x,y+.2,z+.01,width-.48,2.05)
    # Partition, drain, outdoor condenser and washing pole.
    model.box(x+width/2-.1,y+.15,z-.32,.045,1.65,.64,7)
    model.ac(x-width/2+.52,y+.22,z-.38)
    model.beam((x-.5,y+2.13,z-.58),(x+.7,y+2.13,z-.58),.016,13,6)
    for dx in (-.5,.7):
        model.beam((x+dx,y+2.13,z-.025),(x+dx,y+2.13,z-.64),.022,13,6)

def steps(model,x,z,width=1.0,count=4,rise=.14,run=.24,y=0):
    for i in range(count):
        model.box(x,y,z+i*run,width,(i+1)*rise,run,6)

def shrub(model,x,y,z):
    # Three overlapping irregular crowns instead of cylinder-shaped plants.
    for offset in (-.13,.12):
        for ring in range(1,6):
            a=(ring-1)*(math.pi-.04)/5+.02;b=ring*(math.pi-.04)/5+.02
            for segment in range(8):
                u=segment*math.tau/8;v=(segment+1)*math.tau/8
                def p(theta,phi):
                    return (x+offset+.21*math.sin(theta)*math.cos(phi),
                            y+.21+.25*math.cos(theta),z+.19*math.sin(theta)*math.sin(phi))
                model.face([p(a,u),p(b,u),p(b,v),p(a,v)],12)

def stair_flight(model,x,z,y,height,width=.8,run=2.6):
    count=max(4,round(height/.19))
    for i in range(count):
        model.box(x,y+(i+.2)*height/count,z+i*run/count,width,.08,run/count+.02,13)
    for dx in (-width/2,width/2):
        model.beam((x+dx,y,z),(x+dx,y+height,z+run),.038,13,6)
        model.beam((x+dx,y+.95,z),(x+dx,y+height+.95,z+run),.021,13,6)
        for t in np.linspace(0,1,5):
            model.beam((x+dx,y+t*height,z+t*run),(x+dx,y+t*height+.95,z+t*run),.016,13,6)

def house(stem,variant):
    names=['Showa tiled-roof house','Modern two-storey home','Timber bungalow','Urban compact house']
    m=Model(stem,names[variant])
    width,depth=5.8,5.5
    floors=1 if variant==2 else 2
    height=3.0*floors
    wall=[0,7,3,1][variant]
    m.box(0,0,0,7.25,.13,7.25,6)
    m.box(0,.13,.25,width,height,depth,wall)
    m.box(0,.13,.25,width+.04,.32,depth+.04,6)
    if variant in (0,2):
        roof=Model('roof','roof')
        roof.gable(6.45,6.25,height+.22,1.25,5)
        # Individual short curved tile courses, with small tonal changes.
        for side in (-1,1):
            for row in range(7):
                xa=side*(row/7*3.2)
                xb=side*((row+1)/7*3.2)
                ya=height+.24+1.25*(1-abs(xa)/3.2)
                yb=height+.24+1.25*(1-abs(xb)/3.2)
                for z in np.arange(-2.95,3.1,.42):
                    roof.beam((xa,ya+.035,z),(xb,yb+.035,z),.045,5,6)
        rotate_append(m,roof,0,(0,0,.25))
    elif variant==1:
        m.parapet(6.05,5.8,height+.13)
        for x in (-1.45,0,1.45):
            m.box(x,height+.29,.8,1.32,.10,2.2,4)
            for z in np.arange(-.25,1.95,.32):
                m.box(x,height+.395,z,1.3,.012,.012,13)
    else:
        m.parapet(6.05,5.85,height+.13)
        m.box(-1.65,.45,-2.56,1.3,height-.4,.15,3)
    for floor in range(floors):
        y=.8+floor*3
        for x in (-1.6,1.45):
            if floor==1 and variant!=0:
                balcony(m,x,y-.5,-2.56,2.2,variant==3)
            else:
                m.window(x,y,-2.55,1.3,1.38)
                m.box(x,y+1.51,-2.69,1.55,.065,.4,5)
        for side in (1,2,3):
            for x in (-1.55,1.35):
                side_window(m,side,x,y,2.94 if side!=2 else 3.04,.85,1.05)
    m.door(0,-2.61,.94,3)
    m.box(0,2.5,-2.93,1.5,.09,.8,5)
    m.box(0,.13,-2.99,1.45,.13,.62,6)
    m.box(2.7,.13,-3.32,.25,1.1,.22,7)
    m.box(2.7,.85,-3.48,.33,.3,.18,13)
    for x in (-3.35,3.35):
        m.box(x,.13,.1,.11,.63,6.5,6)
    for x in (-1.8,1.8):
        m.box(x,.13,-3.4,1.2,.36,.32,6)
        for dx in (-.4,0,.4):
                shrub(m,x+dx,.5,-3.4)
    m.service_details(width,depth+.5,height)
    m.ac(-2.1,.2,3.26)
    return m

def apartment(stem,floors,variant):
    m=Model(stem,['Exterior-corridor apartment','Brick apartment','Balcony condominium','Residential tower'][variant])
    width=6.1 if floors<=3 else 7.8
    depth=5.2 if floors<=3 else 6.8
    front=-depth/2
    height=floors*2.95
    m.box(0,0,0,width+1.0,.16,depth+2,6)
    m.box(0,.16,0,width,height,depth,1 if variant in (1,3) else 0)
    m.parapet(width+.22,depth+.22,height+.16)
    units=2 if floors<=3 else 3
    bay=width/units
    for floor in range(floors):
        y=.16+floor*2.95
        m.box(0,y,0,width+.08,.13,depth+.08,7)
        for unit in range(units):
            x=(unit-(units-1)/2)*bay
            balcony(m,x,y+.05,front-.03,bay-.05,variant in (1,3))
            # Back doors onto continuous access walkway.
            back=Model('corridor','corridor')
            back.box(x,y+.06,front-.35,bay,.13,.7,6)
            back.box(x,y+.21,front-.045,.88,2.07,.09,3)
            back.box(x-.22,y+1.2,front-.105,.035,.26,.05,13)
            rails(back,x,y+.19,front,bay-.06,.67)
            rotate_append(m,back,math.pi)
        for side in (1,3):
            for x in (-1.6,1.6):
                side_window(m,side,x,y+.9,width/2+.02,.82,1.1)
    # Exterior stairs fit within the declared plot; taller towers have a core.
    if floors<=3:
        for floor in range(floors-1):
            stair_flight(m,-width/2-.38,-1.25,.3+floor*2.95,2.95,.64,2.5)
    else:
        m.box(width/2-.7,height+.16,depth/2-.7,1.6,1.8,1.6,0)
    for x in (-1.4,0,1.4):
        m.ac(x,height+.36,.5)
    m.service_details(width,depth,height)
    return m

def office(stem,variant):
    m=Model(stem,'Brick business building' if variant==0 else 'Curtain-wall office')
    width,depth,floors=8.4,8.0,4
    m.box(0,0,0,9.1,.16,8.9,6)
    m.box(0,.16,0,width,13.1,depth,1 if variant==0 else 7)
    for side in range(4):
        face=4.02 if side%2==0 else 4.22
        for floor in range(floors):
            y=.7+floor*3.15
            for x in (-2.8,-1.4,0,1.4,2.8):
                side_window(m,side,x,y,face,1.12,2.15 if variant else 1.65)
        band=Model('band','band')
        for y in (3.25,6.4,9.55,12.7):
            band.box(0,y,-face-.06,8.4,.12,.16,13)
        rotate_append(m,band,side*math.pi/2)
    m.door(0,-4.14,1.65)
    m.box(0,2.9,-4.25,3.4,.14,.32,5)
    m.sign(0,3.1,-4.19,2.5,.45,23)
    m.parapet(8.65,8.25,13.26)
    m.box(-2,13.38,1.4,2.3,1.25,2.6,0)
    for x in (1,2.4):
        m.ac(x,13.46,1.2)
    m.beam((-2,14.65,1.4),(-2,15.5,1.4),.025,13)
    m.service_details(width,depth,13)
    return m

def extra_shops():
    result=[]
    for variant in range(3):
        m=Model(f'shop_{variant+4:03d}',['Ramen restaurant','Bakery and deli','Neighbourhood supermarket'][variant])
        m.box(0,0,0,7.75,.14,7.1,6)
        m.box(0,.14,.15,7.1,2.9,5.8,3 if variant==0 else 0)
        m.parapet(7.3,6.05,3.04)
        for x in (-2.65,-1.2,1.6,2.8):
            m.window(x,.5,-2.82,.97,1.65)
        m.door(.25,-2.83,1.05)
        m.sign(0,2.5,-3.0,4.8,.49,[26,27,16][variant])
        m.box(0,2.3,-3.04,7.3,.09,.65,9 if variant==0 else 8)
        if variant==0:
            for x in (-.2,.3,.8):
                m.box(x,1.82,-3.09,.48,.47,.028,8)
            for x in (-3.25,3.25):
                m.beam((x,1.9,-3.08),(x,2.23,-3.08),.14,9,12)
            m.beam((2.8,.2,2.9),(2.8,3.8,2.9),.17,13,12)
        elif variant==1:
            for x in (-2.6,-1.2):
                m.box(x,.14,-3.1,1.1,.5,.6,3)
                for dx in (-.32,0,.32):
                    m.beam((x+dx,.66,-3.22),(x+dx,.66,-2.99),.11,14,8)
        else:
            for x in (-2.8,-1.65):
                for z in (-3.12,-3.3):
                    m.beam((x,.18,z),(x,.85,z),.018,13,6)
                m.box(x,.44,-3.2,.67,.25,.28,13)
            m.box(3.3,.14,-3.1,.45,1.7,.48,7)
        m.ac(-1.1,3.23,1.5);m.ac(.1,3.23,1.5)
        m.service_details(7.1,6.1,2.9)
        result.append(m)
    return result

def extra_factories():
    m=Model('factory_003','Sawtooth-roof workshop')
    m.box(0,0,0,6.5,.15,6.5,6)
    m.box(0,.15,0,6.0,3.6,6.0,2)
    for i in range(3):
        z=-3+i*2
        m.face([(-3.1,3.75,z),(3.1,3.75,z),(3.1,4.9,z+1.7),(-3.1,4.9,z+1.7)],5)
        m.box(0,3.76,z+1.72,6.1,1.13,.08,4)
        m.box(0,4.85,z+1.72,6.16,.07,.12,13)
        for x in (-2,-1,0,1,2):
            m.box(x,3.76,z+1.77,.04,1.13,.04,13)
        for x in (-3.05,3.05):
            m.beam((x,3.75,z),(x,4.9,z+1.7),.055,13)
            # Close end caps of each sawtooth bay.
            face=[(x,3.75,z),(x,4.9,z+1.7),(x,3.75,z+1.7)]
            m.face(face if x>0 else face[::-1],2)
        m.box(0,3.73,z+1.87,6.2,.06,.28,5)
    m.box(-.5,.15,-3.05,3.6,2.8,.1,13)
    for y in np.arange(.3,2.95,.17):
        m.box(-.5,y,-3.12,3.5,.025,.04,5)
    m.door(2.35,-3.06,.8,13)
    m.sign(-.5,3.1,-3.08,2.8,.4,19)
    m.service_details(6,6,3.6)
    n=Model('factory_004','Service garage and utility tanks')
    n.box(0,0,0,6.5,.15,6.5,6)
    n.box(-.65,.15,0,4.4,3.5,5.8,0)
    roof=Model('roof','roof');roof.gable(4.7,6.15,3.68,.7)
    rotate_append(n,roof,0,(-.65,0,0))
    n.box(-.65,.15,-2.96,3.45,2.8,.1,13)
    for y in np.arange(.3,2.9,.18):
        n.box(-.65,y,-3.03,3.4,.025,.025,5)
    for z in (-1.6,1.1):
        n.beam((2.35,.2,z),(2.35,4.5,z),.56,13,16)
        n.beam((2.35,4.5,z),(2.35,4.7,z),.6,5,16)
        for y in (1,3.2):
            n.beam((2.35,y,z),(2.35,y+.06,z),.585,5,16)
    n.beam((2.35,1,-1.6),(2.35,1,1.1),.055,13,8)
    n.sign(-.65,3.13,-3.02,2.6,.4,19)
    service=Model('service','service');service.service_details(4.4,5.8,3.4)
    rotate_append(n,service,0,(-.65,0,0))
    return [m,n]

def extra_public():
    models=[]
    for variant in range(2):
        m=Model(f'public_{variant+3:03d}',['Neighbourhood police box','Community reading room'][variant])
        m.box(0,0,0,6.5,.14,6.5,6)
        m.box(0,.14,.1,5.9,6.0,5.7,7 if variant==0 else 1)
        m.parapet(6.12,5.95,6.14)
        for y in (.65,3.65):
            for x in (-1.85,0,1.85):
                m.window(x,y,-2.8,1.36,1.65)
        m.door(0,-2.9,1.2)
        m.sign(0,2.65,-2.95,3.7,.52,24 if variant==0 else 25)
        m.box(0,2.38,-3.03,3.0,.1,.45,8)
        for side in (1,2,3):
            for y in (.85,3.85):
                for x in (-1.6,1.6):
                    side_window(m,side,x,y,2.98,.95,1.3)
        m.ac(1.4,6.37,1.3)
        if variant==0:
            m.beam((-1.8,6.5,1.2),(-1.8,8.0,1.2),.025,13)
            m.beam((2.6,2.4,-3.08),(2.6,2.65,-3.08),.105,9,12)
            m.box(-2.5,.3,-3.02,.7,1.5,.13,5)
            m.box(-2.5,.4,-3.1,.6,1.3,.015,14)
        else:
            m.box(-2.1,.52,-3.07,1.1,.1,.38,3)
            for x in (-2.5,-1.7):
                m.box(x,.14,-3.07,.055,.38,.3,13)
        m.service_details(5.9,5.9,5.9)
        models.append(m)
    return models

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    from PIL import Image,ImageDraw
    material_dir=OUT/'materials'
    material_dir.mkdir(exist_ok=True)
    atlas=Image.open(ROOT/'App/assets/buildings/commercial/town_atlas.png')
    for index in range(32):
        x,y=index%8*128,index//8*128
        atlas.crop((x+4,y+4,x+124,y+124)).save(material_dir/f'{index:02d}.png')
    letters=dict(base.LETTERS)
    letters['B']=['11110','10001','10001','11110','10001','10001','11110']
    for index,label in enumerate(['POLICE','BOOKS','RAMEN','BAKERY'],24):
        tile=Image.new('RGB',(120,120),(35,61,83) if index==24 else (83,60,43))
        draw=ImageDraw.Draw(tile)
        draw.rectangle((3,22,116,99),outline=(228,225,211),width=2)
        scale=3;start=(120-(len(label)*6-1)*scale)//2
        for n,char in enumerate(label):
            for y,row in enumerate(letters[char]):
                for x,bit in enumerate(row):
                    if bit=='1':draw.rectangle((start+(n*6+x)*scale,32+y*8,start+(n*6+x)*scale+2,39+y*8),fill=(240,234,216))
        tile.save(material_dir/f'{index:02d}.png')
    # These functions construct the seven first-pack models without replacing files.
    models=base.shops()+base.factories()+base.public_buildings()
    models += [house(f'residential_{11+i:03d}',i) for i in range(4)]
    models += [apartment('residential_015',2,0),apartment('residential_016',3,1),
               apartment('residential_017',6,2),apartment('residential_018',12,3)]
    models += [office('office_003',0),office('office_004',1)]
    models += extra_shops()+extra_factories()+extra_public()
    records=[]
    for m in models:
        # Keep source faces/materials rather than committing preview-only shading.
        faces=[]
        for p,uv,n in m.faces:
            u,v=uv.mean(0)
            col=min(7,int(u*8));row=min(3,int((1-v)*4))
            material=row*8+col
            local=np.column_stack(((uv[:,0]*1024-col*128-4)/120,
                                   ((1-uv[:,1])*512-row*128-4)/120))
            faces.append({'p':p.tolist(),'uv':local.tolist(),'mat':material})
        folder='residential' if m.stem.startswith('residential') else 'commercial'
        record={'stem':m.stem,'title':m.title,'folder':folder,'source_triangles':len(faces),'faces':faces}
        (OUT/f'{m.stem}.source.json').write_text(json.dumps(record,separators=(',',':')),encoding='utf-8')
        records.append({k:v for k,v in record.items() if k!='faces'})
    (OUT/'source_manifest.json').write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8')
    print(f'{len(records)} construction sources written, {sum(r["source_triangles"] for r in records):,} source triangles')

if __name__=='__main__':
    main()
