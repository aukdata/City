#!/usr/bin/env python3
"""
道路標識一覧ポスターから個別標識を切り出すスクリプト。
Usage:
  python3 extract_signs.py [--preview] [--full]
  --preview : 1/6縮小プレビュー画像を /tmp/preview_*.png に保存
  --full    : 全体実行して signs/ に保存（デフォルト動作）
"""

import cv2
import numpy as np
import os
import sys
from pathlib import Path

INPUT_PATH = "/mnt/d/Users/Takuma/Creations/codes/City/reference/ichiran_600dpi.png"
OUTPUT_DIR = "/mnt/d/Users/Takuma/Creations/codes/City/reference/signs"

# --- パラメータ ---
FLOOD_FILL_TOLERANCE = 60   # floodFill の色差tolerance（罫線を背景に含めるため大きめに設定）
MIN_BBOX = 80               # bbox w,h の最小 (px) ※これ以下は文字等
MAX_BBOX = 3000             # bbox w,h の最大 (px) ※これ以上は帯・タイトル
MIN_AREA = 4000             # 面積最小 (px^2)
MAX_AREA = 4_000_000        # 面積最大
PADDING = 10                # 切り出し時の余白 (px)
MIN_FILL_RATIO = 0.35       # 前景ピクセル比率の最小（タイトル漢字を除外、ひし形・逆三角形は約50%）
TOP_MARGIN = 600            # 画像上部のこの高さ (px) は除外（タイトル行）


def flood_fill_background(gray: np.ndarray, tolerance: int) -> np.ndarray:
    """4隅・辺中点からfloodFillして背景マスクを生成 (255=背景, 0=前景)"""
    h, w = gray.shape
    # floodFill用: 8bit single channel, mask は (h+2, w+2)
    fill_img = gray.copy()
    fill_mask = np.zeros((h + 2, w + 2), np.uint8)

    seeds = [
        (0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1),
        (w // 2, 0), (w // 2, h - 1), (0, h // 2), (w - 1, h // 2),
    ]
    fill_color = 200  # 仮の色
    for sx, sy in seeds:
        if fill_img[sy, sx] > 128:  # 白に近い点のみ
            cv2.floodFill(fill_img, fill_mask, (sx, sy), fill_color,
                          loDiff=tolerance, upDiff=tolerance,
                          flags=cv2.FLOODFILL_FIXED_RANGE)

    # fill_mask の内側 (1:-1, 1:-1) が塗りつぶされた領域
    bg_mask = fill_mask[1:-1, 1:-1] * 255
    return bg_mask


def extract_signs(img_bgr: np.ndarray, scale: float = 1.0,
                  preview: bool = False, output_dir: str = OUTPUT_DIR):
    h_orig, w_orig = img_bgr.shape[:2]
    if scale != 1.0:
        img_bgr = cv2.resize(img_bgr, (int(w_orig * scale), int(h_orig * scale)))

    gray = cv2.cvtColor(img_bgr, cv2.COLOR_BGR2GRAY)

    if preview:
        cv2.imwrite("/tmp/preview_01_gray.png",
                    cv2.resize(gray, (gray.shape[1] // 6, gray.shape[0] // 6)))

    # 1. 外周 flood fill で背景マスク生成（tolerance 大きめで罫線も背景に含める）
    bg_mask = flood_fill_background(gray, FLOOD_FILL_TOLERANCE)

    if preview:
        cv2.imwrite("/tmp/preview_bg.png",
                    cv2.resize(bg_mask, (bg_mask.shape[1] // 6, bg_mask.shape[0] // 6)))

    # 2. 前景マスク
    fg_mask = cv2.bitwise_not(bg_mask)

    if preview:
        cv2.imwrite("/tmp/preview_03_fg_mask.png",
                    cv2.resize(fg_mask, (fg_mask.shape[1] // 6, fg_mask.shape[0] // 6)))

    # 3. 連結成分抽出
    num_labels, labels, stats, _ = cv2.connectedComponentsWithStats(fg_mask, connectivity=8)

    min_bbox = int(MIN_BBOX * scale)
    max_bbox = int(MAX_BBOX * scale)
    min_area = int(MIN_AREA * scale * scale)
    max_area = int(MAX_AREA * scale * scale)
    pad = int(PADDING * scale)

    img_h, img_w = img_bgr.shape[:2]
    sign_count = 0
    results = []

    top_margin = int(TOP_MARGIN * scale)

    for label in range(1, num_labels):
        x, y, w, h, area = stats[label]
        if y < top_margin:
            continue
        if w < min_bbox or h < min_bbox:
            continue
        if w > max_bbox or h > max_bbox:
            continue
        if area < min_area or area > max_area:
            continue

        # アスペクト比フィルタ（極端に細長いものは除外）
        aspect = max(w, h) / max(min(w, h), 1)
        if aspect > 8:
            continue

        # fill ratio フィルタ: タイトル漢字等を除外
        component_mask = (labels[y:y+h, x:x+w] == label).astype(np.uint8)
        fill_ratio = np.sum(component_mask) / (w * h)
        if fill_ratio < MIN_FILL_RATIO:
            continue

        results.append((x, y, w, h, area, label))

    if preview:
        # プレビュー用: bbox を描画
        vis = img_bgr.copy()
        for x, y, w, h, area, label in results:
            cv2.rectangle(vis, (x, y), (x + w, y + h), (0, 0, 255), 2)
        cv2.imwrite("/tmp/preview_05_bboxes.png",
                    cv2.resize(vis, (vis.shape[1] // 6, vis.shape[0] // 6)))
        print(f"[preview] detected {len(results)} candidates")
        return results

    # 4. 出力
    os.makedirs(output_dir, exist_ok=True)
    saved_paths = []

    for i, (x, y, w, h, area, label) in enumerate(results):
        x1 = max(0, x - pad)
        y1 = max(0, y - pad)
        x2 = min(img_w, x + w + pad)
        y2 = min(img_h, y + h + pad)

        roi_bgr = img_bgr[y1:y2, x1:x2]
        roi_bg = bg_mask[y1:y2, x1:x2]

        # alpha = 前景部分のみ不透明
        alpha = cv2.bitwise_not(roi_bg)

        b, g, r = cv2.split(roi_bgr)
        rgba = cv2.merge([b, g, r, alpha])

        fname = f"sign_{i+1:03d}.png"
        fpath = os.path.join(output_dir, fname)
        cv2.imwrite(fpath, rgba)
        saved_paths.append(fpath)
        sign_count += 1

    print(f"Saved {sign_count} signs to {output_dir}")
    return saved_paths


def main():
    args = sys.argv[1:]
    preview_mode = "--preview" in args

    print(f"Loading {INPUT_PATH} ...")
    img = cv2.imread(INPUT_PATH)
    if img is None:
        print(f"ERROR: cannot load {INPUT_PATH}")
        sys.exit(1)
    print(f"Image shape: {img.shape}")

    if preview_mode:
        print("Running preview (1/6 scale)...")
        extract_signs(img, scale=1/6, preview=True)
        print("Preview saved to /tmp/preview_*.png")
    else:
        paths = extract_signs(img, scale=1.0, preview=False, output_dir=OUTPUT_DIR)
        print(f"Done. Total: {len(paths)} signs")
        if paths:
            for p in paths[:5]:
                print(f"  {p}")


if __name__ == "__main__":
    main()
