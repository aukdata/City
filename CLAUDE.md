# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

日本の街づくり・交通シミュレーションゲーム。Siv3D v0.6.16 (C++) で実装。Windows x64 専用。

## Build & Run

**要件**: Visual Studio 2026, MSVC v145, Siv3D v0.6.16 (`$(SIV3D_0_6_16)` 環境変数が必要)

**CLI からのビルド**（WSL/ターミナルから実行可能）:
```bash
# Debug
"/mnt/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" City.sln -p:Configuration=Debug -p:Platform=x64 -verbosity:minimal -noLogo
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

**Test プロジェクト** (`Test/`): Claude Code が自律的にテストを行うための Siv3D プロジェクト。詳細は `Test/CLAUDE.md` を参照。
- `convert_line_endings.py` は `-d src Test` で Test/ も対象にすること
- ビルド前に必ず `to-crlf` を実行すること（BOM なしだとコンパイルエラー）
```bash
# ビルド
"/mnt/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Test/Test.vcxproj -p:Configuration=Debug -p:Platform=x64 -verbosity:minimal -noLogo
# 実行（自動終了する）
cd /mnt/d/Users/Takuma/Creations/codes/City/Test/App && "./Test(debug).exe"
# スクリーンショット確認先
Test/App/Screenshot/
```

**カスタム Agent**（`.claude/agents/` に定義、全て Sonnet モデル）:

| Agent | 説明 | 呼び出し例 |
|---|---|---|
| `build.md` | MSBuild 実行 + エラー・警告解析 | 「ビルドして」 |
| `review.md` | コードレビュー（バグ・規約・設計） | 「レビューして」 |
| `explore.md` | コードベース調査（探索 + 実装読解・処理フロー追跡） | 「〇〇を調べて」「〇〇の実装を調べて」 |
| `commit.md` | 変更を意味のある単位に分割しコミット&プッシュ | 「コミットして」「コミット&プッシュ」 |
| `refactor.md` | チェックリストに従いコードをリファクタリング | 「リファクタして」 |
| `design.md` | 要求を受けて既存コード調査 + 実装計画を作成（Opus、コードは書かない） | 「〇〇を設計して」「計画を立てて」 |
| `implement.md` | 渡された実装計画に従ってコード編集 + ビルド確認 | 「この計画で実装して」 |
| `siv3d-api.md` | Siv3D v0.6.16 API 調査 | 「Siv3D の〇〇の使い方」 |

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
- **memory システムは使わない** — ルールや記憶は全て CLAUDE.md に記載する
- **既知の問題は `ISSUE.md` で管理** — バグ修正時に参照・更新し、完了したら該当項目を削除する
- **「Consoleに出して」は `Console <<` を使う** — `Print`（画面オーバーレイ）ではなくターミナル出力。`Print` は明示的に指示された場合のみ
- **UI を実装したら Test で表示を確認、レビューしてからメインに移す** — 新規 UI ウィジェットやレイアウト変更は Test プロジェクトでスクリーンショット検証してから本体に反映する
- **「推測するな。計測せよ。」** — パフォーマンス問題やバグの原因を推測で修正しない。必ず Console 出力やタイマーで実測データを取得し、データに基づいて修正する
- **コンパイラ警告は基本的にすべて解決すること** — 将来使用する予定のパラメータには `[[maybe_unused]]` を付与し、実際に使われるようになったら除去すること

## Siv3D

コンパイルエラーや API の使い方で迷ったら、まず `plan/SIV3D_NOTES.md` を検索すること。
API 調査は `siv3d-api` Agent 経由で行う（ヘッダを直接 Read する）。Siv3D の型・関数は極力既存 API を使い、自前で再実装しない。

**クラス定義の参照先**: `/mnt/d/Program Files/Siv3D/OpenSiv3D_0.6.16/include/Siv3D/`

## Architecture

### 仕様書

`plan/` に詳細仕様書がある。実装前に必ず参照すること。
実装によって仕様が変わった場合は、該当する仕様書を適宜更新すること:

| ファイル | 内容 |
|---|---|
| `SPEC_INDEX.md` | 仕様書の目次・ナビゲーション |
| `01_overview_spec.md` | ゲーム概要・コアメカニクス |
| `02_technical_spec.md` | アーキテクチャ・データ構造・アルゴリズム |
| `03_procedural_generation_spec.md` | マップ・地形・集落の手続き生成 |
| `04_gameplay_detail_spec.md` | 経済・バランス・イベント・幸福度 |
| `05_zoning_spec.md` | ゾーニングと建物システム |
| `06_ui_spec.md` | UIレイアウト・操作モード |
| `07_road_lane_spec.md` | 道路構造・車線システム |
| `08_pathfinding_spec.md` | グラフベース経路探索 |
| `09_vehicle_spec.md` | 車両種別・物理・交通ルール |
| `10_railway_spec.md` | 鉄道・線路・駅・ダイヤ |
| `11_placename_spec.md` | 手続き的地名生成 |
| `12_visual_spec.md` | レンダリング・ビジュアル設計・LOD |
| `13_sound_spec.md` | オーディオ設計 |
| `14_save_spec.md` | セーブシステム・チャンク永続化 |
| `15_chunk_data_spec.md` | チャンクデータ設計（二層構造・境界ノード重複） |
| `16_road_cross_section_spec.md` | 道路部品・断面構成（RoadPart + OBJ モデル） |
| `17_road_node_spec.md` | 道路ノード接続（継ぎ目・交差点・分岐合流） |
| `18_panel_system_spec.md` | パネルシステム（UI パネル管理・Z オーダー・ドラッグ・PanelBuilder 自動レイアウト） |
| `20_road_object_spec.md` | 道路オブジェクト（橋脚・街灯・標識等）・高架橋 |
| `21_guide_sign_spec.md` | 案内標識（方面及び距離 106・方面及び方向 108の2）— GuideSign 独立系統 |
