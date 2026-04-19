#!/usr/bin/env python3
"""
案内標識（方面及び距離/方面及び方向）用のポール OBJ を生成する。

出力: App/assets/signs/guide/guide_pole.obj

形状: 現物を踏襲した 2 本柱フレーム構造
- 左右 2 本のメイン円柱（radius=0.075m, 16 segments, y=0.03 → y=1.00）
- ベースプレート（各柱に 1 枚ずつ、y=0 → y=0.03、octagon radius 0.14m）
- トップクロスビーム（左右の柱を y=0.92 付近で繋ぐ水平の太い棒、断面 0.10×0.10m）

座標系: 原点はポール接地中央（地面）。
 - X: 柱間方向（左柱 = -0.5m, 右柱 = +0.5m）
 - Y: 上向き（1.0m で単位化。実行時に Scale(1, poleHeight, 1) で伸縮）
 - Z: 看板の前後（看板はローカル +Z 側に向く）

注意: ポール高さ方向（Y）のみ Scale で伸ばされるため、ベースの厚み等は
ある程度伸びるが、設計時に小さめ（0.03m）にしておくことで
3〜4 倍スケール後も自然なサイズ感を保つ。
"""

import math
from pathlib import Path

OUT_OBJ = "/mnt/d/Users/Takuma/Creations/codes/City/App/assets/signs/guide/guide_pole.obj"

# ── 寸法定数（local 単位。Y は 0〜1 で単位化、X/Z は実 [m]）──

POST_RADIUS     = 0.075   # 円柱半径 [m]
POST_SEGMENTS   = 16      # 円周分割数
POST_SPAN_HALF  = 0.5     # 柱間距離の半分 [m]（中心から±0.5 m の 2 本柱）

BASE_Y_LO       = 0.00
BASE_Y_HI       = 0.03
BASE_RADIUS     = 0.14    # ベースプレート半径 [m]（octagon）
BASE_SEGMENTS   = 8

POST_Y_LO       = BASE_Y_HI
POST_Y_HI       = 1.00

BEAM_Y_CTR      = 0.93    # クロスビーム中心 Y
BEAM_HEIGHT     = 0.10    # クロスビーム高さ
BEAM_THICKNESS  = 0.10    # クロスビーム奥行き (Z 方向)


def main():
    verts = []       # (x, y, z)
    normals = []     # (nx, ny, nz)
    faces = []       # [(v_idx, n_idx), ...] のリスト（1-origin）

    # 法線ライブラリ (Y+, Y-, X+, X-, Z+, Z- 等を固定で用意)
    N_YP = add_normal(normals, (0, 1, 0))
    N_YN = add_normal(normals, (0, -1, 0))

    # ── 1. 2本の円柱ポスト ──
    for side in (-1, 1):
        cx = side * POST_SPAN_HALF
        add_cylinder(verts, normals, faces,
                     cx=cx, cz=0.0,
                     y_lo=POST_Y_LO, y_hi=POST_Y_HI,
                     radius=POST_RADIUS, segments=POST_SEGMENTS,
                     close_top=True, close_bottom=False, n_yp=N_YP)

    # ── 2. ベースプレート（各柱の接地部、octagon 厚板）──
    for side in (-1, 1):
        cx = side * POST_SPAN_HALF
        add_cylinder(verts, normals, faces,
                     cx=cx, cz=0.0,
                     y_lo=BASE_Y_LO, y_hi=BASE_Y_HI,
                     radius=BASE_RADIUS, segments=BASE_SEGMENTS,
                     close_top=True, close_bottom=True, n_yp=N_YP, n_yn=N_YN)

    # ── 3. トップクロスビーム（左右の柱を繋ぐ水平の太い棒）──
    beam_y_lo = BEAM_Y_CTR - BEAM_HEIGHT * 0.5
    beam_y_hi = BEAM_Y_CTR + BEAM_HEIGHT * 0.5
    beam_x_lo = -POST_SPAN_HALF - POST_RADIUS * 0.5
    beam_x_hi = +POST_SPAN_HALF + POST_RADIUS * 0.5
    beam_z_lo = -BEAM_THICKNESS * 0.5
    beam_z_hi = +BEAM_THICKNESS * 0.5
    add_box(verts, normals, faces,
            beam_x_lo, beam_y_lo, beam_z_lo,
            beam_x_hi, beam_y_hi, beam_z_hi)

    # ── OBJ 書き出し ──
    lines = [
        "# Guide sign pole (2-post frame)",
        f"# Span={POST_SPAN_HALF*2}m PostRadius={POST_RADIUS}m Height=1.0 (scale Y at runtime)",
        "o GuidePole",
    ]
    for (x, y, z) in verts:
        lines.append(f"v {x:.6f} {y:.6f} {z:.6f}")
    for (nx, ny, nz) in normals:
        lines.append(f"vn {nx:.6f} {ny:.6f} {nz:.6f}")
    for (v_idx, n_idx) in faces:
        a, b, c = v_idx
        lines.append(f"f {a}//{n_idx} {b}//{n_idx} {c}//{n_idx}")

    Path(OUT_OBJ).parent.mkdir(parents=True, exist_ok=True)
    Path(OUT_OBJ).write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"書き出し: {OUT_OBJ}")
    print(f"頂点数={len(verts)}, 法線={len(normals)}, 面={len(faces)}")


# ── ヘルパー ──

def add_normal(normals, n):
    """法線を追加し (1-origin) インデックスを返す。"""
    nx, ny, nz = n
    length = math.sqrt(nx*nx + ny*ny + nz*nz) or 1.0
    normals.append((nx/length, ny/length, nz/length))
    return len(normals)


def add_cylinder(verts, normals, faces, cx, cz, y_lo, y_hi,
                 radius, segments, close_top=False, close_bottom=False,
                 n_yp=None, n_yn=None):
    """円柱を追加する。side surface + optional top/bottom caps。"""
    # 側面頂点を先に追加: bottom ring, top ring
    bottom_start = len(verts) + 1  # 1-origin
    side_normal_indices = []
    for i in range(segments):
        theta = i * 2 * math.pi / segments
        x = cx + radius * math.cos(theta)
        z = cz + radius * math.sin(theta)
        verts.append((x, y_lo, z))
    for i in range(segments):
        theta = i * 2 * math.pi / segments
        x = cx + radius * math.cos(theta)
        z = cz + radius * math.sin(theta)
        verts.append((x, y_hi, z))
    # 側面の法線: 外向き（円周方向）
    for i in range(segments):
        theta = (i + 0.5) * 2 * math.pi / segments   # 面の中央方向
        n_idx = add_normal(normals, (math.cos(theta), 0, math.sin(theta)))
        side_normal_indices.append(n_idx)

    # 側面フェース
    for i in range(segments):
        a = bottom_start + i
        b = bottom_start + (i + 1) % segments
        c = bottom_start + segments + (i + 1) % segments
        d = bottom_start + segments + i
        # quad (a, b, c, d) を 2 triangles に: CW（Siv3D = DirectX は CW 表面）
        # 外側から見て CW: a → d → c → b（外面）
        n_idx = side_normal_indices[i]
        faces.append(((a, d, c), n_idx))
        faces.append(((a, c, b), n_idx))

    # Top cap
    if close_top and n_yp is not None:
        top_start = bottom_start + segments
        for i in range(1, segments - 1):
            a = top_start
            b = top_start + i
            c = top_start + i + 1
            # +Y 法線、上から見て CW（= DirectX 表面）
            faces.append(((a, b, c), n_yp))

    # Bottom cap
    if close_bottom and n_yn is not None:
        for i in range(1, segments - 1):
            a = bottom_start
            b = bottom_start + i + 1
            c = bottom_start + i
            # -Y 法線、下から見て CW
            faces.append(((a, b, c), n_yn))


def add_box(verts, normals, faces, x_lo, y_lo, z_lo, x_hi, y_hi, z_hi):
    """軸平行ボックスを追加する（6 面、各 2 triangles）。"""
    v_start = len(verts) + 1
    # 8 頂点（順序: 000,100,110,010,001,101,111,011）
    corners = [
        (x_lo, y_lo, z_lo),  # 1 000
        (x_hi, y_lo, z_lo),  # 2 100
        (x_hi, y_hi, z_lo),  # 3 110
        (x_lo, y_hi, z_lo),  # 4 010
        (x_lo, y_lo, z_hi),  # 5 001
        (x_hi, y_lo, z_hi),  # 6 101
        (x_hi, y_hi, z_hi),  # 7 111
        (x_lo, y_hi, z_hi),  # 8 011
    ]
    for c in corners:
        verts.append(c)
    v = lambda i: v_start + i  # 0-index for readability

    # 各面の法線とフェース（CW winding from outside）
    n_zp = add_normal(normals, (0, 0, 1))   # +Z 面 (5,6,7,8)
    n_zn = add_normal(normals, (0, 0, -1))  # -Z 面 (1,2,3,4)
    n_xp = add_normal(normals, (1, 0, 0))   # +X 面 (2,6,7,3)
    n_xn = add_normal(normals, (-1, 0, 0))  # -X 面 (1,5,8,4)
    n_yp = add_normal(normals, (0, 1, 0))   # +Y 面 (4,8,7,3)
    n_yn = add_normal(normals, (0, -1, 0))  # -Y 面 (1,5,6,2)

    # +Z 面: outside の観察方向は +Z。そこから見て CW
    faces.append(((v(4), v(5), v(6)), n_zp))  # 5,6,7
    faces.append(((v(4), v(6), v(7)), n_zp))  # 5,7,8
    # -Z 面: outside は -Z 側。そこから見て CW
    faces.append(((v(0), v(2), v(1)), n_zn))  # 1,3,2
    faces.append(((v(0), v(3), v(2)), n_zn))  # 1,4,3
    # +X 面
    faces.append(((v(1), v(2), v(6)), n_xp))  # 2,3,7
    faces.append(((v(1), v(6), v(5)), n_xp))  # 2,7,6
    # -X 面
    faces.append(((v(0), v(4), v(7)), n_xn))  # 1,5,8
    faces.append(((v(0), v(7), v(3)), n_xn))  # 1,8,4
    # +Y 面 (top)
    faces.append(((v(3), v(7), v(6)), n_yp))  # 4,8,7
    faces.append(((v(3), v(6), v(2)), n_yp))  # 4,7,3
    # -Y 面 (bottom)
    faces.append(((v(0), v(1), v(5)), n_yn))  # 1,2,6
    faces.append(((v(0), v(5), v(4)), n_yn))  # 1,6,5


if __name__ == "__main__":
    main()
