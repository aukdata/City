"""Original construction machinery in metres, +Z forward, Y up.

Photo references and measured proportions: artifacts/road_construction/REFERENCES.md.
Author physical shells, running gear and working attachments; Blender bakes local
occlusion into one atlas per game model. No manufacturer artwork is redistributed.
"""
from pathlib import Path
import math,json
import numpy as np
from PIL import Image,ImageDraw
import generate_detailed_buildings as base
from build_civic_transport import loft
ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'artifacts/construction_models'
M=base.Model
YELLOW,BLACK,STEEL,GLASS,WHITE,BLUE,RUBBER,CHROME=10,11,13,4,7,8,5,2

def prism(m,points,x0,x1,mat):
    """Closed extruded YZ profile, used for hollow-looking fabricated platework."""
    points=list(points)
    area=sum(points[i][0]*points[(i+1)%len(points)][1]-points[(i+1)%len(points)][0]*points[i][1] for i in range(len(points)))
    if area<0:points.reverse()
    # Ear clipping keeps the concave boom profile closed without crossing its elbow.
    def cross(a,b,c):return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])
    indices=list(range(len(points)));triangles=[]
    while len(indices)>3:
        for k,b in enumerate(indices):
            a=indices[k-1];c=indices[(k+1)%len(indices)]
            if cross(points[a],points[b],points[c])<=1e-10:continue
            if any(cross(points[a],points[b],points[j])>=-1e-10 and cross(points[b],points[c],points[j])>=-1e-10 and cross(points[c],points[a],points[j])>=-1e-10 for j in indices if j not in (a,b,c)):continue
            triangles.append((a,b,c));indices.pop(k);break
        else:raise ValueError('Invalid extruded profile')
    triangles.append(tuple(indices))
    for tri in triangles:
        m.face([(x0,*points[i]) for i in tri[::-1]],mat,[(0,0)]*3)
        m.face([(x1,*points[i]) for i in tri],mat,[(0,0)]*3)
    for i,(y,z) in enumerate(points):
        yy,zz=points[(i+1)%len(points)]
        m.face([(x0,y,z),(x0,yy,zz),(x1,yy,zz),(x1,y,z)],mat)

def rod(m,a,b,r=.04,mat=STEEL,sides=10):m.beam(a,b,r,mat,sides)
def hose(m,points,r=.025):
    for a,b in zip(points,points[1:]):rod(m,a,b,r,BLACK,7)
def ram(m,a,b,r=.095):
    a=np.array(a,dtype=float);b=np.array(b,dtype=float)
    rod(m,a,a+(b-a)*.60,r,YELLOW,16);rod(m,a+(b-a)*.57,b,r*.52,CHROME,16)
    rod(m,a-(0.13,0,0),a+(0.13,0,0),r*1.35,STEEL,12)
    rod(m,b-(0.1,0,0),b+(0.1,0,0),r,STEEL,12)
def lamps(m,x,y,z):
    m.box(x,y,z,.28,.20,.16,BLACK);m.box(x,y+.025,z+.084,.23,.145,.015,WHITE)
def grille(m,x,y,z,w,h,side=False):
    m.box(x,y,z,.035 if side else w,h,w if side else .035,BLACK)
    for dy in np.arange(.05,h,.075):m.box(x+.025 if side else x,y+dy,z if side else z+.026,.035 if side else w,.021,w if side else .025,STEEL)
def seat(m,x,y,z):
    m.box(x,y,z,.52,.12,.52,BLACK);m.box(x,y+.1,z-.25,.54,.58,.12,BLACK)
    for s in (-1,1):m.box(x+s*.32,y+.28,z,.09,.09,.42,BLACK)

def tire(m,x,y,z,r=.8,width=.45):
    sides=32;rings=[]
    for dx,rr in [(-width*.5,r*.81),(-width*.39,r*.97),(0,r),(width*.39,r*.97),(width*.5,r*.81)]:
        rings.append([(x+dx,y+rr*math.cos(t),z+rr*math.sin(t)) for t in np.arange(0,math.tau,math.tau/sides)])
    for a,b in zip(rings,rings[1:]):
        for i in range(sides):j=(i+1)%sides;m.face([a[i],b[i],b[j],a[j]][::-1],RUBBER)
    for s in (-1,1):
        xx=x+s*(width*.5+.007);rod(m,(xx,y,z),(xx+s*.04,y,z),r*.52,BLUE,24)
        rod(m,(xx+s*.04,y,z),(xx+s*.1,y,z),r*.23,STEEL,16)
        for t in np.arange(0,math.tau,math.tau/8):
            rod(m,(xx+s*.05,y+r*.36*math.cos(t),z+r*.36*math.sin(t)),(xx+s*.07,y+r*.36*math.cos(t),z+r*.36*math.sin(t)),.04,STEEL,6)
    for t in np.arange(0,math.tau,math.tau/28):
        for s in (-1,1):
            a=(x,y+r*math.cos(t),z+r*math.sin(t));b=(x+s*width*.42,y+r*.97*math.cos(t+.11),z+r*.97*math.sin(t+.11))
            rod(m,a,b,.045,BLACK,4)

def tracks(m,length=3.6,width=2.6):
    m.box(0,.47,0,width*.75,.40,length*.82,BLACK)
    radius=.48;straight=length*.5-radius
    for side in (-1,1):
        x=side*(width*.5-.28)
        for z in np.linspace(-straight,straight,7):
            rod(m,(x-.24,.51,z),(x+.24,.51,z),.34,STEEL,20)
            rod(m,(x+side*.245,.51,z),(x+side*.275,.51,z),.14,BLACK,12)
        path=[]
        for z in np.linspace(-straight,straight,17):path.append((.03,z))
        for t in np.linspace(-math.pi/2,math.pi/2,10)[1:]:path.append((.51+radius*math.sin(t),straight+radius*math.cos(t)))
        for z in np.linspace(straight,-straight,17)[1:]:path.append((.99,z))
        for t in np.linspace(math.pi/2,math.pi*1.5,10)[1:]:path.append((.51+radius*math.sin(t),-straight+radius*math.cos(t)))
        for i,p in enumerate(path):
            q=path[(i+1)%len(path)];a=np.array(p);b=np.array(q);d=b-a
            if np.linalg.norm(d)<.015:continue
            c=(a+b)*.5;along=d/np.linalg.norm(d)*min(.12,np.linalg.norm(d)*.45);normal=np.array([-along[1],along[0]])*.32
            profile=[c-along-normal,c+along-normal,c+along+normal,c-along+normal]
            prism(m,profile,x-.29,x+.29,RUBBER)
            rod(m,(x-.28,c[0]+normal[0]*1.6,c[1]+normal[1]*1.6),(x+.28,c[0]+normal[0]*1.6,c[1]+normal[1]*1.6),.027,STEEL,4)

def cabin(m,x,y,z,w=1.0,d=1.6,h=1.55,color=YELLOW):
    # Sloped front, dark structural glazing gaskets, real roof and door frame.
    profile=[(y,z-d*.5),(y,z+d*.5),(y+h*.75,z+d*.5),(y+h,z+d*.32),(y+h,z-d*.45)]
    prism(m,profile,x-w*.5,x+w*.5,BLACK)
    for s in (-1,1):
        xx=x+s*(w*.5+.008)
        pts=[(xx,y+.25,z-d*.39),(xx,y+.25,z+d*.39),(xx,y+h*.74,z+d*.39),(xx,y+h-.08,z+d*.24),(xx,y+h-.08,z-d*.37)]
        m.face(pts[::-s],GLASS,[(0,1),(1,1),(1,.3),(.85,0),(0,0)])
        m.box(xx,y+.1,z,w*.025,.16,d*.85,color)
        rod(m,(xx,y+.19,z+.05),(xx,y+h-.05,z+.05),.023,BLACK,6)
        m.box(xx+s*.025,y+.57,z-.1,.02,.04,.18,STEEL)
    m.face([(x-w*.42,y+.32,z+d*.5+.012),(x+w*.42,y+.32,z+d*.5+.012),(x+w*.42,y+h*.74,z+d*.5+.012),(x-w*.42,y+h*.74,z+d*.5+.012)],GLASS)
    m.face([(x-w*.42,y+h*.78,z+d*.48+.015),(x+w*.42,y+h*.78,z+d*.48+.015),(x+w*.42,y+h-.055,z+d*.33+.012),(x-w*.42,y+h-.055,z+d*.33+.012)],GLASS)
    m.box(x,y+h,z-.06,w+.10,.095,d*.86,color)
    rod(m,(x,y+.38,z+d*.51+.02),(x+.18,y+h*.73,z+d*.51+.02),.012,BLACK,5)
    lamps(m,x-w*.3,y+h-.05,z+d*.37);seat(m,x,y+.15,z-.15)

def excavator():
    m=M('excavator','20 tonne hydraulic excavator');tracks(m,4.15,2.85)
    rod(m,(0,1.05,0),(0,1.24,0),.90,STEEL,32)
    loft(m,[(-2.05,1.22,1.24,2.03),(-1.8,1.4,1.24,2.12),(.95,1.4,1.24,2.02),(1.45,1.2,1.24,1.6)],YELLOW)
    m.box(.75,1.5,-.72,1.08,.87,1.65,YELLOW);grille(m,1.415,1.64,-.85,1.25,.55,True)
    grille(m,0,1.43,-2.06,1.9,.43);m.box(.5,2.1,-1.2,.8,.07,.9,BLACK)
    for z in np.arange(-1.5,-.8,.1):m.box(.5,2.185,z,.73,.02,.035,STEEL)
    cabin(m,-.72,1.45,.53,1.16,1.7,1.61)
    rod(m,(.89,2.2,-1.3),(.89,2.85,-1.3),.065,BLACK,12)
    # Fabricated gooseneck boom with tapered plates instead of straight box beams.
    prism(m,[(1.55,.83),(2.05,1.4),(4.38,2.58),(5.05,4.1),(5.32,4.17),(4.82,2.28),(2.55,1.12)],.25,.70,YELLOW)
    prism(m,[(5.27,4.04),(5.18,4.48),(1.3,5.53),(.97,5.27)],.32,.64,YELLOW)
    for y,z in [(1.9,1.2),(5.14,4.2),(1.12,5.38)]:rod(m,(.19,y,z),(.77,y,z),.12,STEEL,16)
    ram(m,(.47,1.55,1.0),(.47,3.7,2.25),.12)
    ram(m,(.48,4.62,2.65),(.48,5.48,4.13),.09)
    ram(m,(.48,4.45,4.52),(.48,1.85,5.41),.08)
    hose(m,[(.72,1.75,.8),(.76,2.6,1.6),(.76,4.3,2.65),(.73,4.88,3.9),(.76,5.38,4.22)])
    for offset in (-.045,.045):hose(m,[(.82+offset,1.7,1),(.85+offset,2.3,1.0),(.78+offset,2.8,1.5)],.018)
    # Curved open bucket, cutting edge and five replaceable teeth.
    shell=[(.19+.91*(1-math.cos(t)),5.15+.91*math.sin(t)) for t in np.linspace(0,math.pi*.65,14)]
    for (y,z),(yy,zz) in zip(shell,shell[1:]):
        m.face([(-.12,y,z),(1.07,y,z),(1.07,yy,zz),(-.12,yy,zz)],YELLOW)
        m.face([(-.10,y+.04,z),( -.10,yy+.04,zz),(1.05,yy+.04,zz),(1.05,y+.04,z)],STEEL)
    for x in (-.13,1.06):prism(m,shell+[(1.15,5.35)],x,x+.035,YELLOW)
    m.box(.47,.12,5.14,1.30,.14,.22,STEEL)
    for x in np.linspace(-.03,.97,5):prism(m,[(.13,5.05),(.11,4.76),(.28,5.15)],x-.065,x+.065,STEEL)
    rod(m,(-1.42,1.55,.4),(-1.42,2.30,.4),.025,BLACK)
    rod(m,(-1.42,2.3,.4),(-1.75,2.4,.55),.025,BLACK);m.box(-1.75,2.28,.55,.035,.25,.19,BLACK)
    return m

def roller():
    m=M('road_roller','Tandem vibratory asphalt roller')
    for z in (-1.1,1.1):
        rod(m,(-.77,.59,z),(.77,.59,z),.59,STEEL,48)
        for s in (-1,1):
            rod(m,(s*.78,.59,z),(s*.86,.59,z),.51,YELLOW,32)
            rod(m,(s*.87,.59,z),(s*.91,.59,z),.2,BLACK,16)
            prism(m,[(.30,z-.19),(.35,z+.33),(1.26,z+.27),(1.4,z-.24)],s*.89-.05,s*.89+.05,YELLOW)
        m.box(0,.88,z+.58,1.42,.065,.06,BLACK)
        rod(m,(-.69,1.00,z+.59),(.69,1.00,z+.59),.023,STEEL)
    loft(m,[(-1.83,.69,1.10,1.42),(-1.3,.83,.99,1.72),(.2,.82,.96,1.67),(1.7,.68,1.04,1.28)],BLUE)
    m.box(0,1.05,1.64,1.52,.37,.1,YELLOW)
    for x in (-.61,.61):lamps(m,x,1.15,1.7)
    m.box(0,1.30,-.61,1.25,.10,.95,BLACK);seat(m,0,1.4,-.72)
    rod(m,(0,1.38,-.12),(0,1.94,.14),.045,BLACK)
    for t in np.arange(0,math.tau,math.tau/16):
        p=(.27*math.cos(t),1.99+.12*math.sin(t),.2+.21*math.sin(t));q=(.27*math.cos(t+math.tau/16),1.99+.12*math.sin(t+math.tau/16),.2+.21*math.sin(t+math.tau/16));rod(m,p,q,.019,BLACK,6)
    for s in (-1,1):
        grille(m,s*.835,1.28,.55,.64,.20,True)
        rod(m,(s*.58,1.45,-1.18),(s*.58,2.3,-1.18),.025,BLACK)
        m.box(s*.65,1.08,-.55,.22,.12,.72,STEEL)
        rod(m,(s*.65,1.48,-.58),(s*.65,1.88,-.4),.018,BLACK)
    return m

def paver():
    m=M('asphalt_paver','Wheeled asphalt finisher with telescopic screed')
    m.box(0,.4,0,2.35,.43,4.9,BLACK)
    for x in (-1.04,1.04):
        tire(m,x,.66,-.60,.66,.38)
        for z in (1.70,2.35):tire(m,x,.36,z,.36,.24)
    loft(m,[(-1.4,1.05,.78,1.50),(-.9,1.18,.8,1.88),(.3,1.18,.74,1.84),(1.05,1.05,.58,1.05)],YELLOW)
    for s in (-1,1):grille(m,s*1.19,1.05,-.12,.68,.48,True)
    # Hopper: broad open mouth, sloping steel wings, visible dark material and conveyor.
    for s in (-1,1):
        m.face([(s*.58,.50,.75),(s*1.52,1.30,1.3),(s*1.54,1.12,3.15),(s*.70,.49,3.3)][::s],BLUE)
        m.face([(s*.65,.46,.7),(s*1.56,1.33,1.3),(s*1.56,1.15,3.15),(s*.7,.45,3.35)][::-s],BLACK)
        ram(m,(s*.8,.57,1.3),(s*1.36,1.15,1.6),.07)
    m.box(0,.45,2.04,1.45,.10,2.5,BLACK)
    for z in np.arange(.85,3.25,.19):m.box(0,.565,z,1.35,.025,.04,STEEL)
    m.box(0,.65,1.7,1.40,.16,1.35,BLACK)
    rod(m,(-.84,.34,3.42),(.84,.34,3.42),.15,BLACK,16)
    # Operator platform, two seats, control consoles and supported folding canopy.
    m.box(0,1.37,-1.38,2.45,.14,1.37,STEEL)
    for s in (-1,1):
        seat(m,s*.7,1.68,-1.60)
        m.box(s*.78,1.52,-.85,.50,.85,.42,BLACK)
        m.box(s*.78,2.39,-.85,.52,.06,.43,BLUE)
        for dx in (-.12,0,.12):rod(m,(s*.78+dx,2.45,-.85),(s*.78+dx,2.56,-.89),.017,BLACK,6)
        for z in (-2.00,-.7):rod(m,(s*1.10,1.5,z),(s*1.10,3.43,z),.042,STEEL,10)
        for y in (.54,.83,1.12):m.box(s*1.16,y,-1.83,.32,.055,.52,STEEL)
        rod(m,(s*1.22,.85,-2.14),(s*1.22,2.15,-2.14),.025,STEEL)
    m.box(0,3.43,-1.34,2.72,.14,2.40,BLUE)
    m.box(0,3.42,-1.34,2.79,.04,2.48,STEEL)
    # Screed steel sole, extension guides, auger shaft and rear access tread.
    m.box(0,.04,-2.73,4.7,.13,1.10,STEEL);m.box(0,.18,-2.58,2.4,.53,.77,BLUE)
    for s in (-1,1):
        m.box(s*1.67,.19,-2.75,1.04,.36,.60,BLUE)
        for y in (.40,.64):rod(m,(s*.8,y,-2.83),(s*2.19,y,-2.83),.065,CHROME,12)
        m.box(s*2.35,.05,-2.70,.045,.53,1.2,STEEL)
    rod(m,(-2.20,.34,-2.03),(2.20,.34,-2.03),.13,STEEL,16)
    for x in np.arange(-2.12,2.15,.26):rod(m,(x,.30,-2.05),(x+.07,.50,-1.92),.12,STEEL,6)
    m.box(0,.38,-3.22,2.52,.08,.44,STEEL)
    return m

def crane():
    m=M('mobile_crane','Rough terrain crane with four outriggers and telescopic boom')
    loft(m,[(-3.35,1.16,.85,1.32),(-2.95,1.32,.78,1.46),(2.95,1.32,.78,1.46),(3.4,1.1,.85,1.32)],BLUE)
    for x in (-1.23,1.23):
        for z in (-2.05,2.05):tire(m,x,.85,z,.85,.55)
    for z in (-3.24,3.24):
        m.box(0,1.06,z,2.7,.32,.28,WHITE)
        for s in (-1,1):lamps(m,s*1.0,1.13,z+.16)
    for z in (-2.7,2.7):
        m.box(0,.84,z,7.2,.26,.37,BLACK)
        for s in (-1,1):
            rod(m,(s*3.45,.21,z),(s*3.45,1.12,z),.095,CHROME,16)
            m.box(s*3.45,.03,z,.7,.13,.7,BLACK)
            for x in np.arange(.8,3.3,.45):m.box(s*x,1.102,z,.12,.035,.36,WHITE)
    rod(m,(0,1.40,-.15),(0,1.68,-.15),1.0,STEEL,32)
    m.box(.30,1.62,-.6,1.65,.93,2.90,BLUE)
    cabin(m,-.83,1.69,1.05,1.2,2.12,1.58,WHITE)
    loft(m,[(-2.5,.9,1.64,2.18),(-2.1,1.1,1.63,2.3),(-1.2,1.1,1.63,2.3)],WHITE)
    # Four overlapping octagonal boom tubes and extension pads.
    a=np.array((.27,2.02,-.8));direction=np.array((0,.88,.475));direction/=np.linalg.norm(direction)
    for i,length in enumerate((5.8,5.4,4.8,4.0)):
        start=a+direction*(i*3.5);end=start+direction*length
        rod(m,start,end,.43-i*.067,WHITE,8)
        rod(m,end-direction*.12,end,.48-i*.067,BLUE,8)
    tip=a+direction*14.5
    ram(m,(.27,1.93,.5),a+direction*5.3,.18)
    rod(m,tip+(.12,0,0),tip+(.12,-8,0),.022,BLACK,8)
    rod(m,tip-(.12,0,0),tip+(-.12,-8,0),.022,BLACK,8)
    rod(m,tip+(-.24,-8,0),tip+(.24,-8,0),.23,BLUE,16)
    hose(m,[tip+(0,-8.1,0),tip+(0,-8.5,0),tip+(0,-8.64,.16),tip+(0,-8.45,.3)],.06)
    for s in (-1,1):
        for y in (.55,.90,1.25):m.box(s*1.4,y,.65,.26,.06,.64,STEEL)
    return m

def main():
    WORK.mkdir(parents=True,exist_ok=True);(WORK/'materials').mkdir(exist_ok=True)
    colors=[(174,170,161),(149,138,112),(192,198,201),(112,81,47),(53,72,80),(32,36,39),(148,148,142),(211,215,209),(39,65,85),(141,52,39),(224,170,30),(26,30,31),(67,88,55),(127,138,145),(190,178,148),(101,82,57)]
    rng=np.random.default_rng(20260912)
    for i in range(32):
        color=colors[i%16];n=rng.normal(0,1.6,(256,256,1));a=np.clip(np.array(color)+n,0,255).astype('uint8')
        if i==GLASS:
            for y in range(256):a[y]=np.clip(np.array(color)*(1.30-y/420)+n[y],0,255)
        img=Image.fromarray(a)
        img.save(WORK/'materials'/f'{i:02d}.png')
    records=[]
    for m in (excavator(),roller(),paver(),crane()):
        faces=[]
        for p,uv,n in m.faces:
            u,v=uv.mean(0);col=min(7,int(u*8));row=min(3,int((1-v)*4));material=row*8+col
            local=np.column_stack(((uv[:,0]*1024-col*128-4)/120,((1-uv[:,1])*512-row*128-4)/120))
            faces.append({'p':p.tolist(),'uv':local.tolist(),'mat':material})
        item=dict(stem=m.stem,title=m.title,asset_dir='construction',kind='construction',source_triangles=len(faces),faces=faces)
        (WORK/f'{m.stem}.source.json').write_text(json.dumps(item,separators=(',',':')),encoding='utf-8')
        records.append({k:v for k,v in item.items() if k!='faces'})
    (WORK/'source_manifest.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
    print([(r['stem'],r['source_triangles']) for r in records])
if __name__=='__main__':main()
