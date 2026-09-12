"""Run with Blender --background --python scripts/review_buildings_blender.py.

Imports the shipped OBJ files, packs their atlas, saves an editable gallery and
renders front/back views. Preview lights and ground are not game assets.
"""
from pathlib import Path
import json
import math
import bpy
from mathutils import Vector

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'App/assets/buildings/commercial'
OUTPUT = ROOT / 'artifacts/building_models'
OUTPUT.mkdir(parents=True, exist_ok=True)
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
scene = bpy.context.scene
scene.render.engine = 'CYCLES'
scene.cycles.samples = 24
scene.cycles.use_denoising = True
scene.render.resolution_x = 900
scene.render.resolution_y = 900
scene.render.resolution_percentage = 100
scene.render.image_settings.file_format = 'PNG'
scene.world.color = (.35, .35, .35)
scene.view_settings.view_transform = 'AgX'
scene.unit_settings.system = 'METRIC'
scene.unit_settings.scale_length = 1

def area(name, location, power, size):
    data = bpy.data.lights.new(name, 'AREA')
    data.energy, data.shape, data.size = power, 'DISK', size
    obj = bpy.data.objects.new(name, data)
    scene.collection.objects.link(obj)
    obj.location = location
    obj.rotation_euler = (Vector((0, 0, 2)) - obj.location).to_track_quat('-Z', 'Y').to_euler()
    return obj

key = area('Large softbox', (0, 7, 14), 1900, 9)
fill = area('Fill', (-9, -3, 7), 1000, 10)
bpy.ops.object.light_add(type='SUN', location=(0,0,15))
sun = bpy.context.object
sun.name = 'Sun'
sun.data.energy = 1.4
sun.data.angle = math.radians(12)
sun.rotation_euler = (math.radians(25), math.radians(-20), math.radians(-25))
bpy.ops.mesh.primitive_plane_add(size=200)
ground = bpy.context.object
ground.name = 'PREVIEW ONLY - ground'
ground.location.z = -.012
mat = bpy.data.materials.new('Gallery ground')
mat.diffuse_color = (.55,.59,.56,1)
ground.data.materials.append(mat)
bpy.ops.object.camera_add(location=(12,16,12))
camera = bpy.context.object
camera.name = 'Asset review camera'
camera.data.type = 'ORTHO'
scene.camera = camera

manifest = json.loads((SOURCE/'model_manifest.json').read_text(encoding='utf-8'))
objects = []
validation = []
for item in manifest:
    bpy.ops.wm.obj_import(filepath=str(SOURCE/f"{item['stem']}.obj"), forward_axis='NEGATIVE_Z', up_axis='Y')
    imported = list(bpy.context.selected_objects)
    assert len(imported) == 1, item['stem']
    obj = imported[0]
    obj.name = item['stem']
    obj['description'] = item['title']
    obj['game_axes'] = 'OBJ: Y up, front -Z. Blender: Z up, front +Y.'
    obj['source'] = f"App/assets/buildings/commercial/{item['stem']}.obj"
    assert len(obj.data.polygons) == item['triangles']
    assert len(obj.data.materials) == 1
    assert obj.data.uv_layers.active is not None
    texture_nodes = [n for n in obj.data.materials[0].node_tree.nodes if n.type == 'TEX_IMAGE']
    assert texture_nodes and texture_nodes[0].image.size[0] == 1024
    for polygon in obj.data.polygons:
        assert polygon.area > 1e-10
    # Keep the game mesh unchanged: no unapplied bevel/subdivision modifiers.
    objects.append(obj)
    obj.hide_render = True
    validation.append({'stem':item['stem'],'triangles':len(obj.data.polygons),
                       'uv':True,'diffuse_texture_loaded':True,'materials':1})

for obj in objects:
    obj.hide_render = False
    height = obj.dimensions.z
    target = Vector((0,0,height*.44))
    camera.data.ortho_scale = max(11.2,height*1.42)
    for suffix, direction in [('front',(12,16,11)),('back',(-12,-16,11))]:
        camera.location = target + Vector(direction)
        camera.rotation_euler = (target-camera.location).to_track_quat('-Z','Y').to_euler()
        scene.render.filepath = str(OUTPUT/f'{obj.name}_blender_{suffix}.png')
        bpy.ops.render.render(write_still=True)
    obj.hide_render = True

for index,obj in enumerate(objects):
    obj.hide_render=False
    obj.location = ((index%4)*11-16.5, -(index//4)*13+6.5, 0)
camera.location = (26,40,35)
target = Vector((0,0,1))
camera.rotation_euler = (target-camera.location).to_track_quat('-Z','Y').to_euler()
camera.data.ortho_scale = 50
scene.render.resolution_x=2000
scene.render.resolution_y=1300
scene.render.filepath=str(OUTPUT/'blender_gallery.png')
key.location=(0,4,20); key.data.energy=3500; key.data.size=18
bpy.ops.wm.save_as_mainfile(filepath=str(OUTPUT/'town_buildings.blend'))
bpy.ops.file.pack_all()
bpy.ops.wm.save_as_mainfile(filepath=str(OUTPUT/'town_buildings.blend'))
bpy.ops.render.render(write_still=True)
(OUTPUT/'blender_validation.json').write_text(json.dumps(validation,indent=2)+'\n',encoding='utf-8')
print('BUILDING_REVIEW_COMPLETE: 7 OBJ imports, 14 views, packed editable blend, gallery.')
