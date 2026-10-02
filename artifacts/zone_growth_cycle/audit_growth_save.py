"""Read-only complete development/road/terrain save comparison for native growth QA."""
from pathlib import Path
import hashlib
import json
import struct
import sys


class Reader:
    def __init__(self, path):
        self.data = path.read_bytes()
        self.pos = 0

    def read(self, fmt):
        result = struct.unpack_from('<' + fmt, self.data, self.pos)
        self.pos += struct.calcsize('<' + fmt)
        return result


def snapshot(root):
    r = Reader(root / 'global/development.bin')
    header = r.read('IHHIHHI')
    _, _, _, _, _, zone_cells, count = header
    chunks, zones, buildings, patches = {}, {}, {}, {}
    for _ in range(count):
        cx, cz, urban, zone_count, building_count, patch_count = r.read('HHBIII')
        chunks[f'{cx},{cz}'] = urban
        for _ in range(zone_count):
            cell, kind = r.read('HB')
            zones[f'{cx * zone_cells + cell % zone_cells},{cz * zone_cells + cell // zone_cells}'] = kind
        for _ in range(building_count):
            cell, kind, built, angle, edge, t, ox, oz = r.read('HBdfifff')
            buildings[f'{cx * zone_cells + cell % zone_cells},{cz * zone_cells + cell // zone_cells}'] = {
                'type': kind, 'builtAt': built, 'angle': angle, 'edge': edge,
                'edgeT': t, 'offsetX': ox, 'offsetZ': oz,
            }
        for index in range(patch_count):
            ident, kind, elevation, variant, key, vertex_count = r.read('iBfIqI')
            patches[f'{cx},{cz}:{index}'] = {
                'id': ident, 'type': kind, 'elevationOffset': elevation,
                'materialVariant': variant, 'sourceParcelKey': key,
                'polygon': [r.read('dd') for _ in range(vertex_count)],
            }
    assert r.pos == len(r.data), (r.pos, len(r.data))
    return {'header': header, 'chunks': chunks, 'zones': zones,
            'buildings': buildings, 'patches': patches}


def changes(first, second):
    return [{'key': k, 'before': first.get(k), 'after': second.get(k)}
            for k in sorted(first.keys() | second.keys()) if first.get(k) != second.get(k)]


def json_changes(first, second, path=''):
    if type(first) is not type(second):
        return [{'path': path, 'before': first, 'after': second}]
    if isinstance(first, dict):
        result = []
        for key in sorted(first.keys() | second.keys()):
            result.extend(json_changes(first.get(key), second.get(key), path + '/' + key))
        return result
    if isinstance(first, list):
        result = []
        for index in range(max(len(first), len(second))):
            result.extend(json_changes(first[index] if index < len(first) else None,
                                       second[index] if index < len(second) else None,
                                       path + '/' + str(index)))
        return result
    return [] if first == second else [{'path': path, 'before': first, 'after': second}]


def summarize_json_changes(first, second):
    result = []
    for item in changes(first, second):
        leaf = json_changes(item['before'], item['after'], '/' + item['key'])
        result.append({'field': item['key'], 'leafDifferenceCount': len(leaf),
                       'firstDifferences': leaf[:10], 'truncated': len(leaf) > 10})
    return result


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compare(first_root, second_root):
    first, second = snapshot(first_root), snapshot(second_root)
    result = {'before': str(first_root), 'after': str(second_root),
              'headerEqual': first['header'] == second['header']}
    for kind in ('chunks', 'zones', 'buildings', 'patches'):
        diffs = changes(first[kind], second[kind])
        result[kind] = {'beforeCount': len(first[kind]), 'afterCount': len(second[kind]),
                        'changedCount': len(diffs), 'changes': diffs}
    result['roadNetworkBytesEqual'] = ((first_root / 'global/roads.bin').read_bytes()
                                       == (second_root / 'global/roads.bin').read_bytes())
    files_first = {p.relative_to(first_root) for p in (first_root / 'chunks').glob('*/terrain.bin')}
    files_second = {p.relative_to(second_root) for p in (second_root / 'chunks').glob('*/terrain.bin')}
    terrain_diffs = [str(p) for p in sorted(files_first | files_second)
                     if p not in files_first or p not in files_second
                     or (first_root / p).read_bytes() != (second_root / p).read_bytes()]
    result['terrain'] = {'beforeCount': len(files_first), 'afterCount': len(files_second),
                         'changedCount': len(terrain_diffs), 'changedFiles': terrain_diffs}
    for name in ('global/development.bin', 'global/roads.bin'):
        result.setdefault('sha256', {})[name] = {'before': sha256(first_root / name),
                                                'after': sha256(second_root / name)}
    files_first = {p.relative_to(first_root) for p in first_root.rglob('*') if p.is_file()}
    files_second = {p.relative_to(second_root) for p in second_root.rglob('*') if p.is_file()}
    file_diffs = []
    for name in sorted(files_first | files_second):
        before_hash = sha256(first_root / name) if name in files_first else None
        after_hash = sha256(second_root / name) if name in files_second else None
        if before_hash != after_hash:
            item = {'path': str(name), 'beforeSha256': before_hash, 'afterSha256': after_hash}
            if name.suffix == '.json' and before_hash and after_hash:
                first_json = json.loads((first_root / name).read_text(encoding='utf-8-sig'))
                second_json = json.loads((second_root / name).read_text(encoding='utf-8-sig'))
                if isinstance(first_json, dict) and isinstance(second_json, dict):
                    item['changedTopLevelFields'] = summarize_json_changes(first_json, second_json)
            file_diffs.append(item)
    result['allSaveFiles'] = {'beforeCount': len(files_first), 'afterCount': len(files_second),
                              'changedCount': len(file_diffs), 'changes': file_diffs}
    return result


if __name__ == '__main__':
    if len(sys.argv) not in (3, 4):
        raise SystemExit('Usage: audit_growth_save.py BEFORE_SAVE AFTER_SAVE [OUTPUT_JSON]')
    output = json.dumps(compare(Path(sys.argv[1]), Path(sys.argv[2])), indent=2) + '\n'
    if len(sys.argv) == 4:
        Path(sys.argv[3]).write_text(output)
    else:
        print(output, end='')
