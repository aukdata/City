"""Reframe final baked meshes without rebaking or changing game geometry."""
from pathlib import Path
import json
import bpy
from mathutils import Vector

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'artifacts/civic_transport'
bpy.ops.wm.open_mainfile(filepath=str(WORK / 'realistic_town.blend'))
scene = bpy.context.scene
camera = scene.camera
scene.render.resolution_x = 1000
scene.render.resolution_y = 900
items = json.loads((WORK / 'source_manifest.json').read_text())
objects = [bpy.data.objects[item['stem']] for item in items]
positions = [obj.location.copy() for obj in objects]
for obj in objects:
    obj.hide_render = True
    obj.location = (0, 0, 0)
for obj, item in zip(objects, items):
    obj.hide_render = False
    front = '+X' if item['kind'] == 'vehicle' else '+Z' if item['kind'] == 'train' else '-Z'
    obj['front'] = 'OBJ ' + front
    folder = ROOT / 'App/assets' / item['asset_dir']
    (folder / (item['stem'] + '.toml')).write_text(
        '# Metres; Y up; front ' + front + '\nsetback_from_road_m = 1.8\nscale = 1.0\n', encoding='utf-8')
    target = Vector((0, 0, obj.dimensions.z * .5))
    for name, delta, power, size in [('Key', (1, 8, 14), 1800, 8), ('Fill', (-10, -5, 8), 1000, 10)]:
        light = bpy.data.objects[name]
        factor = max(1, obj.dimensions.z / 10)
        light.location = target + Vector(delta) * factor
        light.rotation_euler = (target - light.location).to_track_quat('-Z', 'Y').to_euler()
        light.data.energy = power * factor ** 2
        light.data.size = size * factor
    for side, delta in [('front', (12, 17, 11)), ('back', (-12, -17, 11))]:
        camera.location = target + Vector(delta) * 3
        camera.rotation_euler = (target - camera.location).to_track_quat('-Z', 'Y').to_euler()
        rotation = camera.rotation_euler.to_matrix().transposed()
        corners = [rotation @ (Vector(c) - target) for c in obj.bound_box]
        camera.data.ortho_scale = 1.14 * max(
            max(p.x for p in corners) - min(p.x for p in corners),
            (max(p.y for p in corners) - min(p.y for p in corners)) / .9)
        scene.render.filepath = str(WORK / f'{obj.name}_{side}.png')
        bpy.ops.render.render(write_still=True)
    obj.hide_render = True
for obj, position in zip(objects, positions):
    obj.location = position
    obj.hide_render = False
camera.location = (50, 90, 65)
camera.rotation_euler = (Vector((0, 0, 6)) - camera.location).to_track_quat('-Z', 'Y').to_euler()
camera.data.ortho_scale = 250
bpy.ops.wm.save_as_mainfile(filepath=str(WORK / 'civic_transport.blend'))
print('CIVIC_PREVIEWS_COMPLETE', flush=True)
