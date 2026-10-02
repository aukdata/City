"""Read saved route ordering without loading/rendering or changing the city."""
from pathlib import Path
import json
import runpy
import sys

if len(sys.argv) != 3:
    raise SystemExit('usage: audit_routes.py SNAPSHOT OUTPUT_DIRECTORY')
root = Path(__file__).resolve().parents[2]
state = runpy.run_path(str(root / 'artifacts/signal_topology/audit_network.py'))
reader, edges, nodes = state['r'], state['edges'], state['nodes']
reader.skip(reader.read('I') * 29)  # global RoadObject records
routes = []
for _ in range(reader.read('I')):
    ident, kind = reader.read('iB')
    name = reader.string()
    number, count = reader.read('iI')
    members = [reader.read('i') for _ in range(count)]
    color = reader.read('fff')
    gaps = []
    for index, (first, second) in enumerate(zip(members, members[1:])):
        a, b = edges.get(first), edges.get(second)
        if not a or not b or not {a['a'], a['b']} & {b['a'], b['b']}:
            gaps.append(dict(index=index, edgeA=first, edgeB=second))
    def traverse(start):
        position, failures = start, []
        for index, ident in enumerate(members):
            edge = edges.get(ident)
            if not edge or position not in (edge['a'], edge['b']):
                failures.append(index)
                position = edge['b'] if edge else -1
            else:
                position = edge['b'] if position == edge['a'] else edge['a']
        return failures
    first = edges.get(members[0]) if members else None
    walks = [traverse(first[end]) for end in ('a', 'b')] if first else [[]]
    routes.append(dict(id=ident, kind=kind, name=name, number=number,
                       edgeCount=count, gaps=gaps,
                       orderedWalkViolations=min(walks, key=len),
                       duplicateEdges=len(members)-len(set(members)),
                       missingEdges=[ident for ident in members if ident not in edges],
                       color=color, edges=members))
output = Path(sys.argv[2])
(output / 'route_continuity.json').write_text(json.dumps(routes, ensure_ascii=False, indent=2))
summary = dict(routes=len(routes), adjacencyGaps=sum(len(r['gaps']) for r in routes),
               orderedWalkViolations=sum(len(r['orderedWalkViolations']) for r in routes),
               duplicateEdges=sum(r['duplicateEdges'] for r in routes),
               missingEdges=sum(len(r['missingEdges']) for r in routes))
(output / 'route_summary.json').write_text(json.dumps(summary, indent=2))
print(json.dumps(summary))
