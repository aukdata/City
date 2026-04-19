#!/usr/bin/env python3
"""
RoadSign 用のシンプルな円柱ポール OBJ + JSON を生成する。

出力:
  App/assets/signs/sign_pole.obj / sign_pole.json
  （規制・指示・案内標識の共通ポール。2.5m の円柱）
"""

import json
import math
from pathlib import Path

OUT_DIR = Path("/mnt/d/Users/Takuma/Creations/codes/City/App/assets/signs")

POLE_RADIUS = 0.03
POLE_SEGMENTS = 12
POLE_HEIGHT = 2.5
BOARD_OFFSET_Y = 2.1
BOARD_OFFSET_Z = 0.05


def gen_cylinder_obj(name: str, radius: float, height: float, segments: int) -> str:
	lines = [f"# {name}: cylinder radius={radius}m height={height}m"]
	lines.append(f"o {name}")

	for i in range(segments):
		theta = i * 2 * math.pi / segments
		lines.append(f"v {radius*math.cos(theta):.6f} 0.0 {radius*math.sin(theta):.6f}")
	for i in range(segments):
		theta = i * 2 * math.pi / segments
		lines.append(f"v {radius*math.cos(theta):.6f} {height:.6f} {radius*math.sin(theta):.6f}")

	for i in range(segments):
		theta = (i + 0.5) * 2 * math.pi / segments
		lines.append(f"vn {math.cos(theta):.6f} 0.0 {math.sin(theta):.6f}")
	n_yp = segments + 1
	n_yn = segments + 2
	lines.append("vn 0 1 0")
	lines.append("vn 0 -1 0")

	for i in range(segments):
		a = i + 1
		b = (i + 1) % segments + 1
		c = segments + (i + 1) % segments + 1
		d = segments + i + 1
		n = i + 1
		lines.append(f"f {a}//{n} {d}//{n} {c}//{n}")
		lines.append(f"f {a}//{n} {c}//{n} {b}//{n}")

	top_start = segments + 1
	for i in range(1, segments - 1):
		lines.append(f"f {top_start}//{n_yp} {top_start + i}//{n_yp} {top_start + i + 1}//{n_yp}")
	for i in range(1, segments - 1):
		lines.append(f"f 1//{n_yn} {i + 2}//{n_yn} {i + 1}//{n_yn}")

	return "\n".join(lines) + "\n"


def main():
	OUT_DIR.mkdir(parents=True, exist_ok=True)

	obj = gen_cylinder_obj("SignPole", POLE_RADIUS, POLE_HEIGHT, POLE_SEGMENTS)
	(OUT_DIR / "sign_pole.obj").write_text(obj, encoding="utf-8")

	meta = {
		"_comment": f"RoadSign 共通ポール（{POLE_HEIGHT}m 円柱）のメタデータ。対応 OBJ: sign_pole.obj",
		"_note": "OBJ は実寸 [m] で設計。board は看板中心のポール基底からの相対 3D オフセット [m]",
		"_axes": {
			"X": "道路に対して横方向（車線と直交）",
			"Y": "垂直（上向き）。ポール接地 = 0",
			"Z": "道路の長手方向（driver の進行方向と逆＝手前へ伸ばす軸）"
		},
		"poleHeight": POLE_HEIGHT,
		"board": {
			"offsetX": 0.0,
			"offsetY": BOARD_OFFSET_Y,
			"offsetZ": BOARD_OFFSET_Z
		}
	}
	(OUT_DIR / "sign_pole.json").write_text(
		json.dumps(meta, ensure_ascii=False, indent="\t"), encoding="utf-8")

	# 旧ファイル削除
	for old in ("regulatory_pole.obj", "regulatory_pole.json",
	            "route_pole.obj", "route_pole.json"):
		p = OUT_DIR / old
		if p.exists():
			p.unlink()
			print(f"removed: {old}")

	print(f"Wrote sign_pole.obj + sign_pole.json to {OUT_DIR}")


if __name__ == "__main__":
	main()
