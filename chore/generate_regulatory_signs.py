#!/usr/bin/env python3
"""
円形規制標識（303 侵入禁止 / 311 指定方向外進行禁止）のテクスチャおよび共通円形
看板 OBJ を生成する。

出力:
  App/assets/signs/regulatory/no_entry.png              (303: 赤地+白横帯)
  App/assets/signs/regulatory/directional_restriction.png (311: 青地+白上矢印)
  App/assets/signs/regulatory/circle.obj                (直径 0.6m, 厚さ 0.02m)

PNG 仕様:
  512x512, RGBA (背景透過)。円は中央・直径 460px（約 90%）。
OBJ 仕様:
  ローカル座標: 原点=看板中心, X 右・Y 上・Z 前面法線。
  UV: 円の外接正方形を 0..1 にマップ（PNG 全体 1:1 対応）。
"""

import math
import os
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

OUT_DIR = Path("/mnt/d/Users/Takuma/Creations/codes/City/App/assets/signs/regulatory")
IMG_SIZE = 512
DISK_DIAM_PX = 460  # 画像に対する円盤直径（外周白縁を含む）

DIAM_M = 0.6
THICK_M = 0.02
SEGMENTS = 48


def _draw_disk(fill_rgba):
    img = Image.new("RGBA", (IMG_SIZE, IMG_SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx = cy = IMG_SIZE / 2
    r_outer = DISK_DIAM_PX / 2
    # 白縁（太め）
    d.ellipse((cx - r_outer, cy - r_outer, cx + r_outer, cy + r_outer),
              fill=(255, 255, 255, 255))
    # 本体
    r_body = r_outer - 18
    d.ellipse((cx - r_body, cy - r_body, cx + r_body, cy + r_body), fill=fill_rgba)
    return img, cx, cy, r_body


def gen_no_entry():
    img, cx, cy, r = _draw_disk((200, 30, 30, 255))  # 赤
    d = ImageDraw.Draw(img)
    # 中央白横帯（幅 = 直径の 0.7, 高さ = 直径の 0.16）
    bw = r * 2 * 0.7
    bh = r * 2 * 0.16
    d.rectangle((cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2),
                fill=(255, 255, 255, 255))
    path = OUT_DIR / "no_entry.png"
    img.save(path)
    print(f"wrote {path}")


def gen_directional_restriction():
    # 青地に白い上向き矢印（直進のみ）
    img, cx, cy, r = _draw_disk((30, 70, 180, 255))  # 青
    d = ImageDraw.Draw(img)
    # 矢印: 軸長 = 直径 0.7, 矢頭 幅/高さ = 直径 0.4/0.25
    shaft_w = r * 2 * 0.14
    shaft_h = r * 2 * 0.45
    head_w = r * 2 * 0.38
    head_h = r * 2 * 0.28
    # 軸（下半分寄り）
    shaft_top = cy - shaft_h / 2 + head_h * 0.2
    shaft_bot = cy + shaft_h / 2 + head_h * 0.2
    d.rectangle((cx - shaft_w / 2, shaft_top, cx + shaft_w / 2, shaft_bot),
                fill=(255, 255, 255, 255))
    # 矢頭（三角形）— 軸上端の上に乗る
    head_tip_y = shaft_top - head_h
    d.polygon([
        (cx, head_tip_y),
        (cx - head_w / 2, shaft_top),
        (cx + head_w / 2, shaft_top),
    ], fill=(255, 255, 255, 255))
    path = OUT_DIR / "directional_restriction.png"
    img.save(path)
    print(f"wrote {path}")


def gen_circle_obj():
    """直径 DIAM_M の円盤 OBJ (前面+背面) を生成。UV は PNG 全体へ対応。"""
    r_m = DIAM_M * 0.5
    # 画像上の円盤 UV 半径（PNG 全体に対する比）
    uv_r = (DISK_DIAM_PX * 0.5) / IMG_SIZE
    uv_cx = uv_cy = 0.5
    half_t = THICK_M * 0.5

    verts_front = []  # (x, y, z, u, v)
    verts_back = []
    for i in range(SEGMENTS):
        th = (i / SEGMENTS) * 2.0 * math.pi
        cx_m = math.cos(th) * r_m
        cy_m = math.sin(th) * r_m
        u = uv_cx + math.cos(th) * uv_r
        v = uv_cy - math.sin(th) * uv_r  # Siv3D: v=0 上
        verts_front.append((cx_m, cy_m, +half_t, u, v))
        verts_back.append((cx_m, cy_m, -half_t, u, v))

    # 中心頂点（front / back）
    verts_front.append((0.0, 0.0, +half_t, uv_cx, uv_cy))
    verts_back.append((0.0, 0.0, -half_t, uv_cx, uv_cy))

    lines = [
        f"# NoEntry / DirectionalRestriction circular sign",
        f"# diameter={DIAM_M}m thickness={THICK_M}m segments={SEGMENTS}",
        "o RegulatoryCircleFront",
    ]
    # front vertices
    for (x, y, z, _, _) in verts_front:
        lines.append(f"v {x:.6f} {y:.6f} {z:.6f}")
    for (x, y, z, _, _) in verts_back:
        lines.append(f"v {x:.6f} {y:.6f} {z:.6f}")
    for (_, _, _, u, v) in verts_front:
        lines.append(f"vt {u:.6f} {v:.6f}")
    for (_, _, _, u, v) in verts_back:
        lines.append(f"vt {u:.6f} {v:.6f}")
    lines.append("vn 0.0 0.0 1.0")
    lines.append("vn 0.0 0.0 -1.0")

    n_front = len(verts_front)
    center_front_idx = n_front  # 1-based = n_front (0-based last of front)
    # Actually 1-based: front verts 1..n_front, back verts n_front+1..2*n_front
    # center_front = n_front (last front vertex)
    # center_back  = 2*n_front (last back vertex)
    # Use triangle fan
    center_f = n_front  # 1-based
    # Front faces
    lines.append("s off")
    for i in range(SEGMENTS):
        a = i + 1
        b = ((i + 1) % SEGMENTS) + 1
        # 1-based: v/vt/vn
        lines.append(f"f {a}/{a}/1 {b}/{b}/1 {center_f}/{center_f}/1")

    lines.append("o RegulatoryCircleBack")
    # Back faces (reverse winding)
    offset = n_front  # back vertex 1-based start at offset+1
    center_b = 2 * n_front
    for i in range(SEGMENTS):
        a = offset + i + 1
        b = offset + ((i + 1) % SEGMENTS) + 1
        lines.append(f"f {b}/{b}/2 {a}/{a}/2 {center_b}/{center_b}/2")

    path = OUT_DIR / "circle.obj"
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {path}")


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    gen_no_entry()
    gen_directional_restriction()
    gen_circle_obj()


if __name__ == "__main__":
    main()
