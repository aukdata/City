"""Fetch three CC0 albedo textures; preserve source, author, URL and hashes.
Powered by Poly Haven (https://polyhaven.com).
"""
from pathlib import Path
import hashlib
import json
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
HEADERS = {'User-Agent': 'CityAssetAuthoring/1.0'}

def fetch(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=HEADERS), timeout=60) as response:
        return response.read()

def main():
    catalog = json.loads(fetch('https://api.polyhaven.com/assets?t=textures'))
    records = []
    for name in ['grey_plaster_02', 'wood_planks_grey', 'brick_wall_11']:
        folder = ROOT / 'App/assets/third_party/polyhaven' / name
        folder.mkdir(parents=True, exist_ok=True)
        metadata = json.loads(fetch(f'https://api.polyhaven.com/files/{name}'))
        entry = metadata.get('diff', metadata.get('Diffuse'))['1k']['jpg']
        path = folder / entry['url'].rsplit('/', 1)[-1]
        data = path.read_bytes() if path.exists() else fetch(entry['url'])
        assert hashlib.md5(data).hexdigest() == entry['md5'], name
        path.write_bytes(data)
        record = dict(asset=name, authors=catalog[name]['authors'],
                      source=f'https://polyhaven.com/a/{name}', download=entry['url'],
                      license='CC0 1.0', license_url='https://polyhaven.com/license',
                      legal_text='https://creativecommons.org/publicdomain/zero/1.0/legalcode',
                      retrieved='2026-09-11', sha256=hashlib.sha256(data).hexdigest(),
                      file=str(path.relative_to(ROOT)).replace('\\', '/'),
                      processing='Original JPG unchanged; albedo tiled at metre scale and baked with geometry AO into per-building diffuse maps.')
        (folder/'SOURCE.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
        records.append(record)
        print(name, len(data), record['sha256'])
    (ROOT/'App/assets/licenses/REALISTIC_MATERIALS.json').write_text(json.dumps(records, indent=2)+'\n', encoding='utf-8')

if __name__ == '__main__':
    main()
