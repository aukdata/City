"""Original metre-scale roadside furniture. Retains the signal's live lamp anchors.

The OBJ files use Y up, with outward face winding and explicit smooth tube normals.
SignalRegistry mirrors X on import; guide/regulatory poles are imported directly.
No new texture is necessary: the existing signal atlas supplies metal and dark rubber.
"""
from pathlib import Path
from math import sin, cos, pi, sqrt
import json

ROOT = Path(__file__).resolve().parents[1]
METAL = (.5, .5)
DARK = (51/1024, 1-51/1024)

def add(a,b): return tuple(x+y for x,y in zip(a,b))
def sub(a,b): return tuple(x-y for x,y in zip(a,b))
def mul(a,s): return tuple(x*s for x in a)
def dot(a,b): return sum(x*y for x,y in zip(a,b))
def cross(a,b): return (a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0])
def unit(a): return mul(a,1/sqrt(dot(a,a)))

class Model:
    def __init__(self): self.parts={}; self.part='body'
    def triangle(self, points, normals=None, uv=METAL, uvs=None):
        face=unit(cross(sub(points[1],points[0]),sub(points[2],points[0])))
        if normals and dot(face,add(add(normals[0],normals[1]),normals[2]))<0:
            points=[points[0],points[2],points[1]]
            normals=[normals[0],normals[2],normals[1]]
            if uvs: uvs=[uvs[0],uvs[2],uvs[1]]
        self.parts.setdefault(self.part,[]).append([(p,(normals or [face]*3)[i],(uvs or [uv]*3)[i]) for i,p in enumerate(points)])
    def quad(self, p, normal=None, uv=METAL):
        self.triangle(p[:3],[normal]*3 if normal else None,uv)
        self.triangle([p[0],p[2],p[3]],[normal]*3 if normal else None,uv)
    def box(self, center, size, uv=METAL):
        points=[add(center,(x*size[0]/2,y*size[1]/2,z*size[2]/2)) for x,y,z in
                [(-1,-1,-1),(1,-1,-1),(1,1,-1),(-1,1,-1),(-1,-1,1),(1,-1,1),(1,1,1),(-1,1,1)]]
        for face,n in [((0,3,2,1),(0,0,-1)),((4,5,6,7),(0,0,1)),((0,4,7,3),(-1,0,0)),((1,2,6,5),(1,0,0)),((0,1,5,4),(0,-1,0)),((3,7,6,2),(0,1,0))]:
            self.quad([points[i] for i in face],n,uv)
    def tube(self, a,b,r, tip=None, sides=32, uv=METAL):
        axis=unit(sub(b,a)); tip=r if tip is None else tip
        tangent=unit(cross(axis,(0,1,0) if abs(axis[1])<.9 else (1,0,0)))
        bitangent=cross(axis,tangent)
        length=sqrt(dot(sub(b,a),sub(b,a)))
        radial=[add(mul(tangent,cos(i*2*pi/sides)),mul(bitangent,sin(i*2*pi/sides))) for i in range(sides)]
        for i in range(sides):
            j=(i+1)%sides
            a0,a1=add(a,mul(radial[i],r)),add(a,mul(radial[j],r))
            b0,b1=add(b,mul(radial[i],tip)),add(b,mul(radial[j],tip))
            n0=unit(add(radial[i],mul(axis,(r-tip)/length)))
            n1=unit(add(radial[j],mul(axis,(r-tip)/length)))
            self.triangle([a0,a1,b0],[n0,n1,n0],uv)
            self.triangle([a1,b1,b0],[n1,n1,n0],uv)
            self.triangle([a,a1,a0],[mul(axis,-1)]*3,uv)
            self.triangle([b,b0,b1],[axis]*3,uv)
    def elbow(self, points,r,uv=METAL):
        # Continuous rings with averaged tangents; no polygonal overlap at tube bends.
        rings=[]; normals=[]
        for i,p in enumerate(points):
            direction=unit(sub(points[min(i+1,len(points)-1)],points[max(0,i-1)]))
            side=(0,0,1); vertical=unit(cross(side,direction))
            ns=[add(mul(side,cos(j*2*pi/32)),mul(vertical,sin(j*2*pi/32))) for j in range(32)]
            rings.append([add(p,mul(n,r)) for n in ns]); normals.append(ns)
        for i in range(len(points)-1):
            for j in range(32):
                k=(j+1)%32
                self.triangle([rings[i][j],rings[i+1][j],rings[i][k]],[normals[i][j],normals[i+1][j],normals[i][k]],uv)
                self.triangle([rings[i][k],rings[i+1][j],rings[i+1][k]],[normals[i][k],normals[i+1][j],normals[i+1][k]],uv)
    def rounded_housing(self,x,y,width,height,depth,uv=METAL):
        radius=min(.055,width*.25,height*.25); outline=[]
        for cx,cy,start in [(width/2-radius,height/2-radius,0),(-width/2+radius,height/2-radius,pi/2),(-width/2+radius,-height/2+radius,pi),(width/2-radius,-height/2+radius,3*pi/2)]:
            for i in range(9):
                t=start+i*pi/16
                outline.append((cx+radius*cos(t),cy+radius*sin(t)))
        rings=[]
        for z,scale in [(-depth,.94),(-depth+.009,1),(-.018,1),(-.007,.97)]:
            rings.append([(x+a*scale,y+b*scale,z) for a,b in outline])
        for k in range(len(rings)-1):
            for i in range(len(outline)):
                j=(i+1)%len(outline)
                self.quad([rings[k][i],rings[k][j],rings[k+1][j],rings[k+1][i]])
        for i in range(len(outline)):
            j=(i+1)%len(outline)
            self.triangle([(x,y,-depth),rings[0][j],rings[0][i]],[(0,0,-1)]*3,uv)
            self.triangle([(x,y,-.007),rings[-1][i],rings[-1][j]],[(0,0,1)]*3,uv)
    def visor(self,x,y):
        # Open-bottom sun hood with an actual inner and outer wall, 3 mm sheet thickness.
        for i in range(32):
            angles=[-.12*pi+i*1.24*pi/32,-.12*pi+(i+1)*1.24*pi/32]
            for r,sign in [(.172,1),(.169,-1)]:
                points=[(x+r*cos(a),y+r*sin(a),z) for a,z in [(angles[0],.008),(angles[1],.008),(angles[1],.235),(angles[0],.235)]]
                n=(cos(sum(angles)/2)*sign,sin(sum(angles)/2)*sign,0)
                self.quad(points,n,DARK)
            self.quad([(x+r*cos(a),y+r*sin(a),.235) for a,r in [(angles[0],.169),(angles[1],.169),(angles[1],.172),(angles[0],.172)]],(0,0,1),DARK)
    def lamp(self,x,y):
        for i in range(64):
            a,b=i*2*pi/64,(i+1)*2*pi/64
            self.triangle([(x,y,.015),(x+.15*cos(a),y+.15*sin(a),.015),(x+.15*cos(b),y+.15*sin(b),.015)],[(0,0,1)]*3,
                          uvs=[(.5,.5),(.5+.5*cos(a),.5+.5*sin(a)),(.5+.5*cos(b),.5+.5*sin(b))])
    def write(self, relative):
        lines=['# Original Pavecity street furniture; metres; Y up.']; count=0
        report={}
        for part,triangles in self.parts.items():
            lines.append('o '+part)
            lookup={}; faces=[]
            for triangle in triangles:
                indices=[]
                for vertex in triangle:
                    key=tuple(round(v,7) for group in vertex for v in group)
                    if key not in lookup:
                        count+=1; lookup[key]=count
                        p,n,uv=vertex
                        lines+=['v '+' '.join(f'{v:.7f}' for v in p),'vt '+' '.join(f'{v:.7f}' for v in uv),'vn '+' '.join(f'{v:.7f}' for v in n)]
                    indices.append(lookup[key])
                faces.append('f '+' '.join(f'{i}/{i}/{i}' for i in indices))
            lines+=faces
            report[part]={'triangles':len(triangles),'vertices':len(lookup)}
        (ROOT/relative).write_text('\n'.join(lines)+'\n',encoding='utf-8')
        return report

def flange(m,height,radius,bolts=8):
    m.tube((0,height,0),(0,height+.028,0),radius,sides=32)
    for i in range(bolts):
        angle=2*pi*(i+.5)/bolts; x,z=radius*.77*cos(angle),radius*.77*sin(angle)
        m.tube((x,height+.028,z),(x,height+.043,z),.014,sides=6)
        m.tube((x,height+.043,z),(x,height+.053,z),.007,sides=12)

def build_signal():
    m=Model()
    flange(m,.008,.195)
    m.tube((0,.036,0),(0,3.2,0),.083,.067)
    m.tube((0,3.2,0),(0,6.42,0),.067,.052)
    m.tube((0,3.17,0),(0,3.24,0),.079)
    m.rounded_housing(0,.72,.11,.33,.084) # Flush maintenance cover.
    for y in [.60,.84]: m.tube((0,y,-.091),(0,y,-.086),.006,sides=8)
    points=[(0,6.30,-.0)]+[(.40-.40*cos(i*pi/32),6.42+.40*sin(i*pi/32),0) for i in range(17)]+[(2.92,6.82,0)]
    m.elbow(points,.052)
    m.tube((2.92,6.82,0),(2.925,6.82,0),.053)
    for x in [1.91,2.83]:
        m.tube((x-.032,6.82,0),(x+.032,6.82,0),.062)
        m.box((x,6.67,-.085),(.045,.31,.03))
        m.tube((x,6.52,-.17),(x,6.52,-.095),.018,sides=12)
    m.rounded_housing(2.37131,6.4,1.245,.39,.135)
    for x in [1.84,2.91]:
        for y in [6.27,6.53]:
            m.tube((x,y,-.145),(x,y,-.135),.012,sides=8)
    for i in range(3):
        x=1.97131+i*.4
        m.visor(x,6.4)
        # Weather seal and thin outer lens ring; never cover the live lens disc.
        for j in range(64):
            a,b=j*2*pi/64,(j+1)*2*pi/64
            m.quad([(x+r*cos(t),6.4+r*sin(t),.01) for t,r in [(a,.15),(b,.15),(b,.162),(a,.162)]],(0,0,1),DARK)
        m.part=f'lamp_{i}'; m.lamp(x,6.4); m.part='body'
    m.part='sub_body'; m.rounded_housing(1.97131,6,.39,.39,.135); m.visor(1.97131,6)
    m.box((1.97131,6.21,-.075),(.04,.14,.04))
    for x in [1.84,2.10]: m.tube((x,6.12,-.146),(x,6.12,-.135),.01,sides=8)
    m.part='sub_lamp'; m.lamp(1.97131,6)
    return m.write('App/assets/signals/signal.obj')

def build_guide():
    m=Model(); m.part='pole'
    flange(m,.008,.24)
    m.tube((0,.036,0),(0,3.6,0),.13,.105)
    m.tube((0,3.6,0),(0,6.50,0),.105,.084)
    flange(m,3.58,.16)
    m.tube((0,6.50,0),(0,6.512,0),.086)
    for y in [5.25,6.25]:
        m.tube((0,y-.06,0),(0,y+.06,0),.126)
        m.tube((0,y,0),(3,y,0),.063,.048)
        m.tube((3,y,0),(3.012,y,0),.049)
        for x in [1.28,2.75]:
            m.tube((x-.025,y,0),(x+.025,y,0),.073)
            m.box((x,y,.09),(.10,.10,.12))
            for by in [-.035,.035]: m.tube((x,y+by,.147),(x,y+by,.16),.008,sides=6)
    for x in [1.28,2.75]: m.box((x,5.75,.10),(.04,1.10,.035))
    m.tube((.08,5.6,0),(.72,6.25,0),.035)
    m.rounded_housing(0,.7,.15,.35,.128)
    return m.write('App/assets/signs/guide/guide_pole.obj')

def build_regulatory():
    m=Model(); m.part='SignPole'
    # Embedded post: small ground collar, capped tube and stainless board straps.
    m.tube((0,0,0),(0,.018,0),.048)
    m.tube((0,.018,0),(0,2.492,0),.03)
    m.tube((0,2.492,0),(0,2.5,0),.031,.028)
    for y in [1.93,2.27]:
        m.tube((0,y-.016,0),(0,y+.016,0),.034)
        m.box((0,y,.036),(.046,.036,.023))
        for x in [-.013,.013]: m.tube((x,y,.046),(x,y,.051),.005,sides=6)
    return m.write('App/assets/signs/sign_pole.obj')

if __name__=='__main__':
    report={'signal':build_signal(),'guide':build_guide(),'regulatory':build_regulatory()}
    path=ROOT/'artifacts/roadside_life/furniture_after.json'
    path.write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2))
