#!/usr/bin/env python3
"""
add_bom.py — src/ 以下の .cpp / .hpp / .h ファイルに UTF-8 BOM (EF BB BF) を付与する。
すでに BOM がある場合はスキップする。

使い方:
    python3 chore/add_bom.py            # src/ 以下を全走査
    python3 chore/add_bom.py [path...]  # 指定ファイル/ディレクトリのみ
"""
import sys
import os
from pathlib import Path

BOM = b'\xef\xbb\xbf'
EXTENSIONS = {'.cpp', '.hpp', '.h'}


def process(path: Path) -> None:
    data = path.read_bytes()
    if data.startswith(BOM):
        return
    path.write_bytes(BOM + data)
    print(f'BOM追加: {path}')


def scan(root: Path) -> None:
    for p in root.rglob('*'):
        if p.is_file() and p.suffix in EXTENSIONS:
            process(p)


def main() -> None:
    targets = sys.argv[1:] if len(sys.argv) > 1 else ['src']
    for t in targets:
        p = Path(t)
        if p.is_file():
            process(p)
        elif p.is_dir():
            scan(p)
        else:
            print(f'スキップ（存在しない）: {p}', file=sys.stderr)


if __name__ == '__main__':
    main()
