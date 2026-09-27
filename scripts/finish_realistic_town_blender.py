"""Blender 4.5: bevel, unwrap, bake albedo + local AO, export game OBJ, render.

Usage: blender -b --python scripts/finish_realistic_town_blender.py [-- --only STEM]
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
WORK=ROOT/'artifacts/realistic_town'
ARGS=sys.argv[sys.argv.index('--')+1:] if '--' in sys.argv else []
ONLY=ARGS[ARGS.index('--only')+1] if '--only' in ARGS else None
if '--work' in ARGS:
    WORK=ROOT/'artifacts'/ARGS[ARGS.index('--work')+1]
bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
scene=bpy.context.scene
scene.render.engine='CYCLES'
scene.cycles.samples=16
# Bake on the GPU when Cycles finds one; the CPU path gives the same image.
try:
    cycles=bpy.context.preferences.addons['cycles'].preferences
    for backend in ('OPTIX','CUDA','HIP','ONEAPI','METAL'):
        try:cycles.compute_device_type=backend
        except TypeError:continue
        cycles.get_devices()
        gpus=[d for d in cycles.devices if d.type==backend]
        if gpus:
            for d in cycles.devices:d.use=d.type==backend
            scene.cycles.device='GPU';break
except Exception as error:
    print('GPU bake unavailable:',error)
print('CYCLES DEVICE',scene.cycles.device,flush=True)
scene.cycles.use_denoising=True
scene.render.bake.margin=8
scene.render.bake.use_clear=True
scene.render.resolution_x=1000;scene.render.resolution_y=900
scene.render.resolution_percentage=100
scene.world.color=(.22,.22,.22)
scene.view_settings.view_transform='AgX'
scene.unit_settings.system='METRIC'
scene.unit_settings.scale_length=1

PHOTO={0:'grey_plaster_02',1:'brick_wall_11',3:'wood_planks_grey',6:'concrete',7:'grey_plaster_02',14:'wood_planks_grey'}
TINT={0:(1.05,1.01,.9,1),1:(1,1,1,1),3:(.7,.49,.31,1),6:(.92,.93,.9,1),7:(1.3,1.28,1.2,1),14:(1.12,.94,.65,1)}

# Source material groups for weathering. Indices follow the shared town atlas.
WALLS={0,1,2,3,6,7,14}
ROOFS={5}
METALS={11,13}

def mix(nodes,links,blend,a,b,factor):
    node=nodes.new('ShaderNodeMixRGB');node.blend_type=blend
    for socket,value in ((node.inputs[0],factor),(node.inputs[1],a),(node.inputs[2],b)):
        if isinstance(value,(int,float)):socket.default_value=value
        elif isinstance(value,tuple):socket.default_value=value
        else:links.new(value,socket)
    return node.outputs[0]

def math_node(nodes,links,operation,a,b=None,clamp=False):
    node=nodes.new('ShaderNodeMath');node.operation=operation;node.use_clamp=clamp
    for socket,value in ((node.inputs[0],a),(node.inputs[1],b)):
        if value is None:continue
        if isinstance(value,(int,float)):socket.default_value=value
        else:links.new(value,socket)
    return node.outputs[0]

def remap(nodes,links,value,from_min,from_max,to_min,to_max):
    node=nodes.new('ShaderNodeMapRange');node.clamp=True
    links.new(value,node.inputs['Value'])
    for name,v in (('From Min',from_min),('From Max',from_max),('To Min',to_min),('To Max',to_max)):
        node.inputs[name].default_value=v
    return node.outputs['Result']

def noise(nodes,links,vector,scale,detail=4,roughness=.55,stretch=None):
    if stretch:
        mapping=nodes.new('ShaderNodeMapping');mapping.inputs['Scale'].default_value=stretch
        links.new(vector,mapping.inputs['Vector']);vector=mapping.outputs['Vector']
    node=nodes.new('ShaderNodeTexNoise');node.inputs['Scale'].default_value=scale
    node.inputs['Detail'].default_value=detail;node.inputs['Roughness'].default_value=roughness
    links.new(vector,node.inputs['Vector'])
    return node.outputs['Fac']

def glass(nodes,links):
    """Opaque window glass: reflected sky and neighbours, with lived-in rooms.

    The engine has no reflection probes, so the pane albedo carries a soft
    view-independent reflection and a per-window interior (curtains, blinds,
    dark rooms) chosen by a Voronoi cell about one window wide.
    """
    coord=nodes.new('ShaderNodeTexCoord').outputs['Object']
    cells=nodes.new('ShaderNodeTexVoronoi');cells.inputs['Scale'].default_value=.62
    links.new(coord,cells.inputs['Vector'])
    pick=nodes.new('ShaderNodeSeparateColor');links.new(cells.outputs['Color'],pick.inputs[0])
    room=nodes.new('ShaderNodeValToRGB');ramp=room.color_ramp;ramp.interpolation='CONSTANT'
    ramp.elements[0].color=(.035,.04,.045,1)
    for position,color in ((.38,(.43,.40,.33,1)),(.55,(.10,.09,.08,1)),(.7,(.56,.55,.50,1)),(.84,(.045,.05,.055,1))):
        element=ramp.elements.new(position);element.color=color
    links.new(pick.outputs[0],room.inputs['Fac'])
    reflect=noise(nodes,links,coord,.9,3,.5,(1,1,.35))
    sky=nodes.new('ShaderNodeValToRGB');r=sky.color_ramp
    r.elements[0].position=.3;r.elements[0].color=(.07,.085,.095,1)
    r.elements[1].position=.72;r.elements[1].color=(.36,.42,.47,1)
    links.new(reflect,sky.inputs['Fac'])
    return mix(nodes,links,'MIX',room.outputs['Color'],sky.outputs['Color'],.58)

def weather(nodes,links,surface,index):
    """Bake-time ageing that reads at game distance: never a ruin, never new.

    Colour variation across the facade, rain streaks on vertical faces, splash
    grime at the wall foot, blotchy roofs and slightly worn arrises.
    """
    if index>=16:return surface
    coord=nodes.new('ShaderNodeTexCoord').outputs['Object']
    geometry=nodes.new('ShaderNodeNewGeometry')
    normal=nodes.new('ShaderNodeSeparateXYZ');links.new(geometry.outputs['Normal'],normal.inputs[0])
    position=nodes.new('ShaderNodeSeparateXYZ');links.new(coord,position.inputs[0])
    up=math_node(nodes,links,'ABSOLUTE',normal.outputs['Z'])
    vertical=math_node(nodes,links,'SUBTRACT',1,up,True)
    macro=remap(nodes,links,noise(nodes,links,coord,.28,3,.5),.3,.7,.9,1.06)
    surface=mix(nodes,links,'MULTIPLY',surface,macro,1)
    streak_strength={0:.55,7:.6,6:.5,1:.35,2:.45,3:.3,14:.3,13:.35,11:.15}.get(index,0)
    if streak_strength:
        streak=remap(nodes,links,noise(nodes,links,coord,1.0,5,.62,(9,9,.22)),.5,.76,0,streak_strength)
        streak=math_node(nodes,links,'MULTIPLY',streak,vertical)
        surface=mix(nodes,links,'MULTIPLY',surface,(.6,.58,.54,1),streak)
    if index in WALLS or index in METALS:
        foot=remap(nodes,links,position.outputs['Z'],.05,.85,.5,0)
        surface=mix(nodes,links,'MULTIPLY',surface,(.62,.58,.5,1),math_node(nodes,links,'MULTIPLY',foot,vertical))
    if index in ROOFS or index in (6,7):
        flat=remap(nodes,links,up,.6,.95,0,1)
        blotch=remap(nodes,links,noise(nodes,links,coord,3.2,7,.68),.45,.72,0,.3 if index in ROOFS else .25)
        stains=mix(nodes,links,'MULTIPLY',surface,(.66,.66,.62,1),math_node(nodes,links,'MULTIPLY',blotch,flat))
        dust=remap(nodes,links,noise(nodes,links,coord,1.1,3,.5),.5,.8,0,.14 if index in ROOFS else .08)
        surface=mix(nodes,links,'MIX',stains,(.47,.46,.43,1),math_node(nodes,links,'MULTIPLY',dust,flat))
    if index in WALLS or index in METALS or index in ROOFS:
        bevel=nodes.new('ShaderNodeBevel');bevel.samples=8;bevel.inputs['Radius'].default_value=.03
        dot=nodes.new('ShaderNodeVectorMath');dot.operation='DOT_PRODUCT'
        links.new(bevel.outputs['Normal'],dot.inputs[0]);links.new(geometry.outputs['Normal'],dot.inputs[1])
        edge=remap(nodes,links,dot.outputs['Value'],.995,.9,0,.45)
        surface=mix(nodes,links,'MULTIPLY',surface,(1.2,1.19,1.16,1),edge)
    return surface

def material(index,aged=True):
    mat=bpy.data.materials.new(f'Source_{index:02d}'+('' if aged else '_clean'))
    mat.use_nodes=True
    nodes=mat.node_tree.nodes;nodes.clear();links=mat.node_tree.links
    out=nodes.new('ShaderNodeOutputMaterial')
    emit=nodes.new('ShaderNodeEmission')
    if '--vehicle-paint' in ARGS:
        # Factory paint, rubber, glass and alloy: no plaster texture on coachwork.
        colors = {4:(.035,.075,.10,1), 7:(.78,.80,.79,1), 8:(.04,.18,.22,1),
                  9:(.52,.025,.018,1), 10:(.85,.44,.035,1), 11:(.025,.03,.033,1),
                  13:(.42,.46,.48,1)}
        paint=nodes.new('ShaderNodeRGB')
        paint.outputs[0].default_value=colors.get(index,(.55,.55,.52,1))
        surface=paint.outputs[0]
        if '--train-displays' in ARGS and index in (30,31):
            tex=nodes.new('ShaderNodeTexImage')
            tex.image=bpy.data.images.load(str(WORK/'materials'/f'{index:02d}.png'),check_existing=True)
            uv=nodes.new('ShaderNodeUVMap');uv.uv_map='SourceUV'
            links.new(uv.outputs['UV'],tex.inputs['Vector']);surface=tex.outputs['Color']
    else:
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
        if aged:
            if index==4:surface=glass(nodes,links)
            surface=weather(nodes,links,surface,index)
    ao=nodes.new('ShaderNodeAmbientOcclusion');ao.samples=16;ao.inputs['Distance'].default_value=.48
    ao.only_local=True
    shade=nodes.new('ShaderNodeMath');shade.operation='MULTIPLY_ADD'
    shade.inputs[1].default_value=.67;shade.inputs[2].default_value=.33
    links.new(ao.outputs['AO'],shade.inputs[0])
    multiply=nodes.new('ShaderNodeMixRGB');multiply.blend_type='MULTIPLY';multiply.inputs[0].default_value=1
    links.new(surface,multiply.inputs[1]);links.new(shade.outputs[0],multiply.inputs[2])
    surface=multiply.outputs[0]
    if aged and '--vehicle-paint' not in ARGS:
        # Room-scale occlusion darkens wall feet, parapet corners and recesses
        # that the 0.48 m pass cannot reach, as an overcast sky would.
        wide=nodes.new('ShaderNodeAmbientOcclusion');wide.samples=16;wide.only_local=True
        wide.inputs['Distance'].default_value=2.2
        surface=mix(nodes,links,'MULTIPLY',surface,remap(nodes,links,wide.outputs['AO'],0,1,.72,1),1)
    links.new(surface,emit.inputs['Color']);links.new(emit.outputs[0],out.inputs['Surface'])
    return mat

# Buildings age; vehicles and trains sharing a work folder stay factory clean.
material_sets={}
def materials_for(item):
    aged=item.get('kind') not in ('vehicle','train')
    if aged not in material_sets:material_sets[aged]={i:material(i,aged) for i in range(32)}
    return material_sets[aged]

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
    materials=materials_for(item)
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
        bevel=obj.modifiers.new('12 mm edge bevel','BEVEL');bevel.width=.012;bevel.segments=1
        bevel.limit_method='ANGLE';bevel.angle_limit=math.radians(35)
        bevel.affect='EDGES'
        bpy.ops.object.modifier_apply(modifier=bevel.name)
    mesh=obj.data
    if item.get('kind'):
        for vertex in mesh.vertices:
            assert vertex.co.z >= -.025, (item['stem'], vertex.co.z)
            vertex.co.z=max(0.0,vertex.co.z)
    uv=mesh.uv_layers.new(name='BakeUV');mesh.uv_layers.active=uv;uv.active_render=True
    bpy.ops.object.mode_set(mode='EDIT');bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.smart_project(angle_limit=math.radians(66),island_margin=.006,area_weight=.5)
    # Smart project leaves most of the atlas empty; repacking roughly doubles texel density.
    bpy.ops.uv.select_all(action='SELECT')
    bpy.ops.uv.pack_islands(rotate=True,rotate_method='CARDINAL',scale=True,margin_method='FRACTION',margin=.002,shape_method='CONVEX')
    bpy.ops.object.mode_set(mode='OBJECT')
    folder=ROOT/'App/assets'/item['asset_dir'] if 'asset_dir' in item else ROOT/'App/assets/buildings'/item['folder']
    folder.mkdir(parents=True,exist_ok=True)
    large=max(obj.dimensions)>14 and item.get('kind') not in ('vehicle',)
    resolution=2048 if large or item['stem'].startswith(('residential_017','residential_018','office_')) or item.get('kind') in ('train','station') else 1024
    baked=bpy.data.images.new(item['stem']+'_albedo_ao',width=resolution,height=resolution,alpha=False)
    for mat in mesh.materials:
        nodes=mat.node_tree.nodes
        for node in nodes:node.select=False
        target=nodes.new('ShaderNodeTexImage');target.image=baked;target.select=True;nodes.active=target
    bpy.ops.object.bake(type='EMIT')
    baked.filepath_raw=str(folder/f"{item['stem']}_diffuse.png");baked.file_format='PNG';baked.save()
    final=bpy.data.materials.new(item['stem']+'_baked');final.use_nodes=True
    nodes=final.node_tree.nodes;shader=next(n for n in nodes if n.type=='BSDF_PRINCIPLED')
    texture=nodes.new('ShaderNodeTexImage');texture.image=baked
    final.node_tree.links.new(texture.outputs['Color'],shader.inputs['Base Color'])
    shader.inputs['Roughness'].default_value=.32 if item.get('kind') in ('vehicle','train') else .67
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
    front='+X' if item.get('kind')=='vehicle' else '+Z' if item.get('kind')=='train' else '-Z'
    obj['description']=item['title'];obj['front']='OBJ '+front
    obj['kind']=item.get('kind','building')
    obj['license_sources']='App/assets/licenses/REALISTIC_MATERIALS.json'
    obj['texture']='Photographic CC0 albedo + local AO; no direct light baked'
    # Export only this object. Blender Z-up -> engine Y-up.
    bpy.ops.wm.obj_export(filepath=str(folder/f"{item['stem']}.obj"),export_selected_objects=True,
                          forward_axis='NEGATIVE_Z',up_axis='Y',export_materials=True,
                          export_uv=True,export_normals=True,export_triangulated_mesh=True,path_mode='STRIP')
    if item.get('kind') not in ('vehicle','train'):
        # Plaster, brick and roofing are matte; Blender's default Ks 0.5 gave the
        # engine's Phong term a plastic sheen across whole buildings.
        mtl=folder/f"{item['stem']}.mtl"
        lines=['Ks 0.040000 0.040000 0.040000' if l.startswith('Ks ') else 'Ns 12.000000' if l.startswith('Ns ') else l
               for l in mtl.read_text(encoding='utf-8').splitlines()]
        mtl.write_text('\n'.join(lines)+'\n',encoding='utf-8')
    toml=folder/f"{item['stem']}.toml"
    # Keep hand-measured keys such as front_wall_z_m from earlier passes.
    if not toml.exists():toml.write_text('# Realistic pack; metres; Y up; front '+front+'\nsetback_from_road_m = 1.8\nscale = 1.0\n',encoding='utf-8')
    # Remove baked target nodes from shared authoring materials before next bake.
    for mat in materials.values():
        for node in list(mat.node_tree.nodes):
            if node.type=='TEX_IMAGE' and node.image==baked:mat.node_tree.nodes.remove(node)
    bounds=[obj.matrix_world@Vector(c) for c in obj.bound_box]
    result={k:v for k,v in item.items()}
    result.update(triangles=len(mesh.polygons),vertices=len(mesh.vertices),materials=1,
                  texture_resolution=resolution,dimensions=list(obj.dimensions),
                  ground=min(v.z for v in bounds),obj=str((folder/f"{item['stem']}.obj").relative_to(ROOT)).replace('\\','/'))
    assert len(mesh.polygons)<70000,(item['stem'],len(mesh.polygons))
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
    bpy.ops.wm.save_as_mainfile(filepath=str(WORK/'realistic_town.blend'))
    bpy.ops.render.render(write_still=True)
    records=[json.loads((WORK/f"{i['stem']}.verified.json").read_text()) for i in manifest]
    (WORK/'manifest.json').write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8')
print('REALISTIC_PACK_COMPLETE',len(objects),flush=True)
