#!/usr/bin/env python3
"""CRLF/BOM 変換スクリプト

Usage:
    python3 scripts/crlf.py to-lf     # src/ 内の .cpp/.hpp/.h を LF 化 + BOM 除去
    python3 scripts/crlf.py to-crlf   # src/ 内の .cpp/.hpp/.h を CRLF 化 + BOM 付与
"""

import sys
import pathlib

SRC_DIR = pathlib.Path(__file__).resolve().parent.parent / "src"
EXTENSIONS = {".cpp", ".hpp", ".h"}
BOM = b"\xef\xbb\xbf"


def convert(to_crlf: bool):
    count = 0
    for f in SRC_DIR.rglob("*"):
        if f.suffix not in EXTENSIONS:
            continue
        data = f.read_bytes()
        if to_crlf:
            # BOM 除去済みなら復元、CRLF 化
            if not data.startswith(BOM):
                data = BOM + data
            data = data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
        else:
            # BOM 除去、LF 化
            if data.startswith(BOM):
                data = data[len(BOM):]
            data = data.replace(b"\r\n", b"\n")
        f.write_bytes(data)
        count += 1
    label = "CRLF+BOM" if to_crlf else "LF (no BOM)"
    print(f"{count} files -> {label}")


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("to-lf", "to-crlf"):
        print(__doc__)
        sys.exit(1)
    convert(to_crlf=(sys.argv[1] == "to-crlf"))


if __name__ == "__main__":
    main()
