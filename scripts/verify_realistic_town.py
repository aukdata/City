"""Validate final shipped OBJ/MTL/PNG rather than only authoring data."""
from pathlib import Path
import json
import sys
import numpy as np
from PIL import Image, ImageDraw

ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'artifacts/realistic_town'
if '--work' in sys.argv:
    WORK=ROOT/'artifacts'/sys.argv[sys.argv.index('--work')+1]

def main():
    items=json.loads((WORK/'source_manifest.json').read_text())
    results=[]
    for item in items:
        folder=ROOT/'App/assets'/item['asset_dir'] if 'asset_dir' in item else ROOT/'App/assets/buildings'/item['folder']
        path=folder/f"{item['stem']}.obj"
        points=[];uv=[];normals=[];faces=[];materials=set()
        for line in path.read_text(encoding='utf-8').splitlines():
            values=line.split()
            if not values:continue
            if values[0]=='v':points.append([float(v) for v in values[1:]])
            elif values[0]=='vt':uv.append([float(v) for v in values[1:]])
            elif values[0]=='vn':normals.append([float(v) for v in values[1:]])
            elif values[0]=='usemtl':materials.add(values[1])
            elif values[0]=='f':faces.append([[int(n) for n in v.split('/')] for v in values[1:]])
        points=np.array(points);uv=np.array(uv);normals=np.array(normals);faces=np.array(faces)
        assert faces.shape[1:]==(3,3),path
        assert np.isfinite(points).all() and np.isfinite(uv).all() and np.isfinite(normals).all()
        for channel,count in enumerate((len(points),len(uv),len(normals))):
            assert faces[:,:,channel].min()>=1 and faces[:,:,channel].max()<=count,path
        assert len(materials)==1,path
        assert uv.min()>=-.001 and uv.max()<=1.001,path
        assert abs(points[:,1].min())<.001,path
        dimensions=np.ptp(points,axis=0)
        limit=32 if item.get('kind') in ('train','station') else 12
        assert dimensions[0]<=limit and dimensions[2]<=limit and dimensions[1]<=48,path
        triangles=points[faces[:,:,0]-1]
        areas=np.linalg.norm(np.cross(triangles[:,1]-triangles[:,0],triangles[:,2]-triangles[:,0]),axis=1)/2
        degenerate=int((areas<1e-12).sum())
        assert degenerate==0,(path,degenerate)
        maps=[line.split(maxsplit=1)[1] for line in (folder/f"{item['stem']}.mtl").read_text().splitlines() if line.startswith('map_Kd ')]
        assert len(maps)==1 and maps[0]==f"{item['stem']}_diffuse.png",path
        with Image.open(folder/maps[0]) as image:
            assert image.width==image.height and image.width in (1024,2048)
            resolution=image.width
        result=dict(stem=item['stem'],title=item['title'],folder=item['folder'],triangles=len(faces),vertices=len(points),
                    materials=1,dimensions_xyz=dimensions.tolist(),texture_resolution=resolution,
                    obj=str(path.relative_to(ROOT)).replace('\\','/'),degenerate_triangles=degenerate)
        results.append(result)
    (WORK/'export_validation.json').write_text(json.dumps(results,indent=2)+'\n',encoding='utf-8')
    target=ROOT/'App/assets/civic_transport_manifest.json' if '--work' in sys.argv else ROOT/'App/assets/buildings/realistic_manifest.json'
    target.write_text(json.dumps(results,indent=2)+'\n',encoding='utf-8')
    # Front/back sheets are built from the final Blender renders, labelled clearly.
    columns=5 if '--work' in sys.argv else 6
    width=2400//columns; height=width*9//10; row=height+50
    for side in ('front','back'):
        sheet=Image.new('RGB',(2400,((len(items)+columns-1)//columns)*row),(229,232,228));draw=ImageDraw.Draw(sheet)
        for index,item in enumerate(items):
            x,y=index%columns*width,index//columns*row
            with Image.open(WORK/f"{item['stem']}_{side}.png") as image:
                sheet.paste(image.resize((width,height)),(x,y))
            draw.text((x+10,y+height+6),f"{item['stem']} | {item['title']}",fill=(28,41,35))
            draw.text((x+10,y+height+24),f"{results[index]['triangles']:,} triangles / {side}",fill=(66,78,68))
        sheet.save(WORK/f'contact_{side}.jpg',quality=94)
    print(f'PASS: {len(results)} OBJ/MTL/PNG sets; {sum(r["triangles"] for r in results):,} triangles; zero degenerate faces.')

if __name__=='__main__':
    main()
