from pathlib import Path
import bpy,json
ROOT=Path(__file__).resolve().parents[1];ASSETS=ROOT/'App/assets';OUT=ASSETS/'lod/signals'
source=ASSETS/'signals/signal.obj';report={'source':'signals/signal.obj','levels':[]}
for level,ratio in [(1,.18),(2,.025)]:
 bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
 bpy.ops.wm.obj_import(filepath=str(source),forward_axis='NEGATIVE_Z',up_axis='Y')
 objects=[o for o in bpy.context.selected_objects if o.type=='MESH'];source_count=0;count=0
 for obj in objects:
  bpy.context.view_layer.objects.active=obj
  bpy.ops.object.transform_apply(location=True,rotation=True,scale=True)
  m=obj.modifiers.new('Triangles','TRIANGULATE');bpy.ops.object.modifier_apply(modifier=m.name)
  n=len(obj.data.polygons);source_count+=n
  if n>12:
   m=obj.modifiers.new('Distance','DECIMATE');m.ratio=max(12/n,ratio);m.use_collapse_triangulate=True;bpy.ops.object.modifier_apply(modifier=m.name)
  count+=len(obj.data.polygons)
 bpy.ops.object.select_all(action='SELECT')
 out=OUT/f'signal.lod{level}.obj'
 bpy.ops.wm.obj_export(filepath=str(out),export_selected_objects=True,forward_axis='NEGATIVE_Z',up_axis='Y',export_triangulated_mesh=True,export_materials=True,path_mode='RELATIVE')
 report['sourceTriangles']=source_count;report['levels'].append({'level':level,'path':out.relative_to(ASSETS).as_posix(),'triangles':count})
 print('SIGNAL_LOD',level,count,flush=True)
p=ASSETS/'lod/manifest.json';manifest=json.loads(p.read_text(encoding='utf-8'))
manifest=[report if e['source']==report['source'] else e for e in manifest];p.write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
