# 建物モデル検証 — 2026-09-11

- Windows / Blender 4.5: 配布する7個のOBJを実際にインポート。全モデルにUV、1個のマテリアル、読み込み可能な1024×512拡散色画像があることを確認。三角形数をマニフェストと照合し、面積ゼロのポリゴンがないことを検証。
- Cycles: 全7モデルを前後の14視点から確認。看板のUV反転、屋根縁と診療所外壁の重複面を修正。プレビューには撮影用の照明と地面を使用。
- Siv3D v0.6.16: `City.sln` のDebug / x64ビルド成功。コンパイラ警告なし。
- Test実行: 8件成功、0件失敗。新規2ケースで7モデルの実ロード・寸法・単一マテリアル・画像サイズと、工場／公共施設／オフィスの用途別選択を検証。既存6ケースも成功。
- 編集したC++ファイルはUTF-8 BOM + CRLFを保持。
- ゲーム本体の起動は行っていない。実際の街での接道・配置・フレーム時間は未検証。Blenderプレビューを実ゲーム内の画質や性能の検証結果として扱わない。

成果物:

- `town_buildings.blend`: 全7モデルと共有テクスチャを内蔵。プレビュー用カメラ・照明を含む。
- `blender_gallery.png`: 全モデルの集合レンダー。
- `*_blender_front.png` / `*_blender_back.png`: 個別レンダー。
- `blender_review_sheet.jpg`: 前後14視点の一覧。
- `blender_validation.json`: Blenderインポート検証結果。
- ゲーム用OBJ/MTL/TOML/PNG: `App/assets/buildings/commercial/`。
- 再生成とモデル説明: `App/assets/buildings/commercial/MODEL_CATALOG.md`。
