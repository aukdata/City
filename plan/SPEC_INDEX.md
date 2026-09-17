# 仕様書の入口

現在の実装を確認するときは [現行実装の概要](00_current_implementation.md) から読む。操作と経済は実装と一致する現行仕様にまとめている。旧UI・ゲームプレイ・鉄道案は `unused/` に保存してあり、実装済み機能の根拠には使わない。

## 現行仕様

| 分野 | 仕様 | 担当コード |
|---|---|---|
| 実装範囲・検証・制約 | [00 現行実装](00_current_implementation.md) | 全体 |
| 操作・地図・道路計画・ポーズ | [06 UI](06_ui_spec.md) | `GameScene_Input`, `Camera`, `WorldMapView`, `PauseMenu` |
| 初期人口・収支・時間 | [04 ゲームプレイ](04_gameplay_detail_spec.md) | `Economy`, `CitySimulation`, `GameClock` |
| 日本の都市形成・町割・郊外住宅・農地 | [24 都市類型](24_japanese_urban_morphology.md) | `SettlementPlacement`, `DistrictRoads`, `SettlementFringe`, `SettlementDevelopment`, `AgriculturalLayout` |
| 湾・山脈・平野の生成 | [03 基礎生成](03_procedural_generation_spec.md) | `World`, `MapGenerator` |
| 川・切盛土・橋梁・トンネル・地名 | [26 地形と交通](26_landscape_transport_spec.md) | `RiverNetwork`, `RoadTerrainFit`, `WaterCrossings`, `TunnelRenderer` |
| 沿道店舗・田舎の住宅・山道・自動交通・信号モデル | [27 沿道の暮らし](27_roadside_life_spec.md) | `Building`, `RoadDesignLimits`, `VehiclePopulation`, `RoadSign` |
| 車線・断面・接続 | [07 道路](07_road_lane_spec.md) / [16 断面](16_road_cross_section_spec.md) / [17 ノード](17_road_node_spec.md) | `RoadNetwork`, `RoadGeometry` |
| 道路設備・標識・路線 | [20 道路設備](20_road_object_spec.md) / [21 案内標識](21_guide_sign_spec.md) / [22 路線](22_road_route_spec.md) | `RoadRenderer`, `GuideSign`, `RoadRoute` |
| 日本風の電車・編成・駅停車・復路 | [10 鉄道](10_railway_spec.md) | `TrainNetwork`, `TrainManager`, `TrainConsist`, `TrainRenderer` |
| 道路工事 | [23 工事](23_road_construction_spec.md) | `RoadPlanDraft`, `RoadPlanConstruction`, `RoadConstructionStart`, `RoadConstruction`, `ConstructionSite` |
| 描画・LOD・キャッシュ | [12 ビジュアル](12_visual_spec.md) | `WorldRenderer`, `RoadRenderer`, `CityLighting` |
| 自由運転・運転席 | [06 操作](06_ui_spec.md#自由運転) / [09 車両](09_vehicle_spec.md#自由運転の現行実装2026-09-14) | `DrivingController`, `GameScene_Driving`, `DrivingHud`, `VehiclePose` |
| 車両・経路探索 | [08 経路](08_pathfinding_spec.md) / [09 車両](09_vehicle_spec.md) / [19 移動](19_vehicle_movement_spec.md) | `VehicleManager`, `SimGraph`, `SimThread` |
| データ・保存・パネル | [14 保存](14_save_spec.md) / [15 チャンク](15_chunk_data_spec.md) / [18 パネル](18_panel_system_spec.md) | `SaveTransaction`, `World`, `PanelManager` |

## 全体設計と構想

次の資料には未実装の構想も含まれる。機能の有無は00の実装範囲、操作は06、既知の不具合はルートの `ISSUE.md` で確認する。

- [01 ゲーム構想](01_overview_spec.md)、[02 技術設計](02_technical_spec.md)、[03 基礎生成](03_procedural_generation_spec.md)
- [05 ゾーニング](05_zoning_spec.md)、[11 地名](11_placename_spec.md)、[13 音](13_sound_spec.md)
- [描画改善計画](23_realistic_city_rendering_plan.md)、[初期実装計画](IMPLEMENTATION_PLAN.md)
- [旧UI案](unused/06_ui_proposal_2026-04.md)、[旧ゲームプレイ案](unused/04_gameplay_detail_proposal_2026-04.md)
- [Siv3D API調査メモ](SIV3D_NOTES.md)、[デバッグ仕様](DEBUG_SPEC.md)、[標識参考資料](REFERENCE_SIGNS.md)

## 更新時の扱い

仕様変更は担当仕様の本文に反映する。測定条件・比較結果・写真調査・過去の修正経緯は `artifacts/<作業名>/REVIEW.md` に置く。未解決事項だけを `ISSUE.md` に残す。スクリーンショットはローカル保存し、チャットへ画像データを送信しない。

- 調査補足: [日本の街の成立と集落ネットワーク（2026-09-15）](research/2026-09_japanese_settlement_networks.md)。実装は24章§8。

- 現行仕様: [生成設定・LOD・大量交通](28_generation_assets_lod_traffic.md)。外部設定、全モデルLOD、建物発着と車列の維持。

- 現行操作: [コマンドパレット](28_commands.md)。時刻・日付・道路状態・カメラ・資金・FPSの変更。

- 実装構成: [コード構成と所有権](29_code_architecture.md)。担当ファイル、非同期処理、共有計算の入口。
