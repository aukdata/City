"""Generate editor schemas and check the external generation defaults.

The C++ catalog owns types/ranges; values live only in assets/generation.
Run from the repository root: python scripts/generation_schema.py [--check]
"""
import argparse
import json
import math
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'App/assets/generation'


def schemas():
    result = {}
    catalog = (ROOT / 'src/gen/GenerationSettings.def').read_text(encoding='utf-8-sig')
    for kind, group, key, lower, upper in re.findall(
            r'GENERATION_SETTING\((\w+), \w+, U"([^"]+)", U"([^"]+)", ([^,]+), ([^)]+)\)', catalog):
        schema = result.setdefault(group, {
            '$schema': 'https://json-schema.org/draft/2020-12/schema',
            'title': f'Pavecity generation: {group}', 'type': 'object',
            'additionalProperties': False, 'required': [], 'properties': {}})
        schema['required'].append(key)
        schema['properties'][key] = {
            'type': 'number' if kind in ('double', 'float') else 'integer',
            'minimum': float(lower), 'maximum': float(upper)}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    count = 0
    for group, schema in schemas().items():
        values = json.loads((ASSETS / f'{group}.json').read_text(encoding='utf-8'))
        assert set(values) == set(schema['required']), f'{group}: missing or unknown keys'
        for key, value in values.items():
            rule = schema['properties'][key]
            assert type(value) in (int, float) and math.isfinite(value), f'{group}.{key}: not a finite number'
            assert rule['minimum'] <= value <= rule['maximum'], f'{group}.{key}: outside range'
            assert rule['type'] != 'integer' or int(value) == value, f'{group}.{key}: not an integer'
        output = json.dumps(schema, ensure_ascii=False, indent=2) + '\n'
        path = ASSETS / 'schema' / f'{group}.schema.json'
        if args.check:
            assert path.read_text(encoding='utf-8') == output, f'{path}: stale schema'
        else:
            path.parent.mkdir(exist_ok=True)
            path.write_text(output, encoding='utf-8')
        count += len(values)
    catalog = (ROOT / 'src/gen/GenerationSettings.def').read_text(encoding='utf-8-sig')
    fields = set(re.findall(r'GENERATION_SETTING\(\w+, (\w+),', catalog))
    used = set()
    for path in (ROOT / 'src').rglob('*'):
        if path.suffix not in ('.cpp', '.hpp', '.h'):
            continue
        source = path.read_text(encoding='utf-8-sig')
        references = set(re.findall(r'GenerationSettings::get\(\)\.(\w+)', source))
        assert references <= fields, f'{path}: unknown settings {references - fields}'
        used |= fields & set(re.findall(r'\b\w+\b', source))
    assert fields <= used, f'Unused external settings: {fields - used}'
    print(f'{count} settings: all defaults, schemas and references valid')
    from urban_structure_schema import check
    check(args.check)


if __name__ == '__main__':
    main()
