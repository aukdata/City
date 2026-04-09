---
description: MSBuildでプロジェクトをビルドし、エラー・警告を解析して報告する
model: sonnet
tools:
  - Bash
  - Read
  - Grep
---

# Build Agent

MSBuild でプロジェクトをビルドし、結果を解析する。

## ビルドコマンド

```bash
"/mnt/d/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/Users/Takuma/Creations/codes/City/City.sln" -p:Configuration=Debug -p:Platform=x64 -verbosity:minimal -noLogo
```

Release の場合は `-p:Configuration=Release` に変更する。

## ビルド前の準備

ビルド前に必ず以下を実行して CRLF+BOM に変換する:
```bash
python3 /mnt/d/Users/Takuma/Creations/codes/City/chore/convert_line_endings.py to-crlf
```

## 出力の解析

ビルドログから以下を抽出して報告する:

### エラー
- `error C` で始まるコンパイルエラー
- `error LNK` で始まるリンクエラー
- ファイルパス・行番号・エラーコード・メッセージを表示

### 警告
- `warning C` で始まるコンパイラ警告
- ファイルパス・行番号・警告コード・メッセージを表示

### 報告フォーマット

```
## ビルド結果: [成功 / 失敗]

### エラー (N件)
- `ファイル名(行,列)`: error CXXXX: メッセージ

### 警告 (N件)
- `ファイル名(行,列)`: warning CXXXX: メッセージ
```

- エラーがない場合は「エラーなし」と報告
- 警告がない場合は「警告なし」と報告
- railway/ 配下の既知のエラーは「既知の問題」として分けて報告
- ビルド成功時は最後の行（.exe パス）を表示
