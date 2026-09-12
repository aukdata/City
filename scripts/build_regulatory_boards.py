"""Rebuild physical sign plates (metres); artwork follows reference/ks084812_INDEX.md."""
from pathlib import Path
import math

OUTPUT = Path(__file__).resolve().parents[1] / 'App/assets/signs/regulatory'

def plate(name, outline):
    lines = ['# 20 mm aluminium sign plate; local front is -Z', 'o sign_board']
    count = len(outline)
    for z in [-.01, .01]:
        lines.extend(f'v {x:.6f} {y:.6f} {z:.6f}' for x, y in outline)
    width=max(x for x,y in outline)-min(x for x,y in outline)
    height=max(y for x,y in outline)-min(y for x,y in outline)
    lines.extend(f'vt {x/width+.5:.6f} {y/height+.5:.6f}' for x,y in outline)
    # Outline is counterclockwise in XY. Reverse for front normal -Z.
    for i in range(1,count-1):
        lines.append(f'f 1/1 {i+2}/{i+2} {i+1}/{i+1}')
        lines.append(f'f {count+1}/1 {count+i+1}/{i+1} {count+i+2}/{i+2}')
    for i in range(count):
        j=(i+1)%count
        lines.append(f'f {i+1}/{i+1} {j+1}/{j+1} {count+j+1}/{j+1} {count+i+1}/{i+1}')
    (OUTPUT/name).write_text('\n'.join(lines)+'\n',encoding='utf-8')

if __name__=='__main__':
    OUTPUT.mkdir(parents=True,exist_ok=True)
    plate('circle.obj',[(.3*math.cos(i*math.tau/48),.3*math.sin(i*math.tau/48)) for i in range(48)])
    plate('inverted_triangle.obj',[(-.4,.3464),(0,-.3464),(.4,.3464)])
