"""Validate the external city-structure catalog and maintain its editor schema."""
import json
import math
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IDS = ('metropolitan', 'historic_grid', 'transit_corridor', 'coastal_hubs',
       'planned_grid', 'constrained_linear', 'regional_hub')


def object_schema(properties):
    return {'type': 'object', 'additionalProperties': False,
            'required': list(properties), 'properties': properties}


def check(check_only=False):
    properties = {'name': {'type': 'string', 'minLength': 1}}
    catalog = (ROOT / 'src/gen/UrbanStructureFields.def').read_text(encoding='utf-8-sig')
    for kind, name, low, high in re.findall(r'URBAN_FIELD\((\w+), (\w+), ([^,]+), ([^)]+)\)', catalog):
        properties[name] = {'type': 'integer' if kind == 'int' else 'number',
                            'minimum': float(low), 'maximum': float(high)}
    center = object_schema({
        'x': {'type': 'number', 'minimum': -.85, 'maximum': .85},
        'z': {'type': 'number', 'minimum': -.85, 'maximum': .85},
        'radius': {'type': 'number', 'minimum': .1, 'maximum': .8},
        'role': {'type': 'integer', 'minimum': 0, 'maximum': 2}, 'rail': {'type': 'boolean'}})
    green = object_schema({key: {'type': 'number', 'minimum': -.95 if key in ('x', 'z') else .01,
                                  'maximum': .95 if key in ('x', 'z') else 1.9}
                           for key in ('x', 'z', 'w', 'h')})
    properties['centers'] = {'type': 'array', 'minItems': 1, 'maxItems': 8, 'items': center}
    properties['greenAreas'] = {'type': 'array', 'maxItems': 16, 'items': green}
    profile = object_schema(properties)
    schema = {'$schema': 'https://json-schema.org/draft/2020-12/schema',
              'title': 'Pavecity urban structures', '$defs': {'profile': profile},
              **object_schema({key: {'$ref': '#/$defs/profile'} for key in IDS})}

    def validate(value, rule, key):
        if '$ref' in rule:
            rule = schema['$defs'][rule['$ref'].rsplit('/', 1)[-1]]
        kind = rule['type']
        if kind == 'object':
            assert type(value) is dict and set(value) == set(rule['required']), key
            for child, item in value.items(): validate(item, rule['properties'][child], f'{key}.{child}')
        elif kind == 'array':
            assert type(value) is list and rule.get('minItems', 0) <= len(value) <= rule['maxItems'], key
            for i, item in enumerate(value): validate(item, rule['items'], f'{key}[{i}]')
        elif kind == 'string': assert type(value) is str and len(value) >= rule['minLength'], key
        elif kind == 'boolean': assert type(value) is bool, key
        else:
            assert type(value) in (int, float) and math.isfinite(value), key
            assert rule['minimum'] <= value <= rule['maximum'], key
            assert kind != 'integer' or int(value) == value, key

    directory = ROOT / 'App/assets/generation'
    values = json.loads((directory / 'urbanStructures.json').read_text(encoding='utf-8'))
    validate(values, schema, 'urbanStructures')
    for key, value in values.items():
        assert value['minimumRelief'] <= value['maximumRelief'], key
        assert sum(value[name] for name in ('coreHighShare', 'coreOfficeShare', 'coreMidShare')) <= 1, key
        assert value['innerDetachedShare'] <= value['outerDetachedShare'], key
        assert any(center['rail'] for center in value['centers']), key
        for green in value['greenAreas']:
            assert green['x'] + green['w'] <= .95 and green['z'] + green['h'] <= .95, key
    output = json.dumps(schema, ensure_ascii=False, indent=2) + '\n'
    path = directory / 'schema/urbanStructures.schema.json'
    if check_only: assert path.read_text(encoding='utf-8') == output, 'Urban structure schema is stale'
    else: path.write_text(output, encoding='utf-8')
    print('7 urban structures: defaults, center layouts and schema valid')


if __name__ == '__main__':
    import sys
    check('--check' in sys.argv)
