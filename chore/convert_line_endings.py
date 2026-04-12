#!/usr/bin/env python3
"""CRLF/BOM 変換スクリプト

Usage:
    python3 chore/convert_line_endings.py to-lf                # src/ 内の .cpp/.hpp/.h を全て LF 化
    python3 chore/convert_line_endings.py to-crlf              # src/ 内の .cpp/.hpp/.h を全て CRLF 化
    python3 chore/convert_line_endings.py to-lf  -d src Test   # 指定ディレクトリを走査
    python3 chore/convert_line_endings.py to-crlf file1 file2  # 指定ファイルのみ CRLF 化
"""

import sys
import pathlib

PROJECT_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_DIRS = ["src"]
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


def convert_dir(dir_path: pathlib.Path, to_crlf: bool) -> int:
    """ディレクトリ内の対象ファイルを全て変換し、変換数を返す。"""
    count = 0
    for f in dir_path.rglob("*"):
        if f.suffix not in EXTENSIONS:
            continue
        if convert_file(f, to_crlf):
            count += 1
    return count


def convert_specified(to_crlf: bool, files):
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
    args = sys.argv[2:]

    if args and args[0] == "-d":
        # -d dir1 dir2 ... : ディレクトリ指定モード
        dirs = args[1:] if len(args) > 1 else DEFAULT_DIRS
        count = 0
        for d in dirs:
            p = PROJECT_ROOT / d
            if p.is_dir():
                count += convert_dir(p, to_crlf)
            else:
                print(f"Warning: {d} is not a directory, skipping", file=sys.stderr)
        label = "CRLF+BOM" if to_crlf else "LF (no BOM)"
        print(f"{count} files converted to {label}")
    elif args:
        # ファイル指定モード
        convert_specified(to_crlf, args)
    else:
        # デフォルト: src/
        count = 0
        for d in DEFAULT_DIRS:
            count += convert_dir(PROJECT_ROOT / d, to_crlf)
        label = "CRLF+BOM" if to_crlf else "LF (no BOM)"
        print(f"{count} files converted to {label}")


if __name__ == "__main__":
    main()
