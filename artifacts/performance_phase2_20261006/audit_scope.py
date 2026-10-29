from pathlib import Path
import subprocess, json, hashlib, xml.etree.ElementTree as ET
root=Path.cwd()
git=['git','-c','safe.directory='+root.as_posix()]
changed=subprocess.check_output(git+['diff','--name-only','--','src','Test','City.vcxproj','City.vcxproj.filters','plan/29_code_architecture.md','ISSUE.md'],encoding='utf-8').splitlines()
new=['src/render/MeshUploadSlices.hpp','src/render/RoadRenderer_Async.cpp','src/world/HeightSnapshot.hpp']
scope_file=Path('artifacts/performance_phase2_20261006/source_scope.json')
previous=json.loads(scope_file.read_text(encoding='utf-8')) if scope_file.exists() else {}
paths=sorted(set(changed+new+list(previous)))
for name in paths:
    p=Path(name);b=p.read_bytes()
    if p.suffix.lower() in ['.cpp','.hpp','.h']:
        assert b.startswith(b'\xef\xbb\xbf'),name
        assert b'\n' not in b.replace(b'\r\n',b''),name
        assert all(line.rstrip(b' \t')==line for line in b.split(b'\r\n')),name
ns={'ms':'http://schemas.microsoft.com/developer/msbuild/2003'}
for name in ['City.vcxproj','Test/Test.vcxproj']:
    tree=ET.parse(name)
    for tag in ['ClCompile','ClInclude']:
        entries=[node.get('Include') for node in tree.findall('.//ms:'+tag,ns) if node.get('Include')]
        assert len(entries)==len(set(entries)),(name,tag,'duplicate project entries')
    for ending in ['RoadRenderer_Async.cpp','MeshUploadSlices.hpp','HeightSnapshot.hpp']:
        assert sum((node.get('Include') or '').endswith(ending) for node in tree.iter())==1,(name,ending)
        assert sum((node.get('Include') or '').endswith(ending) for node in ET.parse(name+'.filters').iter())==1,(name,ending,'IDE filter')
out={name:hashlib.sha256(Path(name).read_bytes()).hexdigest() for name in paths}
Path('artifacts/performance_phase2_20261006/source_scope.json').write_text(json.dumps(out,indent=2),encoding='utf-8')
print('Audited',len(paths),'task files: encoding, line endings, whitespace, project entries and IDE filters passed')
