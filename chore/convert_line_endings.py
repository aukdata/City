#!/usr/bin/env python3
"""CRLF/BOM 変換スクリプト

Usage:
    python3 chore/convert_line_endings.py to-lf              # src/ 内の .cpp/.hpp/.h を全て LF 化
    python3 chore/convert_line_endings.py to-crlf            # src/ 内の .cpp/.hpp/.h を全て CRLF 化
    python3 chore/convert_line_endings.py to-lf  file1 file2 # 指定ファイルのみ LF 化
    python3 chore/convert_line_endings.py to-crlf file1 file2 # 指定ファイルのみ CRLF 化
"""

import sys
import pathlib

SRC_DIR = pathlib.Path(__file__).resolve().parent.parent / "src"
EXTENSIONS = {".cpp", ".hpp", ".h"}
BOM = b"\xef\xbb\xbf"


def convert_file(f: pathlib.Path, to_crlf: bool) -> bool:
    """1ファイルを変換する。変更があれば True を返す。"""
    original = f.read_bytes()
    data = original
    if to_crlf:
        if not data.startswith(BOM):
            data = BOM + data
        data = data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
    else:
        if data.startswith(BOM):
            data = data[len(BOM):]
        data = data.replace(b"\r\n", b"\n")
    if data != original:
        f.write_bytes(data)
        return True
    return False


def convert_all(to_crlf: bool):
    """src/ 内の対象ファイルを全て変換する。"""
    count = 0
    for f in SRC_DIR.rglob("*"):
        if f.suffix not in EXTENSIONS:
            continue
        if convert_file(f, to_crlf):
            count += 1
    label = "CRLF+BOM" if to_crlf else "LF (no BOM)"
    print(f"{count} files converted to {label}")


def convert_specified(to_crlf: bool, files: list[str]):
    """指定ファイルを変換する。"""
    count = 0
    for path_str in files:
        f = pathlib.Path(path_str).resolve()
        if not f.exists():
            print(f"Warning: {path_str} not found, skipping", file=sys.stderr)
            continue
        if convert_file(f, to_crlf):
            count += 1
    label = "CRLF+BOM" if to_crlf else "LF (no BOM)"
    print(f"{count} files converted to {label}")


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("to-lf", "to-crlf"):
        print(__doc__)
        sys.exit(1)
    to_crlf = sys.argv[1] == "to-crlf"
    if len(sys.argv) >= 3:
        convert_specified(to_crlf, sys.argv[2:])
    else:
        convert_all(to_crlf)


if __name__ == "__main__":
    main()
