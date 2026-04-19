#!/usr/bin/env python3
"""
national_route.png（おにぎり型=国道標識）の透過アウトラインから onigiri.obj を生成する。

出力: App/assets/signs/guide/onigiri.obj
- ローカル座標: 原点=看板中心, X 右・Y 上・Z 前面法線
- 幅 0.6m に正規化（高さは画像アスペクト比から計算）
- 厚さ 0.02m (前 +Z / 背 -Z の2面)
- UV: (u, v) = (px_x / img_w, px_y / img_h) — national_route.png をそのままテクスチャとして貼れる
"""

import cv2
import numpy as np
from pathlib import Path

SRC_PNG = "/mnt/d/Users/Takuma/Creations/codes/City/App/assets/signs/guide/national_route.png"
OUT_OBJ = "/mnt/d/Users/Takuma/Creations/codes/City/App/assets/signs/guide/onigiri.obj"

WIDTH_M               = 0.6     # 看板横幅 [m]（国道標識実寸基準）
THICKNESS_M           = 0.02    # 厚さ [m]
CONTOUR_EPSILON_RATIO = 0.0008  # approxPolyDP の許容誤差（周長比）


def main():
    img = cv2.imread(SRC_PNG, cv2.IMREAD_UNCHANGED)
    assert img is not None, f"読み込み失敗: {SRC_PNG}"
    assert img.shape[2] == 4, "透過 PNG を期待"
    h_img, w_img = img.shape[:2]

    alpha = img[:, :, 3]
    _, bin_img = cv2.threshold(alpha, 10, 255, cv2.THRESH_BINARY)
    contours, _ = cv2.findContours(bin_img, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    assert contours, "輪郭が見つからない"
    contour = max(contours, key=cv2.contourArea)

    peri = cv2.arcLength(contour, True)
    approx = cv2.approxPolyDP(contour, CONTOUR_EPSILON_RATIO * peri, True)
    pts = approx[:, 0, :]  # (N, 2) 画像 px 座標
    N = len(pts)

    x_min, y_min = pts.min(axis=0)
    x_max, y_max = pts.max(axis=0)
    bw = x_max - x_min
    bh = y_max - y_min
    aspect = bh / bw
    height_m = WIDTH_M * aspect
    print(f"輪郭頂点数={N}, bbox={bw}x{bh}px, aspect={aspect:.3f}, h={height_m:.3f}m")

    cx_px = 0.5 * (x_min + x_max)
    cy_px = 0.5 * (y_min + y_max)
    scale = WIDTH_M / bw

    def to_local(px):
        lx = (px[0] - cx_px) * scale
        ly = -(px[1] - cy_px) * scale   # 画像 y は下向き → 3D y は上向き
        return lx, ly

    def to_uv(px):
        # 看板は driver 側 (local -Z 面) が視認面。u を反転して左右を合わせる
        return 1.0 - px[0] / w_img, px[1] / h_img

    # 片面メッシュ（z=0 平面）。裏面は RoadRenderer の CullFront パスで灰色描画する
    verts_front = []
    uvs         = []
    for p in pts:
        lx, ly = to_local(p)
        u, v   = to_uv(p)
        verts_front.append((lx, ly, 0.0))
        uvs.append((u, v))

    lines = []
    lines.append("# Onigiri (national route sign shape, extracted from national_route.png)")
    lines.append(f"# width={WIDTH_M}m height={height_m:.4f}m verts={N} (single-sided)")

    # 前面 (+Z 法線) — CullBack でテクスチャ描画 / CullFront で灰色描画される
    lines.append("o Onigiri")
    for x, y, z in verts_front:
        lines.append(f"v {x:.6f} {y:.6f} {z:.6f}")
    for u, v in uvs:
        lines.append(f"vt {u:.6f} {v:.6f}")
    lines.append("vn 0.0 0.0 1.0")
    for i in range(1, N - 1):
        a = 1
        b = i + 1
        c = i + 2
        lines.append(f"f {a}/{a}/1 {b}/{b}/1 {c}/{c}/1")

    Path(OUT_OBJ).write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"書き出し: {OUT_OBJ}")


if __name__ == "__main__":
    main()
