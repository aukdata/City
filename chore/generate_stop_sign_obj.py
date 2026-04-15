#!/usr/bin/env python3
"""
sign_198.png（止まれ標識 = 角丸逆三角形）の輪郭から stop_sign.obj を生成する。

出力: App/assets/signs/stop_sign.obj
- ローカル座標: 原点=看板中心, X 右・Y 上・Z 前面法線
- 幅 0.8m に正規化（高さは画像アスペクト比から計算）
- 厚さ 0.02m (front +Z 側と back -Z 側の2面)
- UV: (u, v) = (px_x / img_w, px_y / img_h)
"""

import cv2
import numpy as np
from pathlib import Path

SRC_PNG = "/mnt/d/Users/Takuma/Creations/codes/City/App/assets/signs/stop_sign.png"
OUT_OBJ = "/mnt/d/Users/Takuma/Creations/codes/City/App/assets/signs/stop_sign.obj"

WIDTH_M = 0.8               # 看板横幅 [m]
THICKNESS_M = 0.02          # 厚さ [m]
CONTOUR_EPSILON_RATIO = 0.0008  # approxPolyDP の許容誤差（周長に対する比率）


def main():
    img = cv2.imread(SRC_PNG, cv2.IMREAD_UNCHANGED)
    assert img is not None and img.shape[2] == 4, "透過 PNG を期待"
    h_img, w_img = img.shape[:2]

    alpha = img[:, :, 3]
    # 不透明部分を塗りつぶし、最大輪郭を取得
    _, bin_img = cv2.threshold(alpha, 10, 255, cv2.THRESH_BINARY)
    contours, _ = cv2.findContours(bin_img, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    assert contours, "輪郭が見つからない"
    contour = max(contours, key=cv2.contourArea)

    # 軽く単純化（カーブは保ちつつ冗長点を削減）
    peri = cv2.arcLength(contour, True)
    approx = cv2.approxPolyDP(contour, CONTOUR_EPSILON_RATIO * peri, True)
    pts = approx[:, 0, :]  # (N, 2) 画像ピクセル座標 (x_right, y_down)
    N = len(pts)
    print(f"輪郭頂点数: {N}")

    # bbox とアスペクト比
    x_min, y_min = pts.min(axis=0)
    x_max, y_max = pts.max(axis=0)
    bw = x_max - x_min
    bh = y_max - y_min
    aspect = bh / bw
    height_m = WIDTH_M * aspect
    print(f"bbox: {bw} x {bh} px, aspect={aspect:.3f}, height={height_m:.3f} m")

    # 画像 px → ローカル 3D 座標（中心=0, X右・Y上, Zは厚さ）
    cx_px = 0.5 * (x_min + x_max)
    cy_px = 0.5 * (y_min + y_max)
    scale = WIDTH_M / bw  # 画像1pxあたり [m]

    def to_local(px):
        lx = (px[0] - cx_px) * scale
        ly = -(px[1] - cy_px) * scale   # 画像 y は下向き → 3D y は上向き
        return lx, ly

    # UV は画像全体に対する (u,v) = (px/W, py/H)。
    # Siv3D の UV は一般的に v=0 が上。画像も y=0 が上なので v = py/H で OK。
    def to_uv(px):
        return px[0] / w_img, px[1] / h_img

    verts_front = []  # (x, y, z)
    verts_back = []
    uvs = []
    normals_front = (0.0, 0.0, 1.0)
    normals_back = (0.0, 0.0, -1.0)

    half_t = THICKNESS_M * 0.5
    for p in pts:
        lx, ly = to_local(p)
        u, v = to_uv(p)
        verts_front.append((lx, ly, half_t))
        verts_back.append((lx, ly, -half_t))
        uvs.append((u, v))

    # OBJ 書き出し
    lines = []
    lines.append("# StopSign (rounded triangle, generated from sign_198.png)")
    lines.append(f"# width={WIDTH_M}m height={height_m:.4f}m thickness={THICKNESS_M}m verts={N}")

    # 前面 object
    lines.append("o StopSignFront")
    for (x, y, z) in verts_front:
        lines.append(f"v {x:.6f} {y:.6f} {z:.6f}")
    for (u, v) in uvs:
        lines.append(f"vt {u:.6f} {v:.6f}")
    lines.append(f"vn {normals_front[0]:.1f} {normals_front[1]:.1f} {normals_front[2]:.1f}")
    # 前面: 三角ファン（CCW = 法線 +Z。画像 y は下向きなので 3D y は反転済み → 順序に注意）
    # 前面は +Z を向くので、XY 平面で CCW 順に並べる必要がある。
    # findContours は画像座標で時計回りに輪郭を返す。3D 座標では y を反転したので、
    # 3D の XY 平面では反時計回り（CCW）になっている → そのまま fan でOK。
    for i in range(1, N - 1):
        # v/vt/vn インデックスは1始まり
        a = 1
        b = i + 1
        c = i + 2
        lines.append(f"f {a}/{a}/1 {b}/{b}/1 {c}/{c}/1")

    # 背面 object（法線 -Z、巻き順逆）
    lines.append("o StopSignBack")
    v_off = N   # すでに前面で N 個の v/vt 出力済み
    for (x, y, z) in verts_back:
        lines.append(f"v {x:.6f} {y:.6f} {z:.6f}")
    for (u, v) in uvs:
        lines.append(f"vt {u:.6f} {v:.6f}")
    lines.append(f"vn {normals_back[0]:.1f} {normals_back[1]:.1f} {normals_back[2]:.1f}")
    for i in range(1, N - 1):
        a = v_off + 1
        b = v_off + i + 2
        c = v_off + i + 1
        # vn は2個目 (前面=1, 背面=2)
        lines.append(f"f {a}/{a}/2 {b}/{b}/2 {c}/{c}/2")

    Path(OUT_OBJ).write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"書き出し: {OUT_OBJ}")


if __name__ == "__main__":
    main()
