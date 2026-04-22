# CODEX.md

This file provides guidance to Codex CLI when working with code in this repository.

## Project Overview

日本の街づくり・交通シミュレーションゲーム。Siv3D v0.6.16 (C++) で実装。Windows x64 専用。

## Build & Run

**要件**: Visual Studio 2022, MSVC v143, Siv3D v0.6.16 (`$(SIV3D_0_6_16)` 環境変数が必要)

**CLI からのビルド**（WSL/ターミナルから実行可能）:
```bash
# Debug
"/mnt/d/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe" City.sln -p:Configuration=Debug -p:Platform=x64 -verbosity:minimal -noLogo
# Release（-p:Configuration=Release に変更するだけ）
```

ビルド後、`App/` ディレクトリに実行ファイルが自動コピーされる。実行時のワーキングディレクトリは `App/`。
- Debug: `App/City(debug).exe` / Release: `App/City.exe`

**起動コマンド**（「起動して」と明示的に指示された場合のみ実行。指示がなければ起動しない）:
```bash
cd /mnt/d/Users/Takuma/Creations/codes/City/App && "./City(debug).exe" --load default &
```
- `--load <saveName>` でタイトル画面をスキップして直接ロード
- ワーキングディレクトリは必ず `App/` にすること
- `cmd.exe /c start` は括弧入りパスで失敗するため使わない
- 起動後は `cd /mnt/d/Users/Takuma/Creations/codes/City` でプロジェクトルートに戻ること

## Codex Agents

Codex 用エージェントは `./.codex_agents/` に配置する。
（`build.md`, `review.md`, `explore.md`, `commit.md`, `refactor.md`, `design.md`, `implement.md`, `siv3d-api.md`, `image-summarizer.md`）

Codex の実行時設定ファイルとして、リポジトリルートの `.codex` も参照すること。

## Code Style

詳細なコーディング規約は `CODING_STYLE.md` を参照すること。

### ファイル形式

`.editorconfig` に従う:
- インデント: タブ (サイズ4)
- **文字コード: UTF-8 BOM（必須）** — 全ての `.cpp` / `.hpp` / `.h` ファイルは BOM 付き UTF-8 で保存する。新規ファイル作成時も必ず BOM (`\xEF\xBB\xBF`) をファイル先頭に付与すること。
- ドキュメントコメント: Doxygen形式
- **CRLF/BOM ファイルの編集**: Edit ツールでマッチに失敗するため、**Edit する前に必ず** LF 化し、**全ての Edit 完了後に** 戻すこと:
  ```
  python3 chore/convert_line_endings.py to-lf -d src Test   # Edit 前
  python3 chore/convert_line_endings.py to-crlf -d src Test # Edit 後
  ```
  引数なし: `src/` のみ走査。`-d dir1 dir2 ...` で対象ディレクトリを指定可能

### 命名規則

過剰な略語は使わない。識別子は読んで意味が即座にわかる名前にすること。

**OK（慣用として許容）:**
- `tex` / `vert` — グラフィクス分野の定番
- `bez` — Bezier（プロジェクト全体で統一使用）
- `mat` — Matrix または Material（文脈で明確）
- `dx` / `dz` — delta x/z（数学慣用）
- `md` — MeshData（短スコープかつ型が自明な場合）
- ループ変数 `i` / `j` など

**NG（過剰な略語の例）:**
- `mk` → `make`（動詞の短縮）
- `ls` → `lineStyle`、`rn` → `rightVec`、`sld` → `subLampDef` など文脈がないと読めないもの
- `k` プレフィックスは `constexpr` 定数専用 — 引数・ローカル変数には使わない

## Development Rules

- **コミットは意味のある単位で分ける** — 1つのコミットに無関係な変更を混ぜない
- **既知の問題は `ISSUE.md` で管理** — バグ修正時に参照・更新し、完了したら該当項目を削除する
- **「Consoleに出して」は `Console <<` を使う** — `Print`（画面オーバーレイ）ではなくターミナル出力。`Print` は明示的に指示された場合のみ
- **UI を実装したら Test で表示を確認、レビューしてからメインに移す** — 新規 UI ウィジェットやレイアウト変更は Test プロジェクトでスクリーンショット検証してから本体に反映する
- **「推測するな。計測せよ。」** — パフォーマンス問題やバグの原因を推測で修正しない。必ず Console 出力やタイマーで実測データを取得し、データに基づいて修正する
- **コンパイラ警告は基本的にすべて解決すること** — 将来使用する予定のパラメータには `[[maybe_unused]]` を付与し、実際に使われるようになったら除去すること

## Siv3D

コンパイルエラーや API の使い方で迷ったら、まず `plan/SIV3D_NOTES.md` を検索すること。
Siv3D の型・関数は極力既存 API を使い、自前で再実装しない。

**クラス定義の参照先**: `/mnt/d/Program Files/Siv3D/OpenSiv3D_0.6.16/include/Siv3D/`

## Architecture

`plan/` に詳細仕様書がある。実装前に必ず参照すること。
実装によって仕様が変わった場合は、該当する仕様書を適宜更新すること。
