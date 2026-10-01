from pathlib import Path
import subprocess,re
root=Path(__file__).resolve().parent.parent
out=root/'App/shaders/glsl';out.mkdir(exist_ok=True)
tmp=root/'build-linux/shader-intermediates';tmp.mkdir(parents=True,exist_ok=True)
jobs={'city_forward':{'frag':['Depth_PS','Shading_PS','Terrain_PS','Earth_PS','Aggregate_PS','Field_PS','Paddy_PS','Foliage_PS','River_PS','Pavement_PS','Asphalt_PS','Building_PS','VehicleInstance_PS','VehiclePaint_PS']},'tree_instances':{'vert':['NearVS','FarVS']},'vehicle_instances':{'vert':['VS']},'city_cable':{'vert':['Cable_VS'],'frag':['Cable_PS']},'selection_outline':{'frag':['PS']}}
for stem,stages in jobs.items():
 source=(root/f'App/shaders/hlsl/{stem}.hlsl').read_text(encoding='utf-8-sig')
 header=[]
 for line in source.splitlines():
  if line.strip() and not line.lstrip().startswith('//'):break
  header.append(line)
 if 'namespace s3d' in source:
  a=source.index('namespace s3d'); b=source.index('{',a);depth=1;e=b+1
  while depth:
   depth+=(source[e]=='{')-(source[e]=='}');e+=1
  source=source[:a]+source[b+1:e-1]+source[e:]
  source=source.replace('s3d::','')
 clean=tmp/f'{stem}.hlsl';clean.write_text(source)
 for stage,entries in stages.items():
  for entry in entries:
   spv=tmp/f'{stem}_{entry}.spv';glsl=out/f'{stem}_{entry}.{stage}'
   subprocess.run(['glslangValidator','-D','-V','-S',stage,'-e',entry,'--auto-map-locations','--hlsl-iomap',str(clean),'-o',str(spv)],check=True)
   subprocess.run(['spirv-cross',str(spv),'--version','410','--no-420pack-extension','--output',str(glsl)],check=True)
   s=glsl.read_text()
   sampler_slots={'g_texture0':0,'g_shadowMap':1,'g_coastalSand':2,'g_dynamicShadowMap':3,'g_earthNormal':4}
   for name,slot in sampler_slots.items():
    s=re.sub(r'SPIRV_Cross_Combined'+name+r'\w+',f'Texture{slot}',s)
   # Siv3D transposes its built-in vertex matrices before the GL UBO upload.
   if stage=='vert':
    for block in ('VSPerView','VSPerObject'):
     s=s.replace(f'layout(std140) uniform {block}',f'layout(std140, row_major) uniform {block}')
   # The custom shadow projection retains D3D [0,1] clip Z on Linux.
   if entry=='Depth_PS':s=s.replace('gl_FragCoord.z','(gl_FragCoord.z * 2.0 - 1.0)')
   glsl.write_text('// Generated from the matching HLSL entry point by chore/generate_linux_shaders.py.\n'+'\n'.join(header)+'\n'+s)
   subprocess.run(['glslangValidator','-S',stage,str(glsl)],check=True)
print('Validated all 20 GLSL shaders')
