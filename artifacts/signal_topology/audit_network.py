"""Read-only topology and signal-control audit of a saved City network."""
from pathlib import Path
import struct,json,math,collections,sys
import numpy as np
ROOT=Path.cwd();SAVE=Path(sys.argv[1]) if len(sys.argv)>1 else ROOT/'App/saves/default';OUT=Path(sys.argv[2]) if len(sys.argv)>2 else ROOT/'artifacts/road_integrity';OUT.mkdir(parents=True,exist_ok=True)
class Reader:
 def __init__(self,p):self.data=p.read_bytes();self.pos=0
 def read(self,f):
  a=struct.unpack_from('<'+f,self.data,self.pos);self.pos+=struct.calcsize('<'+f);return a[0] if len(a)==1 else a
 def skip(self,n):self.pos+=n
 def string(self):
  n=self.read('H');t=self.data[self.pos:self.pos+n].decode();self.pos+=n;return t
r=Reader(SAVE/'global/roads.bin');magic,version,cx,cz,nc,ec,pc,lc=r.read('IHiiIIII')
assert magic==0x4e4452 and 13<=version<=20, (magic,version)
nodes={};edges={}
for _ in range(nc):
 ident,x,y,z,typ,ac=r.read('idddBI' if version>=19 else 'ifffBI');ats=[r.read('ifBB') for i in range(ac)]
 signal=r.read('B');phases=[]
 if signal:
  r.string();r.skip(4)
  for i in range(r.read('I')):
   duration=r.read('f');ids=[r.read('i') for j in range(r.read('I'))];phases.append({'duration':duration,'ids':ids})
 nodes[ident]={'p':[x,y,z],'type':typ,'atts':ats,'signal':bool(signal),'phases':phases}
for _ in range(ec):
 ident,a,b=r.read('iii');ca=list(r.read('ddd' if version>=19 else 'fff'));cb=list(r.read('ddd' if version>=19 else 'fff'));typ,speed,length,plan,cutA,cutB,state=r.read('BffiffB');start,borderA,borderB,nlanes=r.read('diiI');lanes=[r.read('fffffBBBBBBB') for i in range(nlanes)];elev=r.read('B');parts=[]
 for i in range(r.read('I')):
  name=r.string();ranges=r.read('ffff');build,kind=r.read('BB')
  if version>=14:r.skip(2)
  if version>=15:r.skip(8)
  if version>=16:r.skip(2)
  parts.append({'name':name,'ranges':ranges,'build':build,'kind':kind})
 r.skip(r.read('I')*26)
 edges[ident]={'id':ident,'a':a,'b':b,'ca':ca,'cb':cb,'type':typ,'length':length,'cutA':cutA,'cutB':cutB,'flags':elev,'elev':bool(elev&1),'parts':parts,'laneData':lanes,'laneTypes':[l[7] for l in lanes],'lanes':nlanes,'state':state}

print('parsed',nc,'nodes',ec,'edges; remaining appended network metadata bytes',len(r.data)-r.pos)
adj=collections.defaultdict(set)
for e in edges.values():
 adj[e['a']].add(e['b']);adj[e['b']].add(e['a'])
seen=set(); comps=[]
for n in adj:
 if n in seen:continue
 q=[n];seen.add(n)
 for v in q:
  for x in adj[v]:
   if x not in seen:seen.add(x);q.append(x)
 comps.append(q)
comps.sort(key=len,reverse=True)
for c in comps[:20]:
 ns=set(c);es=[e for e in edges.values() if e['a'] in ns]
 print('component',len(c),'edges',len(es),'types',dict(collections.Counter(e['type'] for e in es)),'farm',sum(bool(e['flags']&8) for e in es),'center',[sum(nodes[n]['p'][i] for n in c)/len(c) for i in (0,2)])
print('totals',nc,ec,'components',len(comps),'orphans',len(nodes)-len(adj))
(OUT/'network.json').write_text(json.dumps({'nodes':nodes,'edges':edges}))

print('signals',sum(n['signal'] for n in nodes.values()))
invalid=[(i,n['p'],n['atts']) for i,n in nodes.items() if n['signal'] and any(a[3]!=3 for a in n['atts'])]
print('signal_non3',len(invalid), invalid[:5])
print('railEdges',sum(11 in e['laneTypes'] for e in edges.values()))

violations=[]
for i,n in nodes.items():
 if not n['signal']:continue
 bad=[]
 for a in n['atts']:
  e=edges[a[0]]
  incoming=sum(l[7]!=11 and l[6] in (0,1) and ((l[5]==0 and e['b']==i) or (l[5]==1 and e['a']==i)) for l in e['laneData'])
  if a[3]!=3 and incoming:bad.append({'edge':a[0],'incomingLanes':incoming,'control':a[3]})
 if bad:violations.append({'node':i,'position':n['p'],'degree':len(n['atts']),'bad':bad})
print('incoming_signal_violations',len(violations),'degree3plus',sum(v['degree']>=3 for v in violations),'examples',violations[:3])
(OUT/'signal_violations.json').write_text(json.dumps(violations,indent=2))

summary={'save':str(SAVE.resolve()),'version':version,'nodes':nc,'edges':ec,'signals':sum(n['signal'] for n in nodes.values()),'incomingSignalViolations':len(violations),'junctionViolations':sum(v['degree']>=3 for v in violations),'components':[]}
for c in comps:
 ns=set(c); es=[e for e in edges.values() if e['a'] in ns]
 summary['components'].append({'nodes':len(c),'edges':len(es),'railEdges':sum(11 in e['laneTypes'] for e in es),'farmEdges':sum(bool(e['flags']&8) for e in es)})
(OUT/'summary.json').write_text(json.dumps(summary,indent=2))

phase_counts=[len(n['phases']) for n in nodes.values() if n['signal']]
summary['signalPhaseCountHistogram']=dict(collections.Counter(phase_counts))
summary['maxSignalCycleDuration']=max((sum(p['duration'] for p in n['phases']) for n in nodes.values() if n['signal']),default=0)
(OUT/'summary.json').write_text(json.dumps(summary,indent=2))
