"""Compare four fixed overview distances; warm-up and cold peaks stay separate."""
import csv
import json
import math
import statistics
import sys
from pathlib import Path


def summarize(folder):
    with (folder / 'playtest_frames.csv').open(encoding='utf-8-sig', newline='') as stream:
        rows = list(csv.DictReader(stream))
    result = {}
    for phase in range(1, 5):
        group = [row for row in rows if int(row['commandId']) == phase]
        if len(group) < 480:
            raise ValueError(f'Incomplete phase {phase}: {len(group)} frames')
        summary = {'samples': 240, 'warmup_frames': 240}
        for key in ('frameMs', 'renderMs', 'terrainOnlyMs', 'roadMs', 'shadowMs', 'hudStatsMs', 'hudDrawMs', 'buildingDrawCalls', 'gpuMs'):
            values = sorted(float(row[key]) for row in group[240:480])
            if not all(math.isfinite(value) for value in values):
                raise ValueError(f'Non-finite measurement: {key}')
            summary[key] = {'mean': round(statistics.fmean(values), 3), 'p95': values[math.ceil(len(values) * .95) - 1], 'max': values[-1]}
        summary['cold_max_renderMs'] = max(float(row['renderMs']) for row in group)
        summary['over_100ms_render_frames'] = sum(float(row['renderMs']) > 100 for row in group)
        result[group[0]['distance']] = summary
    (folder / 'summary.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return result


if __name__ == '__main__':
    root = Path(__file__).resolve().parent
    for label in sys.argv[1:]:
        result = summarize(root / label)
        for distance, data in result.items():
            print(label, distance, 'frameMs=', data['frameMs'], 'coldMaxRenderMs=', data['cold_max_renderMs'])
