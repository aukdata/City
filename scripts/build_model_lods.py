"""Build material-preserving medium/far LODs for every shipped OBJ.

Run with Blender: blender --background --python scripts/build_model_lods.py
Source models are never modified. Outputs mirror their paths below assets/lod/.
"""
from pathlib import Path
import json
import sys
import bpy
import bmesh
from mathutils.kdtree import KDTree

ASSETS = Path(__file__).resolve().parents[1] / 'App/assets'
DEST = ASSETS / 'lod'
SOURCES = sorted(p for p in ASSETS.rglob('*.obj') if 'lod' not in p.relative_to(ASSETS).parts)
reports = []

for source in SOURCES:
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    bpy.ops.wm.obj_import(filepath=str(source), forward_axis='NEGATIVE_Z', up_axis='Y')
    objects = [o for o in bpy.context.selected_objects if o.type == 'MESH' and o.data.vertices]
    if not objects:
        raise RuntimeError(f'Empty source: {source}')
    bpy.context.view_layer.objects.active = objects[0]
    bpy.ops.object.join()
    original = bpy.context.object
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    triangulate = original.modifiers.new('Source triangulation', 'TRIANGULATE')
    bpy.ops.object.modifier_apply(modifier=triangulate.name)
    source_faces = len(original.data.polygons)
    original_mesh = original.data.copy()
    used = {i for face in original_mesh.polygons for i in face.vertices}
    points = [original_mesh.vertices[i].co.copy() for i in used]
    low = [min(p[a] for p in points) for a in range(3)]
    high = [max(p[a] for p in points) for a in range(3)]
    tree = KDTree(len(points))
    for i, point in enumerate(points):
        tree.insert(point, i)
    tree.balance()
    entry = {'source': source.relative_to(ASSETS).as_posix(), 'sourceTriangles': source_faces, 'levels': []}
    for level, ratio, minimum in [(1, .18, 48), (2, .025, 24)]:
        original.data = original_mesh.copy()
        far_vehicle = level == 2 and source.parent.name == 'vehicles'
        if far_vehicle:
            size = [high[a]-low[a] for a in range(3)]
            center = [(high[a]+low[a])*.5 for a in range(3)]
            palette = {'sedan':(.64,.68,.74),'kei_wagon':(.70,.76,.79),'city_bus':(.86,.78,.58),
                       'fire_engine':(.7,.055,.045),'patrol_car':(.84,.85,.85),
                       'delivery_truck':(.72,.73,.71),'cargo_truck':(.68,.69,.70),'car':(.65,.68,.72)}
            materials=[]
            for name,color in [('body',palette[source.stem]),('glass',(.055,.11,.15)),('tyres',(.025,.025,.03))]:
                material=bpy.data.materials.new(name);material.diffuse_color=(*color,1);materials.append(material)
            parts=[]
            def box(offset,fractions,material):
                bpy.ops.mesh.primitive_cube_add(size=1,location=(center[0]+offset[0]*size[0],center[1]+offset[1]*size[1],low[2]+offset[2]*size[2]))
                obj=bpy.context.object;obj.scale=tuple(fractions[a]*size[a] for a in range(3))
                bpy.ops.object.transform_apply(location=True,rotation=True,scale=True)
                obj.data.materials.append(materials[material]);parts.append(obj)
            box((0,0,.45),(1,1,.50),0);box((0,0,.10),(.75,.94,.20),2)
            if source.stem in ('cargo_truck','delivery_truck','fire_engine'):
                box((-.12,0,.75),(.72,.96,.50),0);box((.36,0,.72),(.27,.84,.44),1);box((.36,0,.95),(.28,.88,.10),0)
            else:
                box((0,0,.825),(.90 if source.stem=='city_bus' else .53,.87,.35),1)
                box((0,0,.97),(.92 if source.stem=='city_bus' else .54,.89,.06),0)
            bpy.ops.object.select_all(action='DESELECT')
            for obj in parts:obj.select_set(True)
            bpy.context.view_layer.objects.active=parts[0];bpy.ops.object.join()
            original.data=bpy.context.object.data.copy();bpy.data.objects.remove(bpy.context.object,do_unlink=True)
            original.select_set(True);bpy.context.view_layer.objects.active=original
            modifier=original.modifiers.new('Far triangles','TRIANGULATE');bpy.ops.object.modifier_apply(modifier=modifier.name)
        target = max(minimum, round(source_faces * ratio))
        if target < source_faces and not far_vehicle:
            modifier = original.modifiers.new('Distance reduction', 'DECIMATE')
            modifier.ratio = target / source_faces
            modifier.use_collapse_triangulate = True
            bpy.ops.object.modifier_apply(modifier=modifier.name)
        # Thin trim can extrapolate beyond the original shell during collapse.
        if not far_vehicle:
            for vertex in original.data.vertices:
                nearest, _, distance = tree.find(vertex.co)
                if distance > .08: vertex.co = nearest
        mesh=bmesh.new();mesh.from_mesh(original.data)
        bmesh.ops.remove_doubles(mesh,verts=list(mesh.verts),dist=1e-6)
        bmesh.ops.dissolve_degenerate(mesh,edges=list(mesh.edges),dist=1e-8)
        mesh.to_mesh(original.data);mesh.free()
        used={i for face in original.data.polygons for i in face.vertices}
        reduced_low=[min(original.data.vertices[i].co[a] for i in used) for a in range(3)]
        reduced_high=[max(original.data.vertices[i].co[a] for i in used) for a in range(3)]
        for vertex in original.data.vertices:
            for axis in range(3):
                span = reduced_high[axis] - reduced_low[axis]
                if span > 1e-6:
                    vertex.co[axis] = low[axis] + (vertex.co[axis] - reduced_low[axis]) / span * (high[axis] - low[axis])
        original.data.update()
        output = DEST / source.relative_to(ASSETS)
        output = output.with_name(f'{source.stem}.lod{level}.obj')
        output.parent.mkdir(parents=True, exist_ok=True)
        bpy.ops.wm.obj_export(filepath=str(output), export_selected_objects=True,
            forward_axis='NEGATIVE_Z', up_axis='Y', export_triangulated_mesh=True,
            export_materials=True, path_mode='RELATIVE')
        entry['levels'].append({'level': level, 'path': output.relative_to(ASSETS).as_posix(),
                                'triangles': len(original.data.polygons)})
    reports.append(entry)
    print('LOD', entry['source'], source_faces, [v['triangles'] for v in entry['levels']], flush=True)

DEST.mkdir(parents=True, exist_ok=True)
(DEST / 'manifest.json').write_text(json.dumps(reports, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print('COMPLETE', len(reports), flush=True)

# Signal lamps keep independent names so their live colours still animate.
import runpy
runpy.run_path(str(Path(__file__).with_name("build_signal_lods.py")))
