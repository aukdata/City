"""Author civic facilities, car parks, stations and road/rail vehicles in metres."""
from pathlib import Path
import json
import math
import shutil
import numpy as np
from PIL import Image,ImageDraw
import generate_detailed_buildings as base
from build_realistic_town import rotate_append,side_window,steps,rails

ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'artifacts/civic_transport'
M=base.Model

def loft(m,sections,material):
    """Eight-sided coachwork cross-sections: z, half-width, bottom, top."""
    rings=[]
    for z,w,b,t in sections:
        chamfer=min(.13,(t-b)*.18)
        rings.append([(-w+.1,b,z),(w-.1,b,z),(w,b+chamfer,z),(w,t-chamfer,z),
                      (w-.1,t,z),(-w+.1,t,z),(-w,t-chamfer,z),(-w,b+chamfer,z)])
    m.face(rings[0][::-1],material,[(0,0)]*8)
    m.face(rings[-1],material,[(0,0)]*8)
    for a,b in zip(rings,rings[1:]):
        for i in range(8):
            j=(i+1)%8;m.face([a[j],b[j],b[i],a[i]],material)

def wheel(m,x,y,z,r=.31,width=.2):
    m.beam((x-width/2,y,z),(x+width/2,y,z),r,11,16)
    outer=x+math.copysign(width/2+.007,x)
    m.beam((outer,y,z),(outer+math.copysign(.016,x),y,z),r*.65,13,12)
    for angle in np.arange(0,math.tau,math.tau/5):
        m.beam((outer,y,z),(outer,y+r*.52*math.cos(angle),z+r*.52*math.sin(angle)),.024,7,6)

def car(stem,kind):
    m=M(stem,{'sedan':'Japanese family sedan','kei':'Kei tall wagon','patrol':'Police patrol sedan',
              'fire':'Urban fire engine','bus':'Low-floor city bus'}[kind])
    bus=kind=='bus';fire=kind=='fire';kei=kind=='kei'
    width=2.45 if bus else 2.1 if fire else 1.48 if kei else 1.78
    length=10.5 if bus else 6.5 if fire else 3.39 if kei else 4.5
    body=9 if fire else 7
    lo,hi=-length/2,length/2
    m.box(0,.29,0,width*.8,.2,length*.84,11)
    if bus or fire:
        cabin_end=hi-.2 if bus else lo+2.15
        loft(m,[(lo,width/2-.09,.57,2.8 if bus else 2.5),(lo+.3,width/2,.55,3.0 if bus else 2.65),
                (cabin_end,width/2,.55,3.0 if bus else 2.65)],body)
        m.box(0,1.35,lo-.012,width-.34,1.15,.025,4)
        m.box(0,1.35,lo-.035,.045,1.2,.045,11)
        if fire:
            m.box(0,.6,1.0,width,1.9,4.0,9)
            for side in (-1,1):
                for z in (-.15,1.15,2.45):
                    m.box(side*(width/2+.012),.85,z,.03,1.3,1.1,13)
                    for y in np.arange(.95,2.1,.13):m.box(side*(width/2+.032),y,z,.022,.025,1.08,5)
            for x in (-.45,.45):
                m.beam((x,2.61,-.3),(x,2.61,2.8),.035,13)
            for z in np.arange(-.2,2.8,.3):m.beam((-.45,2.61,z),(.45,2.61,z),.03,13)
            m.beam((0,2.9,lo+.6),(0,3.02,lo+.6),.22,9,12)
        else:
            for side in (-1,1):
                for z in np.arange(lo+.8,hi-.4,1.3):
                    m.box(side*(width/2+.012),1.5,z,.035,.95,1.08,4)
                m.box(side*(width/2+.025),1.15,0,.028,.2,length-.3,8)
            for z in (-3.3,2.5):
                m.box(width/2+.035,.65,z,.045,1.9,1.13,4)
                m.box(width/2+.065,.65,z,.02,1.9,.035,13)
            m.box(0,3.0,.8,1.5,.28,2.0,13)
    else:
        loft(m,[(lo,.64 if kei else .72,.5,.9),(lo+.25,width/2,.43,1.03),
                (hi-.2,width/2,.43,1.03),(hi,width/2-.15,.55,.91)],7)
        front=lo+.5 if kei else lo+1.35
        rear=hi-.4 if kei else hi-1.0
        top=1.72 if kei else 1.46
        loft(m,[(front-.28,width/2-.07,.96,1.04),(front+.16,width/2-.19,1.0,top),
                (rear-.18,width/2-.19,1.0,top),(rear+.28,width/2-.07,.96,1.06)],7)
        # Interpolate the actual loft roof plane, leaving a 15mm outward gap.
        # Fixed heights previously put most of the glass inside the body.
        def glazing(z0,z1,y0,y1,w0,w1):
            points=[]
            for t,side in ((.12,-1),(.12,1),(.88,1),(.88,-1)):
                points.append((side*(w0+(w1-w0)*t-.135),
                               y0+(y1-y0)*t+.015,z0+(z1-z0)*t))
            m.face(points[::-1],4)
        glazing(front-.28,front+.16,1.04,top,width/2-.07,width/2-.19)
        glazing(rear-.18,rear+.28,top,1.06,width/2-.19,width/2-.07)
        for side in (-1,1):
            # Side glass follows the sloped cabin rather than sitting on a box.
            m.face([(side*(width/2-.065),1.06,front+.03),(side*(width/2-.065),1.06,rear-.02),
                    (side*(width/2-.18),top-.055,rear-.2),(side*(width/2-.18),top-.055,front+.2)][::-side],4)
            mid=(front+rear)/2
            m.beam((side*(width/2-.06),1.03,mid),(side*(width/2-.175),top,mid),.026,11,6)
            for z in (front+.25,rear-.15):
                m.box(side*(width/2+.012),.94,z,.022,.04,.14,13)
            m.box(side*(width/2+.09),1.06,front+.09,.19,.12,.16,7)
            if kind=='patrol':m.box(side*(width/2+.015),.55,0,.025,.32,length*.9,11)
        if kind=='patrol':
            m.box(0,top+.03,0,.8,.12,.27,9)
            for x in (-.34,.34):m.beam((x,top+.12,0),(x,top+.22,0),.09,9,12)
    for z in (lo+length*.2,hi-length*.2):
        for side in (-1,1):wheel(m,side*(width/2-.03),.39 if(bus or fire) else .31,z,.39 if(bus or fire) else .31)
    for x in (-width*.31,width*.31):
        m.box(x,.74,lo-.025,.29,.16,.06,7)
        m.box(x,.78,hi+.025,.27,.17,.06,9)
    m.box(0,.51,lo-.04,.6,.12,.045,11)
    m.box(0,.52,hi+.045,.32,.13,.025,10 if kei else 7)
    for y in (.64,.7,.76):m.box(0,y,lo-.052,width*.45,.025,.025,13)
    return m

def parking(stem,canopy=False):
    m=M(stem,'Solar-canopy parking' if canopy else 'Coin-operated parking')
    m.box(0,0,0,10.6,.09,10.8,11)
    for x in (-5.2,5.2):m.box(x,.09,0,.14,.16,10.8,6)
    m.box(0,.09,5.3,10.4,.16,.14,6)
    for x in (-3.8,-1.25,1.3,3.85):
        m.box(x,.092,2.55,.09,.008,4.95,7)
    for x in (-2.52,.02,2.57):
        m.box(x,.1,4.7,1.55,.12,.16,6)
        m.box(x,.1,.5,.7,.08,.32,13)
    m.box(-4.55,.09,-3.2,.65,1.7,.55,7)
    m.box(-4.55,.8,-3.49,.44,.65,.025,4)
    m.beam((-4.6,.09,-4.7),(-4.6,3.2,-4.7),.065,13)
    m.sign(-4.6,2.45,-4.7,.85,.8,29)
    for x in (-2.5,2.5):m.beam((x,.09,-4.8),(x,.8,-4.8),.055,10)
    parked=car('parked','kei');rotate_append(m,parked,0,(.02,.09,2.4))
    if canopy:
        for x in (-3.8,3.8):
            for z in (.1,4.95):m.box(x,.09,z,.12,2.8,.12,13)
        m.box(0,2.87,2.5,8.0,.12,5.2,5)
        for x in (-2.65,0,2.65):
            for z in (1.2,3.8):
                m.box(x,3.0,z,2.48,.07,2.4,4)
                for dz in (-.8,0,.8):m.box(x,3.075,z+dz,2.45,.008,.015,13)
    return m

def civic(stem,kind):
    names=['District police station','Neighbourhood koban','Municipal fire station','City hall']
    m=M(stem,names[kind]);w=9.5 if kind!=1 else 5.8;d=8.0 if kind!=1 else 5.4
    height=[9.3,5.8,7.0,12.3][kind]
    m.box(0,0,0,w+.6,.16,d+1.0,6)
    m.box(0,.16,.15,w,height,d,0 if kind!=3 else 7)
    m.parapet(w+.2,d+.2,height+.16)
    for floor in range(round(height/3.1)):
        for x in np.arange(-w/2+.9,w/2-.3,1.6):
            if kind==2 and floor==0:continue
            m.window(x,.8+floor*3.1,-d/2+.09,1.13,1.55)
        for side in (1,2,3):
            for x in (-d/2+1.1,d/2-1.1):side_window(m,side,x,.8+floor*3.1,w/2+.025 if side!=2 else d/2+.32,.95,1.4)
    if kind==2:
        for x in (-3.05,0,3.05):
            m.box(x,.16,-d/2+.035,2.75,2.6,.1,9)
            for y in np.arange(.3,2.7,.17):m.box(x,y,-d/2-.025,2.7,.025,.035,13)
            m.box(x,1.5,-d/2-.05,2.45,.45,.03,4)
    else:
        m.door(0,-d/2-.05,1.55 if kind!=1 else 1.05)
        m.box(0,2.65,-d/2-.32,3.3,.13,.85,13)
    m.sign(0,2.98,-d/2-.04,min(6,w-.5),.65,24+kind)
    for x in (-1.5,0,1.5):m.ac(x,height+.36,1.8)
    if kind in (0,1,2):
        m.beam((w/2-.7,height+.2,1),(w/2-.7,height+1.3,1),.024,13)
    if kind==1:m.beam((-2.3,2.8,-d/2-.4),(-2.3,3.0,-d/2-.4),.12,9,12)
    if kind==3:
        for x in (-3.4,3.4):
            m.beam((x,.16,-d/2-.22),(x,5.3,-d/2-.22),.028,13)
            m.box(x+.3,4.45,-d/2-.22,.6,.5,.018,7)
        m.box(0,height+.16,1.4,2.5,1.0,2,0)
    m.service_details(w,d+.3,height)
    return m

def station(stem,modern):
    m=M(stem,'Suburban station and footbridge' if modern else 'Rural station with platform')
    # Track centre X=0; platform edge stays >=1.85m from the centre line.
    m.box(3.65,0,0,3.6,1.05,26,6)
    m.box(2.2,1.051,0,.3,.012,25.5,10)
    m.box(1.91,1.051,0,.1,.012,25.8,7)
    for z in (-10,-5,0,5,10):
        m.box(4.25,1.05,z,.12,2.55,.12,13)
        m.beam((2.0,3.45,z),(5.1,3.7,z),.055,13)
    m.box(3.6,3.7,0,3.4,.12,24,5)
    for z in (-7,3,8):
        m.box(4.3,1.47,z,.6,.08,1.7,3)
        for dz in (-.65,.65):m.box(4.3,1.05,z+dz,.4,.42,.055,13)
    # Building set behind the platform; no rail/ballast is embedded in the asset.
    building=M('stationhouse','stationhouse')
    building.box(0,0,0,5.8,.16,8.4,6)
    building.box(0,.16,0,5.3,3.1,7.7,7 if modern else 3)
    if modern:building.parapet(5.6,8.0,3.26)
    else:building.gable(5.8,8.3,3.3,1.1)
    building.door(0,-3.9,1.7)
    for x in (-1.7,1.7):building.window(x,.65,-3.88,1.0,1.65)
    building.sign(0,2.75,-3.94,4,.46,28)
    rotate_append(m,building,math.pi/2,(8.1,0,0))
    # Steps connect ground-side entrance to the platform.
    steps(m,5.5,-6.5,1.3,7,.15,.28)
    if modern:
        m.box(-3.65,0,0,3.6,1.05,26,6)
        m.box(-2.2,1.051,0,.3,.012,25.5,10)
        m.box(-3.65,3.7,0,3.5,.12,24,5)
        for z in (-10,-5,0,5,10):m.box(-4.3,1.05,z,.12,2.6,.12,13)
        m.box(0,5.3,9,11,.25,2.3,6)
        for x in (-5.2,5.2):m.box(x,1.05,9,.23,4.25,.23,13)
        for z in (7.9,10.1):m.box(0,5.55,z,11,1.1,.075,13)
        # Two stair flights behind the shelters, clear of the track envelope.
        from build_realistic_town import stair_flight
        for x in (-4.3,4.3):stair_flight(m,x,2.7,1.05,4.25,1.0,6.2)
    return m

def train(stem,variant):
    m=M(stem,'Stainless commuter EMU' if variant==0 else 'Regional electric railcar')
    width=2.8;length=20.0
    loft(m,[(-10,1.22,1.0,3.35),(-9.6,1.4,.95,3.65),(9.6,1.4,.95,3.65),(10,1.22,1.0,3.35)],13 if variant==0 else 7)
    for side in (-1,1):
        m.box(side*1.409,1.4,0,.025,.23,19.2,8 if variant==0 else 9)
        for z in (-7.4,-2.5,2.5,7.4):
            m.box(side*1.427,1.03,z,.025,2.13,1.23,7)
            m.box(side*1.449,1.9,z,.02,.98,1.03,4)
            m.box(side*1.466,1.04,z,.012,2.1,.025,13)
        for z in (-8.8,-5.8,-4.2,-.85,.85,4.2,5.8,8.8):
            m.box(side*1.418,1.95,z,.028,1.03,1.28,4)
    for z in (-10.015,10.015):
        m.box(0,2.25,z,2.02,.9,.024,4)
        m.box(0,2.25,z+math.copysign(.018,z),.045,.91,.03,13)
        for x in (-.85,.85):m.box(x,1.55,z,.27,.17,.04,7)
        m.box(0,.67,z,.32,.22,.32,11)
    for z in (-6.7,6.7):
        m.box(0,.41,z,2.12,.45,2.6,11)
        for dz in (-.85,.85):
            for side in (-1,1):wheel(m,side*.74,.39,z+dz,.39,.16)
    for z in (-3,0,3):m.box(0,.5,z,1.5,.36,1.55,5)
    for z in (-5,1,6):m.box(0,3.65,z,1.35,.29,1.9,7)
    for z in (-3.0,3.0):
        m.box(0,3.65,z,1.1,.11,.85,11)
        for x in (-.38,.38):
            m.beam((x,3.76,z-.4),(x,4.25,z),.025,13)
            m.beam((x,4.25,z),(x,4.55,z-.25),.025,13)
        m.beam((-.7,4.55,z-.25),(.7,4.55,z-.25),.027,11)
    return m

def main():
    WORK.mkdir(parents=True,exist_ok=True);(WORK/'materials').mkdir(exist_ok=True)
    for path in (ROOT/'artifacts/realistic_town/materials').glob('*.png'):shutil.copyfile(path,WORK/'materials'/path.name)
    letters=dict(base.LETTERS);letters.update({'B':['11110','10001','10001','11110','10001','10001','11110'],
        'D':['11110','10001','10001','10001','10001','10001','11110']})
    for index,label in enumerate(['POLICE','KOBAN','FIRE','CITY HALL','STATION','P','LOCAL','RAPID'],24):
        tile=Image.new('RGB',(120,120),(27,58,83));draw=ImageDraw.Draw(tile)
        scale=min(3,108//(len(label)*6));start=(120-(len(label)*6-1)*scale)//2
        for i,char in enumerate(label):
            for y,row in enumerate(letters[char]):
                for x,bit in enumerate(row):
                    if bit=='1':draw.rectangle((start+(i*6+x)*scale,32+y*8,start+(i*6+x)*scale+scale-1,39+y*8),fill=(241,239,224))
        tile.save(WORK/'materials'/f'{index:02d}.png')
    entries=[]
    for i in range(2):entries.append((parking(f'parking_{i+1:03d}',i==1),'buildings/commercial','parking'))
    for i in range(4):entries.append((civic(f'public_{i+5:03d}',i),'buildings/commercial','civic'))
    for i in range(2):entries.append((station(f'station_{i+1:03d}',i==1),'railway','station'))
    for stem,kind in [('sedan','sedan'),('kei_wagon','kei'),('patrol_car','patrol'),('fire_engine','fire'),('city_bus','bus')]:
        raw=car(stem,kind);oriented=M(stem,raw.title);rotate_append(oriented,raw,-math.pi/2)
        entries.append((oriented,'vehicles','vehicle'))
    for i in range(2):
        raw=train(f'commuter_{i+1:03d}',i);oriented=M(raw.stem,raw.title)
        rotate_append(oriented,raw,math.pi) # Train front +Z, heading atan2(x,z).
        entries.append((oriented,'railway','train'))
    records=[]
    for m,folder,kind in entries:
        faces=[]
        for p,uv,n in m.faces:
            u,v=uv.mean(0);col=min(7,int(u*8));row=min(3,int((1-v)*4));material=row*8+col
            local=np.column_stack(((uv[:,0]*1024-col*128-4)/120,((1-uv[:,1])*512-row*128-4)/120))
            faces.append({'p':p.tolist(),'uv':local.tolist(),'mat':material})
        item=dict(stem=m.stem,title=m.title,folder='commercial',asset_dir=folder,kind=kind,
                  source_triangles=len(faces),faces=faces)
        (WORK/f'{m.stem}.source.json').write_text(json.dumps(item,separators=(',',':')),encoding='utf-8')
        records.append({k:v for k,v in item.items() if k!='faces'})
    (WORK/'source_manifest.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
    print('Authored',len(records),'civic and transport assets')

if __name__=='__main__':main()
