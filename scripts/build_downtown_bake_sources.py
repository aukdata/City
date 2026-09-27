"""Export the downtown atlas buildings as Blender bake sources.

The street buildings (office_005/006, shop_007..010) and the metropolitan
facilities (office towers, city hall, mall, hospital, school) were shipped with
flat atlas colour and no occlusion. This writes their near-level geometry in the
realistic-pack source format so finish_realistic_town_blender.py can give them
photographic materials, weathering and baked occlusion like the other buildings:

    python scripts/build_downtown_bake_sources.py
    blender -b --python scripts/finish_realistic_town_blender.py -- --work downtown_streets
    blender -b --python scripts/finish_realistic_town_blender.py -- --work downtown_facilities
    blender -b --python scripts/build_model_lods.py -- office_005 office_006 ...

Each atlas has its own work folder because sign tiles 16-31 differ.
"""
from pathlib import Path
import json
import numpy as np
from PIL import Image
import generate_detailed_buildings as base
import build_japanese_streets as streets
import build_urban_facilities as facilities

ROOT = Path(__file__).resolve().parents[1]

def street_models():
    for stem, floors, variant in [('office_005',7,0),('office_006',12,1),('shop_007',5,2),('shop_008',7,3)]:
        yield streets.city_building(stem, floors, variant)
    for i in range(2):
        yield streets.shop_row(f'shop_{i+9:03d}', i)

def facility_models():
    for kind, count in [('office_tower',2),('city_hall',1),('shopping_mall',1),('hospital',1),('school',1)]:
        for variant in range(1, count+1):
            yield facilities.build(kind, variant, 0)

def export(work, atlas_name, models):
    work = ROOT/'artifacts'/work
    (work/'materials').mkdir(parents=True, exist_ok=True)
    atlas = Image.open(ROOT/'App/assets/buildings/commercial'/atlas_name).convert('RGB')
    for index in range(32):
        x, y = index % 8 * 128, index // 8 * 128
        atlas.crop((x+4, y+4, x+124, y+124)).save(work/'materials'/f'{index:02d}.png')
    records = []
    for model in models:
        faces = []
        for points, uv, _ in model.faces:
            u, v = uv.mean(0)
            col = min(7, int(u*8)); row = min(3, int((1-v)*4))
            local = np.column_stack(((uv[:,0]*1024-col*128-4)/120, ((1-uv[:,1])*512-row*128-4)/120))
            faces.append({'p': points.tolist(), 'uv': local.tolist(), 'mat': row*8+col})
        record = {'stem': model.stem, 'title': model.title, 'folder': 'commercial', 'source_triangles': len(faces)}
        (work/f'{model.stem}.source.json').write_text(json.dumps(dict(record, faces=faces), separators=(',',':')), encoding='utf-8')
        records.append(record)
    (work/'source_manifest.json').write_text(json.dumps(records, indent=2)+'\n', encoding='utf-8')
    print(f'{work.name}: {len(records)} sources, {sum(r["source_triangles"] for r in records):,} triangles')

def main():
    export('downtown_streets', 'japan_street_atlas.png', street_models())
    export('downtown_facilities', 'town_atlas.png', facility_models())

if __name__ == '__main__':
    main()
