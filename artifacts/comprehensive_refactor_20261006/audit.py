from pathlib import Path
import collections
import hashlib
import json
import re
ROOT = Path.cwd()
files = [p for d in ('src', 'Test', 'chore', 'scripts', 'tools', 'App/shaders') for p in Path(d).rglob('*') if p.suffix in ('.cpp', '.hpp', '.h', '.py', '.def', '.ps1', '.hlsl') and not (p.parts[0] == 'Test' and 'App' in p.parts)]
files += [Path(p) for p in ('Main.cpp', 'stdafx.cpp', 'stdafx.h')]
inventory = []
blocks = collections.defaultdict(list)
for path in sorted(files):
    data = path.read_bytes()
    source = data.decode('utf-8-sig')
    lines = source.splitlines()
    rel = path.as_posix()
    includes = re.findall(r'^\s*#\s*include\s+["<]([^">]+)', source, re.M)
    declarations = [line.strip() for line in lines if re.match(r'^(?:class|struct|enum class|namespace|[\w:<>,*& ]+\w+::\w+\()', line)]
    findings = [[i, line.strip()] for i, line in enumerate(lines,1) if re.search(r'Console\s*<<|\bPrint\s*<<|\b(?:TODO|FIXME)\b|catch\s*\(|\.detach\(|\b(?:new|delete)\b|\bstatic\s+(?!constexpr|const)', line)]
    inventory.append(dict(path=rel, lines=len(lines), sha256=hashlib.sha256(data).hexdigest(), encoding_ok=path.suffix not in ('.cpp','.hpp','.h') or (data.startswith(b'\xef\xbb\xbf') and data.count(b'\n')==data.count(b'\r\n')), includes=includes, declarations=declarations, findings=findings))
    if path.suffix in ('.cpp','.hpp'):
        for start in range(len(lines)-11):
            block = '\n'.join(line.strip() for line in lines[start:start+12])
            if len(block)>260 and block.count(';')>=4:
                blocks[block].append([rel,start+1])
duplicates = [dict(code=block,locations=loc) for block,loc in blocks.items() if len({p for p,i in loc})>1]
output=Path('artifacts/comprehensive_refactor_20261006')
(output/'inventory.json').write_text(json.dumps(inventory,ensure_ascii=False,indent=2),encoding='utf-8')
(output/'duplicates.json').write_text(json.dumps(duplicates,ensure_ascii=False,indent=2),encoding='utf-8')
(output/'inventory.tsv').write_text('path\tlines\tsha256\n' + ''.join(f"{item['path']}\t{item['lines']}\t{item['sha256']}\n" for item in inventory), encoding='utf-8')
print('FILES',len(inventory),'LINES',sum(i['lines'] for i in inventory))
print('DIRECTORIES',dict(collections.Counter({d:sum(i['lines'] for i in inventory if str(Path(i['path']).parent)==d) for d in sorted({str(Path(i['path']).parent) for i in inventory})})))
print('LARGEST',[(i['path'],i['lines']) for i in sorted(inventory,key=lambda i:-i['lines'])[:30]])
print('ENCODING',[i['path'] for i in inventory if not i['encoding_ok']])
print('DUPLICATE WINDOWS',len(duplicates))
