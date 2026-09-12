"""Record the ground-floor facade plane, excluding projecting roofs/balconies.

Read original OBJ geometry once, then store the result in each asset's TOML.
Runtime placement uses this plane instead of the model's overall bounding box.
"""
from collections import defaultdict
from pathlib import Path
import math
import re

ROOT = Path(__file__).resolve().parents[1]


def facade_plane(path):
    vertices = []
    areas = defaultdict(float)
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        if line.startswith("v "):
            vertices.append(tuple(map(float, line.split()[1:4])))
        elif line.startswith("f "):
            indices = [int(part.split('/')[0]) for part in line.split()[1:]]
            points = [vertices[index - 1 if index > 0 else index] for index in indices]
            for i in range(1, len(points) - 1):
                a, b, c = points[0], points[i], points[i + 1]
                ab = [b[j] - a[j] for j in range(3)]
                ac = [c[j] - a[j] for j in range(3)]
                normal = [ab[1]*ac[2]-ab[2]*ac[1], ab[2]*ac[0]-ab[0]*ac[2], ab[0]*ac[1]-ab[1]*ac[0]]
                area = math.sqrt(sum(n*n for n in normal)) * .5
                # Ground-floor wall faces must reach the human-scale band.
                if area < .02 or normal[2] > -1.8 * area:
                    continue
                if min(a[1], b[1], c[1]) > 2.4 or max(a[1], b[1], c[1]) < .7:
                    continue
                z = (a[2] + b[2] + c[2]) / 3
                if z < 0:
                    areas[round(z / .02) * .02] += area
    if not areas:
        return None
    # Window panes and their frames stand slightly proud of the structural wall.
    # Keep significant facade layers, but reject tiny pipes/handles/railing bars.
    threshold = max(areas.values()) * .05
    return min(z for z, area in areas.items() if area >= threshold) - .04


if __name__ == "__main__":
    for folder in ("residential", "commercial"):
        for path in sorted((ROOT / "App/assets/buildings" / folder).glob("*.obj")):
            if path.stem.startswith(("parking_", "factory_")):
                continue
            front = facade_plane(path)
            metadata = path.with_suffix(".toml")
            if front is None or not metadata.exists():
                continue
            raw = metadata.read_bytes().decode("utf-8-sig")
            raw = re.sub(r"^front_wall_z_m\s*=.*\r?\n", "", raw, flags=re.M)
            raw = raw.rstrip() + f"\r\nfront_wall_z_m = {front:.4f}\r\n"
            metadata.write_bytes(raw.encode("utf-8"))
            print(f"{path.stem}: wall={front:.2f}")
