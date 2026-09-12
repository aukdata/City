from pathlib import Path
import json,math,numpy as np
base=Path('artifacts/road_integrity/acceptance42');data=json.loads((base/'network.json').read_text(encoding='utf-8'));report=json.loads((base/'audit.json').read_text(encoding='utf-8'))
def geometry(eid):
 e=data['edges'][str(eid)];a=np.array(data['nodes'][str(e['a'])]['p']);b=np.array(e['ca']);c=np.array(e['cb']);d=np.array(data['nodes'][str(e['b'])]['p'])
 def evaluate(t):return a*(1-t)**3+b*3*t*(1-t)**2+c*3*t*t*(1-t)+d*t**3
 ts=np.linspace(0,1,51);samples=np.array([evaluate(t) for t in ts]);arc=np.insert(np.cumsum(np.linalg.norm(np.diff(samples,axis=0),axis=1)),0,0)
 quads=[];roadbed=[p for p in e['parts'] if p['kind']==0]
 for part in roadbed:
  start=max(0,e['cutA']-.1);end=min(arc[-1],arc[-1]-e['cutB']+.1);count=max(3,min(100,int((end-start)/2)+1));previous=None
  for distance in np.linspace(start,end,count+1):
   t=float(np.interp(distance,arc,ts));p=evaluate(t);tangent=3*(b-a)*(1-t)**2+6*(c-b)*t*(1-t)+3*(d-c)*t*t;right=np.array([tangent[2],-tangent[0]]);right/=np.linalg.norm(right);f=distance/arc[-1];r=part['ranges'];left=r[0]*(1-f)+r[2]*f;rightOffset=r[1]*(1-f)+r[3]*f;section=[p[[0,2]]+right*left,p[[0,2]]+right*rightOffset]
   if previous is not None:quads.append([previous[0],previous[1],section[1],section[0]])
   previous=section
 return quads
def cross(a,b):return float(a[0]*b[1]-a[1]*b[0])
def area(poly):return abs(sum(cross(poly[i],poly[(i+1)%len(poly)]) for i in range(len(poly))))*.5 if len(poly)>2 else 0
def intersection(a,b):
 a=np.array(a);b=np.array(b)
 if np.any(a.max(axis=0)<b.min(axis=0)) or np.any(b.max(axis=0)<a.min(axis=0)):return 0
 origin=a[0].copy();a=list(a-origin);b=b-origin;sign=1 if sum(cross(b[i],b[(i+1)%len(b)]) for i in range(len(b)))>=0 else -1
 for i in range(len(b)):
  edge=b[(i+1)%len(b)]-b[i];out=[]
  if not a:break
  for j,p in enumerate(a):
   q=a[(j+1)%len(a)];dp=cross(edge,p-b[i])*sign;dq=cross(edge,q-b[i])*sign
   if dp>=0:out.append(p)
   if (dp>=0)!=(dq>=0):out.append(p+(q-p)*dp/(dp-dq))
  a=out
 return area(a)
result=[]
for item in report['overlaps']:
 a,b=map(geometry,item['edges']);overlap=sum(intersection(p,q) for p in a for q in b);result.append({'edges':item['edges'],'asphaltOverlapAreaM2':round(overlap,5),'broadRunM':item['length']})
(base/'strict_overlap.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print(json.dumps(result))
