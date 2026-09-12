from pathlib import Path
import sys,json,math,numpy as np
source=Path('artifacts/road_integrity/audit_save.py').read_text(encoding='utf-8-sig').split('mixed=[]')[0]
namespace={'__file__':str(Path('artifacts/road_integrity/audit_save.py').resolve())}
exec(source,namespace)
nodes,edges,curve,height=(namespace[k] for k in ('nodes','edges','curve','height'))
wet=[];decks=[]
for edge in edges.values():
 count=max(2,math.ceil(edge['length']/4))
 points=curve(edge,np.linspace(0,1,count+1)[:,None])
 for point in points:
  surface=point[1] if edge['elev'] else height(point[0],point[2])
  if surface<.1:
   wet.append({'edge':edge['id'],'point':point.tolist(),'surface':surface,'elevated':edge['elev']});break
 if edge['elev']:
  middle=points[len(points)//2];decks.append(float(middle[1]-height(middle[0],middle[2])))
report={'roads':len(edges),'submerged':len(wet),'elevated':len(decks),'medianGroundClearance':float(np.median(decks)) if decks else 0,'violations':wet}
Path(sys.argv[2]).mkdir(parents=True,exist_ok=True)
Path(sys.argv[2],'water.json').write_text(json.dumps(report,indent=2))
print({k:v for k,v in report.items() if k!='violations'})
