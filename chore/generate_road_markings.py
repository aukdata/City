#!/usr/bin/env python3
"""
路面標示矢印 TOML を reference/204.ht{1,2,3}.gif から直接抽出して生成する。

- OpenCV で最大白成分を輪郭抽出 → approxPolyDP で頂点削減
- 画像座標 (tip=低x, tail=高x) → TOML (+x=前方, +y=右)
- 左右反転で Right / StraightRight を生成

仕様:
- 全長 5m (bbox 幅 → 5m)
- TOML 頂点は math CCW (shoelace 正) — Siv3D Polygon の outer 順序

出力: App/assets/road_markings/{straight,left,right,straightleft,straightright}.toml
"""

import cv2
import numpy as np
from pathlib import Path

REF = Path("/mnt/d/Users/Takuma/Creations/codes/City/reference")
OUT = Path("/mnt/d/Users/Takuma/Creations/codes/City/App/assets/road_markings")

ARROW_TOTAL_LENGTH_M = 5.0

# 各 reference 画像での approxPolyDP 許容誤差（周長比）
EPS = {
    "204.ht1.gif": 0.002,   # Left: 11 頂点
    "204.ht2.gif": 0.002,   # Straight: 7 頂点
    "204.ht3.gif": 0.003,   # StraightLeft: 14 頂点
}


def extract_arrow_mask(path: Path):
    """画像から矢印マスクと bbox を抽出。画像境界に接する成分は除外。"""
    img = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY) if len(img.shape) == 3 else img
    bw = (gray == 255).astype(np.uint8)
    num, labels, stats, _ = cv2.connectedComponentsWithStats(bw)
    H, W = gray.shape
    best = None
    for i in range(1, num):
        x, y, w, h, area = stats[i]
        if x < 20 or y < 20 or x + w > W - 20 or y + h > H - 20:
            continue
        if best is None or area > stats[best][4]:
            best = i
    mask = (labels == best).astype(np.uint8) * 255
    return mask, stats[best]


def measure_shaft_center(mask, stat):
    """bbox 右端列 (=tail 側) の白画素範囲から shaft 中心 y を求める。"""
    bx, by, bw, bh, _ = stat
    col = bx + bw - 1
    ys = np.where(mask[:, col] > 0)[0]
    return float((ys.min() + ys.max()) / 2)


def extract_polygon(name: str):
    """reference/name から TOML 頂点 (CCW) を抽出して返す。"""
    mask, stat = extract_arrow_mask(REF / name)
    bx, by, bw, bh, _ = stat
    contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    c = max(contours, key=cv2.contourArea)
    peri = cv2.arcLength(c, True)
    approx = cv2.approxPolyDP(c, EPS[name] * peri, True)
    pts = approx[:, 0, :]  # (N, 2) image coords

    shaft_y = measure_shaft_center(mask, stat)
    # image (x, y) → TOML (X, Y)
    # X = +2.5 at image_x=bx ; X = -2.5 at image_x=bx+bw-1
    # Y = -(image_y - shaft_y) * scale (下方向=TOML -y=左)
    scale = ARROW_TOTAL_LENGTH_M / (bw - 1)
    verts = []
    for (ix, iy) in pts:
        X = +2.5 - (ix - bx) * scale
        Y = -(iy - shaft_y) * scale
        verts.append((X, Y))

    # CCW (shoelace > 0) を保証
    if shoelace_2x(verts) < 0:
        verts = list(reversed(verts))
    return verts


def shoelace_2x(verts):
    s = 0.0
    n = len(verts)
    for i in range(n):
        x0, y0 = verts[i]
        x1, y1 = verts[(i + 1) % n]
        s += x0 * y1 - x1 * y0
    return s


def mirror_y(verts):
    """y 反転 + 順序反転で CCW 保持"""
    return [(x, -y) for x, y in reversed(verts)]


def write_toml(id_str: str, verts, filename=None):
    sl = shoelace_2x(verts)
    assert sl > 0, f"{id_str}: shoelace <= 0 (CW). sl={sl}"
    fn = filename or f"{id_str.lower()}.toml"
    path = OUT / fn
    lines = [f'id = "{id_str}"', "verts = ["]
    for x, y in verts:
        lines.append(f"    [{x:.4f}, {y:.4f}],")
    lines.append("]")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {path} (verts={len(verts)}, area={sl*0.5:.4f})")


def main():
    OUT.mkdir(parents=True, exist_ok=True)

    v_straight       = extract_polygon("204.ht2.gif")
    v_left           = extract_polygon("204.ht1.gif")
    v_straight_left  = extract_polygon("204.ht3.gif")

    write_toml("Straight",       v_straight)
    write_toml("Left",           v_left)
    write_toml("Right",          mirror_y(v_left))
    write_toml("StraightLeft",   v_straight_left)
    write_toml("StraightRight",  mirror_y(v_straight_left))


if __name__ == "__main__":
    main()
