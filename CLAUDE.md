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

ビルド後、`App/` ディレクトリに実行ファイルが自動コピーされる。実行時のワーキングディレクトリは `App/` 。

テスト・リントの自動化ツールはない (Visual Studio のビルド成功が確認手段)。

## Code Style

`.editorconfig` に従う:
- インデント: タブ (サイズ4)
- **文字コード: UTF-8 BOM（必須）** — 全ての `.cpp` / `.hpp` / `.h` ファイルは BOM 付き UTF-8 で保存する。新規ファイル作成時も必ず BOM (`\xEF\xBB\xBF`) をファイル先頭に付与すること。
  - **BOM 付与スクリプト**: 新規ファイルを複数作成した後は必ず `python3 scripts/add_bom.py` を実行して BOM を付与すること。Write ツールで作成したファイルは BOM が付与されないため、このスクリプトで補完する。
- ドキュメントコメント: Doxygen形式

## Siv3D 実装留意点

コンパイルエラーや API の使い方で迷ったら、まず `plan/SIV3D_NOTES.md` を検索すること。
Siv3D の型・関数は極力既存 API を使い、自前で再実装しない。

**Siv3D クラス定義の参照先**: `/mnt/d/Program Files/Siv3D/OpenSiv3D_0.6.16/include/Siv3D/`
- API を確認するときは上記ディレクトリの `.hpp` を直接 Read すること。

## Architecture

### 仕様書

`plan/` に14本の詳細仕様書がある。実装前に必ず参照すること:

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
