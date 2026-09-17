"""Generate distant building meshes with Blender; retain source materials and UVs.

Run: blender --background --python scripts/build_city_lod.py
"""
from pathlib import Path
import bpy
import json
import sys
from mathutils.kdtree import KDTree

ROOT = Path(__file__).resolve().parents[1]
BUILDINGS = ROOT / 'App/assets/buildings'
DEST = BUILDINGS / 'lod'
DEST.mkdir(exist_ok=True)
reports = []
sources = sorted((BUILDINGS / 'residential').glob('residential_???.obj'))
for stem in ('shop', 'office', 'factory', 'public', 'parking'):
    sources += sorted((BUILDINGS / 'commercial').glob(stem + '_???.obj'))
for folder, stem in [('residential', 'rural_house'), ('commercial', 'convenience_urban'),
                     ('commercial', 'convenience_roadside'), ('commercial', 'fuel_urban'),
                     ('commercial', 'fuel_roadside')]:
    sources += sorted((BUILDINGS / folder).glob(stem + '_???.obj'))
if '--new-sites' in sys.argv:
    sources = [path for path in sources if path.stem.startswith(('rural_house_', 'convenience_', 'fuel_'))]

for source in sources:
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    bpy.ops.wm.obj_import(filepath=str(source), forward_axis='NEGATIVE_Z', up_axis='Y')
    objects = [o for o in bpy.context.selected_objects if o.type == 'MESH']
    before = sum(len(o.data.polygons) for o in objects)
    for obj in objects:
        bpy.context.view_layer.objects.active = obj
        bpy.ops.object.mode_set(mode='OBJECT')
        original = [v.co.copy() for v in obj.data.vertices]
        tree = KDTree(len(original))
        for index, point in enumerate(original):
            tree.insert(point, index)
        tree.balance()
        low = [min(p[axis] for p in original) for axis in range(3)]
        high = [max(p[axis] for p in original) for axis in range(3)]
        modifier = obj.modifiers.new('Distance reduction', 'DECIMATE')
        modifier.ratio = 0.18
        modifier.use_collapse_triangulate = True
        bpy.ops.object.modifier_apply(modifier=modifier.name)
        # Collapsing thin, disconnected trim can extrapolate below the foundation.
        # Constrain the result to the source surface and preserve the original extents.
        for vertex in obj.data.vertices:
            nearest, _, distance = tree.find(vertex.co)
            if distance > 0.08:
                vertex.co = nearest
        reduced_low = [min(v.co[axis] for v in obj.data.vertices) for axis in range(3)]
        reduced_high = [max(v.co[axis] for v in obj.data.vertices) for axis in range(3)]
        for vertex in obj.data.vertices:
            for axis in range(3):
                span = reduced_high[axis] - reduced_low[axis]
                if span > 1e-6:
                    vertex.co[axis] = low[axis] + (vertex.co[axis] - reduced_low[axis]) / span * (high[axis] - low[axis])
        obj.data.update()
    after = sum(len(o.data.polygons) for o in objects)
    output = DEST / source.name
    bpy.ops.wm.obj_export(filepath=str(output), export_selected_objects=True,
        forward_axis='NEGATIVE_Z', up_axis='Y', export_triangulated_mesh=True,
        export_materials=True, path_mode='RELATIVE')
    reports.append({'model': source.stem, 'sourceFaces': before,
                    'lodFaces': after, 'ratio': round(after / max(1, before), 4)})
    print(f'LOD {source.stem}: {before} -> {after}', flush=True)

manifest = DEST / 'manifest.json'
if '--new-sites' in sys.argv and manifest.exists():
    updated = {report['model'] for report in reports}
    reports = [report for report in json.loads(manifest.read_text()) if report['model'] not in updated] + reports
manifest.write_text(json.dumps(reports, indent=2) + '\n', encoding='utf-8')
print('LOD COMPLETE', len(reports), flush=True)
