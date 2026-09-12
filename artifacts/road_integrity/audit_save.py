from pathlib import Path
import struct,json,math,collections,sys
import numpy as np
ROOT=Path(__file__).resolve().parents[2];SAVE=Path(sys.argv[1]) if len(sys.argv)>1 else ROOT/'App/saves/default';OUT=Path(sys.argv[2]) if len(sys.argv)>2 else ROOT/'artifacts/road_integrity';OUT.mkdir(parents=True,exist_ok=True)
class Reader:
 def __init__(self,p):self.data=p.read_bytes();self.pos=0
 def read(self,f):
  a=struct.unpack_from('<'+f,self.data,self.pos);self.pos+=struct.calcsize('<'+f);return a[0] if len(a)==1 else a
 def skip(self,n):self.pos+=n
 def string(self):
  n=self.read('H');t=self.data[self.pos:self.pos+n].decode();self.pos+=n;return t
r=Reader(SAVE/'global/roads.bin');magic,version,cx,cz,nc,ec,pc,lc=r.read('IHiiIIII')
nodes={};edges={}
for _ in range(nc):
 ident,x,y,z,typ,ac=r.read('ifffBI');ats=[r.read('ifBB') for i in range(ac)]
 if r.read('B'):
  r.string();r.skip(4)
  for i in range(r.read('I')):r.skip(4);r.skip(r.read('I')*4)
 nodes[ident]={'p':[x,y,z],'type':typ,'atts':ats}
for _ in range(ec):
 ident,a,b=r.read('iii');ca=list(r.read('fff'));cb=list(r.read('fff'));typ,speed,length,plan,cutA,cutB,state=r.read('BffiffB');start,borderA,borderB,nlanes=r.read('diiI');lanes=[r.read('fffffBBBBBBB') for i in range(nlanes)];elev=r.read('B');parts=[]
 for i in range(r.read('I')):
  name=r.string();ranges=r.read('ffff');build,kind=r.read('BB')
  if version>=14:r.skip(2)
  if version>=15:r.skip(8)
  if version>=16:r.skip(2)
  parts.append({'name':name,'ranges':ranges,'build':build,'kind':kind})
 r.skip(r.read('I')*26)
 edges[ident]={'id':ident,'a':a,'b':b,'ca':ca,'cb':cb,'type':typ,'length':length,'cutA':cutA,'cutB':cutB,'elev':bool(elev),'parts':parts,'lanes':nlanes,'state':state}
r.skip(r.read('I')*29);routes=[]
for _ in range(r.read('I')):
 ident,kind=r.read('iB');name=r.string();number=r.read('i');eids=[r.read('i') for i in range(r.read('I'))];r.skip(12);routes.append({'id':ident,'kind':kind,'name':name,'number':number,'edges':eids})
terrain={}
def height(x,z):
 cx=min(63,max(0,int(x//1024)));cz=min(63,max(0,int(z//1024)));key=(cx,cz)
 if key not in terrain:terrain[key]=np.frombuffer((SAVE/f'chunks/{cx}_{cz}/terrain.bin').read_bytes(),dtype='<f4',offset=4).reshape(65,65)
 g=terrain[key];fx=min(64,max(0,(x-cx*1024)/16));fz=min(64,max(0,(z-cz*1024)/16));col=min(63,int(fx));row=min(63,int(fz));u=fx-col;v=fz-row
 return float(g[row,col]*(1-u-v)+g[row,col+1]*u+g[row+1,col]*v) if u+v<=1 else float(g[row+1,col+1]*(u+v-1)+g[row,col+1]*(1-v)+g[row+1,col]*(1-u))
def curve(e,t):
 a=np.array(nodes[e['a']]['p']);b=np.array(nodes[e['b']]['p']);return a*(1-t)**3+np.array(e['ca'])*3*(1-t)**2*t+np.array(e['cb'])*3*(1-t)*t*t+b*t**3
mixed=[]
for ident,n in nodes.items():
 es=[edges[a[0]] for a in n['atts'] if a[0] in edges];elev=sum(e['elev'] for e in es)
 if elev and elev<len(es):
  x,y,z=n['p'];g=height(x,z);mixed.append({'node':ident,'position':n['p'],'ground':g,'difference':y-g,'elev':elev,'edges':[e['id'] for e in es]})
selected=[r for r in routes if r['number'] in [151,208]]
selectedNodes={e[k] for r in selected for i in r['edges'] if (e:=edges.get(i)) for k in ['a','b']}
# Restrict pair diagnosis to route neighbourhoods visible in the user screenshots.
selectedEdges={e['id'] for e in edges.values() if e['a'] in selectedNodes or e['b'] in selectedNodes}
selectedEdges=set(edges)
segments=[];buckets=collections.defaultdict(list)
for e in edges.values():
 steps=max(1,int(math.ceil(e['length']/12)));ts=np.linspace(0,1,steps+1);p=curve(e,ts[:,None])
 width=max((abs(q) for part in e['parts'] for q in part['ranges']),default=3)
 for i in range(steps):
  a=p[i,[0,2]];b=p[i+1,[0,2]];d=b-a;l=np.linalg.norm(d)
  if l<.01:continue
  index=len(segments);segments.append((e['id'],a,b,d/l,l,width,(i+.5)/steps,p[i,1]))
  for x in range(int(min(a[0],b[0])-width)//32,int(max(a[0],b[0])+width)//32+1):
   for z in range(int(min(a[1],b[1])-width)//32,int(max(a[1],b[1])+width)//32+1):buckets[x,z].append(index)
seen=set();overlaps=collections.defaultdict(float);detail={}
for bucket in buckets.values():
 for ii,i in enumerate(bucket):
  a=segments[i]
  for j in bucket[ii+1:]:
   b=segments[j]
   if a[0]==b[0] or (a[0] not in selectedEdges and b[0] not in selectedEdges):continue
   pair=(min(i,j),max(i,j))
   if pair in seen:continue
   seen.add(pair)
   if abs(float(np.dot(a[3],b[3])))<.965 or abs(a[7]-b[7])>1.0:continue
   # Measure overlapped longitudinal run inside structural widths, away from cut mouths.
   ca=edges[a[0]];cb=edges[b[0]]
   if a[6]*ca['length']<ca['cutA'] or (1-a[6])*ca['length']<ca['cutB'] or b[6]*cb['length']<cb['cutA'] or (1-b[6])*cb['length']<cb['cutB']:continue
   p0=float(np.dot(b[1]-a[1],a[3]));p1=float(np.dot(b[2]-a[1],a[3]));along=min(a[4],max(p0,p1))-max(0,min(p0,p1))
   distance=abs(float(np.cross(a[3],(b[1]+b[2])*.5-a[1])))
   if along>0 and distance<min(a[5]+b[5]-1.0,8):
    key=tuple(sorted([a[0],b[0]]));overlaps[key]+=along;detail[key]=[distance,((a[1]+a[2])*.5).tolist()]
report={'version':version,'nodes':nc,'edges':ec,'mixed_nodes':len(mixed),'mixed_below_ground':[x for x in mixed if x['difference']<-.05],'routes':selected,'route_mixed':[x for x in mixed if x['node'] in selectedNodes],'overlaps':[{'edges':k,'length':v,'distance':detail[k][0],'position':detail[k][1]} for k,v in sorted(overlaps.items(),key=lambda p:-p[1]) if v>8]}
(OUT/'audit.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
(OUT/'network.json').write_text(json.dumps({'nodes':nodes,'edges':edges,'routes':routes},ensure_ascii=False),encoding='utf-8')
print(json.dumps({k:report[k] for k in ['version','nodes','edges','mixed_nodes']},ensure_ascii=False));print('mixed_below',len(report['mixed_below_ground']));print('route mixed',json.dumps(report['route_mixed'][:12]));print('overlaps',json.dumps(report['overlaps'][:15]))
