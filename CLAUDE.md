# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

日本の街づくり・交通シミュレーションゲーム。Siv3D v0.6.16 (C++) で実装。Windows x64 専用。

現状: Main.cpp は Siv3D のテンプレートのみ。詳細な仕様書が `plan/` に揃っており、実装を開始できる状態。

## Build

**要件**: Visual Studio 2022, MSVC v143, Siv3D v0.6.16 (`$(SIV3D_0_6_16)` 環境変数が必要)

Visual Studio でソリューションを開いてビルド:
- **Debug**: `Intermediate/City/Debug/City(debug).exe`
- **Release**: `Intermediate/City/Release/City.exe`

**CLI からのビルド**（WSL/ターミナルから実行可能）:
```bash
"/mnt/d/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe" City.sln -p:Configuration=Debug -p:Platform=x64 -verbosity:minimal -noLogo
```
Release の場合は `-p:Configuration=Release` に変更する。

ビルド後、`App/` ディレクトリに実行ファイルが自動コピーされる。実行時のワーキングディレクトリは `App/` 。

**起動コマンド**（「起動して」と言われたら実行）:
```bash
cd /mnt/d/Users/Takuma/Creations/codes/City/App && "./City(debug).exe" --load default &
```
- `--load <saveName>` でタイトル画面をスキップして直接ロード
- ワーキングディレクトリは必ず `App/` にすること
- `cmd.exe /c start` は括弧入りパスで失敗するため使わない
- 起動後は `cd /mnt/d/Users/Takuma/Creations/codes/City` でプロジェクトルートに戻ること

テスト・リントの自動化ツールはない (Visual Studio のビルド成功が確認手段)。

**Test プロジェクト** (`Test/`): Claude Code が自律的にテストを行うための Siv3D プロジェクト。詳細は `Test/CLAUDE.md` を参照。
- `convert_line_endings.py` は `-d src Test` で Test/ も対象にすること
- ビルド前に必ず `to-crlf` を実行すること（BOM なしだとコンパイルエラー）
```bash
# ビルド
"/mnt/d/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe" Test/Test.vcxproj -p:Configuration=Debug -p:Platform=x64 -verbosity:minimal -noLogo
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
| `siv3d-api.md` | Siv3D v0.6.16 API 調査 | 「Siv3D の〇〇の使い方」 |

## Task Management Files

- **`ISSUE.md`** — 既知の問題一覧。完了したら該当項目を削除する。バグ修正時に参照・更新すること。

## Workflow Rules

- **コミットは意味のある単位で分ける** — 1つのコミットに無関係な変更を混ぜない
- **「Consoleに出して」は `Console <<` を使う** — `Print`（画面オーバーレイ）ではなくターミナル出力。`Print` は明示的に指示された場合のみ
- **memory システムは使わない** — ルールや記憶は全て CLAUDE.md に記載する

## Debugging Principles

- **「推測するな。計測せよ。」** — パフォーマンス問題やバグの原因を推測で修正しない。必ず Console 出力やタイマーで実測データを取得し、データに基づいて修正する。

## Warnings Policy

- **コンパイラ警告は基本的にすべて解決すること。**
- 将来使用する予定で先んじて定義しているパラメータには `[[maybe_unused]]` を付与する。
- `[[maybe_unused]]` を付けたパラメータが実際に使われるようになったら、`[[maybe_unused]]` を必ず除去すること。

## Code Style

**詳細なコーディング規約は `CODING_STYLE.md` を参照すること。**

`.editorconfig` に従う:
- インデント: タブ (サイズ4)
- **文字コード: UTF-8 BOM（必須）** — 全ての `.cpp` / `.hpp` / `.h` ファイルは BOM 付き UTF-8 で保存する。新規ファイル作成時も必ず BOM (`\xEF\xBB\xBF`) をファイル先頭に付与すること。
- ドキュメントコメント: Doxygen形式
- **CRLF/BOM ファイルの編集**: `.cpp`/`.hpp`/`.h` は CRLF 改行 + UTF-8 BOM のため Edit ツールでマッチに失敗する。**Edit する前に必ず**以下のスクリプトで LF 化し、**全ての Edit 完了後に**戻すこと:
  ```
  python3 chore/convert_line_endings.py to-lf -d src Test   # LF化 + BOM除去（Edit前に必ず実行）
  # ... Edit ツールで編集 ...
  python3 chore/convert_line_endings.py to-crlf -d src Test # CRLF化 + BOM復元（Edit後に必ず実行）
  ```
  - 引数なし: `src/` のみ走査。`-d dir1 dir2 ...` で対象ディレクトリを指定可能

## Siv3D 実装留意点

コンパイルエラーや API の使い方で迷ったら、まず `plan/SIV3D_NOTES.md` を検索すること。
Siv3D の使い方は `siv3d-api` Agent で調査する（ヘッダファイルを直接読む）。
Siv3D の型・関数は極力既存 API を使い、自前で再実装しない。

**Siv3D クラス定義の参照先**: `/mnt/d/Program Files/Siv3D/OpenSiv3D_0.6.16/include/Siv3D/`
トークン削減のため基本は `siv3d-api` Agent 経由で確認する。
API を確認するときは上記ディレクトリの `.hpp` を直接 Read すること。

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
