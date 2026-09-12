"""Blender 4.5: bevel, unwrap, bake albedo + local AO, export game OBJ, render.

Usage: blender -b --python scripts/finish_construction_models_blender.py [-- --only STEM]
The final exported single-material game mesh is used for every preview.
"""
from pathlib import Path
import json
import math
import sys
import bpy
import bmesh
from mathutils import Vector

ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'artifacts/construction_models'
ARGS=sys.argv[sys.argv.index('--')+1:] if '--' in sys.argv else []
ONLY=ARGS[ARGS.index('--only')+1] if '--only' in ARGS else None
if '--work' in ARGS:
    WORK=ROOT/'artifacts'/ARGS[ARGS.index('--work')+1]
bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
scene=bpy.context.scene
scene.render.engine='CYCLES'
scene.cycles.samples=16
scene.cycles.use_denoising=True
scene.render.bake.margin=8
scene.render.bake.use_clear=True
scene.render.resolution_x=1000;scene.render.resolution_y=900
scene.render.resolution_percentage=100
scene.world.color=(.22,.22,.22)
scene.view_settings.view_transform='AgX'
scene.unit_settings.system='METRIC'
scene.unit_settings.scale_length=1

PHOTO={}
TINT={}

def material(index):
    mat=bpy.data.materials.new(f'Source_{index:02d}')
    mat.use_nodes=True
    nodes=mat.node_tree.nodes;nodes.clear();links=mat.node_tree.links
    out=nodes.new('ShaderNodeOutputMaterial')
    emit=nodes.new('ShaderNodeEmission')
    tex=nodes.new('ShaderNodeTexImage')
    uv=nodes.new('ShaderNodeUVMap');uv.uv_map='SourceUV'
    if index in PHOTO:
        name=PHOTO[index]
        filename=ROOT/'App/assets/third_party/polyhaven'/name/f'{name}_diff_1k.jpg'
    else:
        filename=WORK/'materials'/f'{index:02d}.png'
    tex.image=bpy.data.images.load(str(filename),check_existing=True)
    tex.extension='REPEAT' if index<16 else 'EXTEND'
    links.new(uv.outputs['UV'],tex.inputs['Vector'])
    tint=nodes.new('ShaderNodeMixRGB');tint.blend_type='MULTIPLY';tint.inputs[0].default_value=1
    tint.inputs[2].default_value=TINT.get(index,(1,1,1,1))
    links.new(tex.outputs['Color'],tint.inputs[1])
    surface=tint.outputs[0]
    if index in (0,7):
        # Painted plaster: retain fine photographic variation without turning
        # an ordinary inhabited home into a heavily stained ruin.
        paint=nodes.new('ShaderNodeMixRGB');paint.blend_type='MIX'
        paint.inputs[0].default_value=.72
        paint.inputs[2].default_value=(.67,.65,.60,1) if index==0 else (.8,.8,.77,1)
        links.new(surface,paint.inputs[1]);surface=paint.outputs[0]
    ao=nodes.new('ShaderNodeAmbientOcclusion');ao.samples=16;ao.inputs['Distance'].default_value=.48
    ao.only_local=True
    shade=nodes.new('ShaderNodeMath');shade.operation='MULTIPLY_ADD'
    shade.inputs[1].default_value=.67;shade.inputs[2].default_value=.33
    links.new(ao.outputs['AO'],shade.inputs[0])
    multiply=nodes.new('ShaderNodeMixRGB');multiply.blend_type='MULTIPLY';multiply.inputs[0].default_value=1
    links.new(surface,multiply.inputs[1]);links.new(shade.outputs[0],multiply.inputs[2])
    links.new(multiply.outputs[0],emit.inputs['Color']);links.new(emit.outputs[0],out.inputs['Surface'])
    return mat

materials={i:material(i) for i in range(32)}

def bake_model(item):
    source=json.loads((WORK/f"{item['stem']}.source.json").read_text())
    vertices=[];triangles=[]
    for face in source['faces']:
        first=len(vertices)
        vertices += [(x,-z,y) for x,y,z in face['p']]
        triangles.append((first,first+1,first+2))
    mesh=bpy.data.meshes.new(item['stem']);mesh.from_pydata(vertices,[],triangles);mesh.update()
    obj=bpy.data.objects.new(item['stem'],mesh);scene.collection.objects.link(obj)
    bpy.ops.object.select_all(action='DESELECT');obj.select_set(True);bpy.context.view_layer.objects.active=obj
    for i in range(32):mesh.materials.append(materials[i])
    source_uv=mesh.uv_layers.new(name='SourceUV')
    for polygon,face in zip(mesh.polygons,source['faces']):
        index=face['mat'];polygon.material_index=index
        n=polygon.normal
        for loop_index,local_uv in zip(polygon.loop_indices,face['uv']):
            p=mesh.vertices[mesh.loops[loop_index].vertex_index].co
            if index in PHOTO or index in (2,5,13):
                # World-space metres prevent brick/metal stretching on tall walls.
                period=.65 if index in (0,7) else 2.0
                if abs(n.z)>.7: u,v=p.x/period,p.y/period
                elif abs(n.x)>.7: u,v=p.y/period,p.z/period
                else: u,v=p.x/period,p.z/period
            else: u,v=local_uv[0],1-local_uv[1]
            source_uv.data[loop_index].uv=(u,v)
    # Welding preserves face-corner UVs; bevel only physical angle changes.
    bpy.ops.object.mode_set(mode='EDIT');bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.mesh.remove_doubles(threshold=.00001)
    bpy.ops.object.mode_set(mode='OBJECT')
    if item['source_triangles'] < 12000:
        bevel=obj.modifiers.new('12 mm edge bevel','BEVEL');bevel.width=.008;bevel.segments=2
        bevel.limit_method='ANGLE';bevel.angle_limit=math.radians(35)
        bevel.affect='EDGES'
        bpy.ops.object.modifier_apply(modifier=bevel.name)
    mesh=obj.data
    if item.get('kind')=='construction':
        minimum=min(v.co.z for v in mesh.vertices)
        for v in mesh.vertices: v.co.z-=minimum
    if item.get('kind'):
        for vertex in mesh.vertices:
            assert vertex.co.z >= -.025, (item['stem'], vertex.co.z)
            vertex.co.z=max(0.0,vertex.co.z)
    uv=mesh.uv_layers.new(name='BakeUV');mesh.uv_layers.active=uv;uv.active_render=True
    bpy.ops.object.mode_set(mode='EDIT');bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.smart_project(angle_limit=math.radians(66),island_margin=.006,area_weight=.5)
    bpy.ops.object.mode_set(mode='OBJECT')
    folder=ROOT/'App/assets'/item['asset_dir'] if 'asset_dir' in item else ROOT/'App/assets/buildings'/item['folder']
    folder.mkdir(parents=True,exist_ok=True)
    resolution=2048 if item.get('kind')=='construction' or item['stem'].startswith(('residential_017','residential_018','office_')) or item.get('kind') in ('train','station') else 1024
    baked=bpy.data.images.new(item['stem']+'_albedo_ao',width=resolution,height=resolution,alpha=False)
    for mat in mesh.materials:
        nodes=mat.node_tree.nodes
        for node in nodes:node.select=False
        target=nodes.new('ShaderNodeTexImage');target.image=baked;target.select=True;nodes.active=target
    bpy.ops.object.bake(type='EMIT')
    baked.filepath_raw=str(folder/f"{item['stem']}_diffuse.png");baked.file_format='PNG';baked.save()
    final=bpy.data.materials.new(item['stem']+'_baked');final.use_nodes=True
    nodes=final.node_tree.nodes;shader=nodes.get('Principled BSDF')
    texture=nodes.new('ShaderNodeTexImage');texture.image=baked
    final.node_tree.links.new(texture.outputs['Color'],shader.inputs['Base Color'])
    shader.inputs['Roughness'].default_value=.32 if item.get('kind') in ('vehicle','train','construction') else .67
    mesh.materials.clear();mesh.materials.append(final)
    for polygon in mesh.polygons:polygon.material_index=0
    mesh.uv_layers.remove(mesh.uv_layers['SourceUV'])
    tri=obj.modifiers.new('Game triangles','TRIANGULATE');bpy.ops.object.modifier_apply(modifier=tri.name)
    mesh=obj.data
    # Bevel intersections can leave sub-micrometre slivers that collapse at the
    # OBJ exporter's six-decimal precision. Remove only those zero-area faces.
    topology=bmesh.new();topology.from_mesh(mesh)
    collapsed=[]
    for face in topology.faces:
        p=[Vector(tuple(round(c,6) for c in vertex.co)) for vertex in face.verts]
        if len(p)==3 and (p[1]-p[0]).cross(p[2]-p[0]).length<2e-10:collapsed.append(face)
    bmesh.ops.delete(topology,geom=collapsed,context='FACES_ONLY')
    topology.to_mesh(mesh);topology.free();mesh.update();mesh.calc_loop_triangles()
    front='+Z' if item.get('kind')=='construction' else '+X' if item.get('kind')=='vehicle' else '+Z' if item.get('kind')=='train' else '-Z'
    obj['description']=item['title'];obj['front']='OBJ '+front
    obj['kind']=item.get('kind','building')
    obj['license_sources']='App/assets/construction/README.md'
    obj['texture']='Original paint and metal colours, baked local occlusion; no direct light baked'
    # Export only this object. Blender Z-up -> engine Y-up.
    bpy.ops.wm.obj_export(filepath=str(folder/f"{item['stem']}.obj"),export_selected_objects=True,
                          forward_axis='NEGATIVE_Z',up_axis='Y',export_materials=True,
                          export_uv=True,export_normals=True,export_triangulated_mesh=True,path_mode='STRIP')
    (folder/f"{item['stem']}.toml").write_text('# Realistic pack; metres; Y up; front '+front+'\nsetback_from_road_m = 1.8\nscale = 1.0\n',encoding='utf-8')
    # Remove baked target nodes from shared authoring materials before next bake.
    for mat in materials.values():
        for node in list(mat.node_tree.nodes):
            if node.type=='TEX_IMAGE' and node.image==baked:mat.node_tree.nodes.remove(node)
    bounds=[obj.matrix_world@Vector(c) for c in obj.bound_box]
    result={k:v for k,v in item.items()}
    result.update(triangles=len(mesh.polygons),vertices=len(mesh.vertices),materials=1,
                  texture_resolution=resolution,dimensions=list(obj.dimensions),
                  ground=min(v.z for v in bounds),obj=str((folder/f"{item['stem']}.obj").relative_to(ROOT)).replace('\\','/'))
    assert len(mesh.polygons)<100000,(item['stem'],len(mesh.polygons))
    assert abs(result['ground'])<.001
    (WORK/f"{item['stem']}.verified.json").write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    obj.hide_render=True
    return obj

manifest=json.loads((WORK/'source_manifest.json').read_text())
objects=[]
for item in manifest:
    if ONLY and item['stem']!=ONLY:continue
    print('AUTHORING',item['stem'],flush=True)
    objects.append(bake_model(item))

def area(name,location,power,size):
    data=bpy.data.lights.new(name,'AREA');data.energy=power;data.shape='DISK';data.size=size
    obj=bpy.data.objects.new(name,data);scene.collection.objects.link(obj);obj.location=location
    obj.rotation_euler=(Vector((0,0,3))-obj.location).to_track_quat('-Z','Y').to_euler()
    return obj
key=area('Key',(1,8,16),1800,8)
fill=area('Fill',(-10,-5,10),1000,10)
bpy.ops.object.light_add(type='SUN');sun=bpy.context.object;sun.data.energy=1.6;sun.data.angle=.14
sun.rotation_euler=(.45,-.25,-.45)
bpy.ops.mesh.primitive_plane_add(size=250);ground=bpy.context.object;ground.name='Preview ground';ground.location.z=-.02
groundmat=bpy.data.materials.new('Ground');groundmat.diffuse_color=(.37,.4,.39,1);ground.data.materials.append(groundmat)
bpy.ops.object.camera_add();camera=bpy.context.object;scene.camera=camera;camera.data.type='ORTHO'
for obj in objects:
    obj.hide_render=False
    height=obj.dimensions.z
    target=Vector((0,0,height*.5))
    light_scale=max(1,height/10)
    key.location=target+Vector((1,8,14))*light_scale
    fill.location=target+Vector((-10,-5,8))*light_scale
    key.data.energy=1800*light_scale**2;key.data.size=8*light_scale
    fill.data.energy=1000*light_scale**2;fill.data.size=10*light_scale
    for light in (key,fill):
        light.rotation_euler=(target-light.location).to_track_quat('-Z','Y').to_euler()
    camera.data.ortho_scale=max(5 if obj['kind']=='vehicle' else 11.5,height*1.25,max(obj.dimensions.x,obj.dimensions.y)*1.3)
    for side,delta in [('front',(12,17,11)),('back',(-12,-17,11))]:
        camera.location=target+Vector(delta)*max(1,height/10)
        camera.rotation_euler=(target-camera.location).to_track_quat('-Z','Y').to_euler()
        rotation=camera.rotation_euler.to_matrix().transposed()
        corners=[rotation@(Vector(c)-target) for c in obj.bound_box]
        camera.data.ortho_scale=1.14*max(max(p.x for p in corners)-min(p.x for p in corners),
                                      (max(p.y for p in corners)-min(p.y for p in corners))/.9)
        scene.render.filepath=str(WORK/f'{obj.name}_{side}.png')
        bpy.ops.render.render(write_still=True)
    obj.hide_render=True
if not ONLY:
    for i,obj in enumerate(objects):
        spacing=34 if '--work' in ARGS else 13
        obj.hide_render=False;obj.location=((i%6)*spacing-2.5*spacing,-(i//6)*(spacing+2)+(spacing+2)*1.5,0)
    camera.location=(50,90,65);target=Vector((0,0,6))
    camera.rotation_euler=(target-camera.location).to_track_quat('-Z','Y').to_euler()
    camera.data.ortho_scale=250 if '--work' in ARGS else 110
    scene.render.resolution_x=2400;scene.render.resolution_y=1600
    scene.render.filepath=str(WORK/'gallery.png')
    key.location=(0,5,45);key.data.energy=9000;key.data.size=35
    fill.location=(-30,-10,40);fill.data.energy=6000;fill.data.size=30
    for light in (key,fill):
        light.rotation_euler=(target-light.location).to_track_quat('-Z','Y').to_euler()
    bpy.ops.file.pack_all()
    bpy.ops.wm.save_as_mainfile(filepath=str(WORK/'construction_machinery.blend'))
    bpy.ops.render.render(write_still=True)
    records=[json.loads((WORK/f"{i['stem']}.verified.json").read_text()) for i in manifest]
    (WORK/'manifest.json').write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8')
print('REALISTIC_PACK_COMPLETE',len(objects),flush=True)
