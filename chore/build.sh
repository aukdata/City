#!/bin/bash
# ビルドスクリプト: LF/CRLF変換を含むDebugビルド
# 使い方: bash chore/build.sh [Release]

set -e
cd "$(dirname "$0")/.."

CONFIG="${1:-Debug}"

# CRLF+BOM に変換（MSVCビルド用）
python3 chore/convert_line_endings.py to-crlf

# ビルド
"/mnt/d/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe" \
    City.sln -p:Configuration="$CONFIG" -p:Platform=x64 -verbosity:minimal -noLogo

echo "Build ($CONFIG) completed."
