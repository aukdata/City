# コード構成と所有権

## 変更箇所の入口

| 変更したいこと | 主な実装 | 境界 |
| --- | --- | --- |
| 一フレームの進行・入力と描画の順序 | `scene/GameScene.cpp` | ゲーム状態を所有して各サービスを呼ぶ。生成・保存の詳細は持たない。 |
| ロード画面・非同期生成の完了判定 | `GameScene_Loading.cpp` | future の完了を取得し、失敗時は Loading に留める。 |
| 初期生成の順序と事後調整 | `GameScene_Generation.cpp` | gen のアルゴリズムを順番に呼び、描画準備へ渡せるデータにする。 |
| ゲーム全体の保存・復元 | `GameScene_Storage.cpp` | save の形式別処理と SaveTransaction に委譲する。 |
| 再現用撮影・走行確認 | `GameScene_Capture.cpp` / 既存の `*_Review.cpp`, `*_Playtest.cpp` | 通常更新に検証用カメラやログの詳細を混在させない。 |
| 道路断面・計画・路線の編集パネル | `GameScene_Panels.cpp` | RoadPlanDraft / RoadConstructionStart などの共通処理を使う。 |
| 建物・車両・敷地などの情報 | `GameScene_InspectorPanels.cpp` | GameScene の選択状態と PanelManager を使用する。 |
| 信号の現示編集と模式図 | `GameScene_SignalPanel.cpp` | 道路断面と模式図の色は `ui/RoadDiagramStyle.hpp` で共有する。 |
| 道路面・交差点のメッシュ | `render/RoadRenderer.cpp` | 標識・信号・工事・ストリーミングは担当ファイルへ分ける。 |
| 信号の見た目と選択輪郭 | `RoadRenderer_Signals.cpp` | 配置は RoadGeometry、現示は交通シミュレーションを参照する。 |
| 地形・建物・木・遠景の表示 | `WorldRenderer_*`, `TerrainSurfaceGeometry`, `TreeGeometry` | CPU 上の幾何生成と GPU 資源の管理を区別する。 |
| 地区街路・集落の接続候補検索 | `gen/RoadNodeIndex.hpp` | 空間検索だけを共有し、距離・費用・除外条件は呼出側が決める。 |
| 初期集落・農地・家並み | `MapGenerator`, `SettlementPlacement`, `DistrictRoads`, `SettlementDevelopment`, `AgriculturalLayout` | 生成順序と外部設定の依存を維持する。 |
| 車両・運転・経路 | `traffic/VehicleManager*`, `DrivingController`, `sim/SimThread` | 所有する可変車両と、読取り専用の経路グラフを分ける。 |
| 鉄道・ダイヤ・車庫 | `railway/TrainNetwork`, `TrainManager`, `RailTimetable`, `RailDepotBuilder` | 編集データと運行中の状態を区別する。 |

## 生成と通常実行の境界

`GameScene` はワールド、道路、鉄道、選択状態、描画器を所有する。分割された `.cpp` は同じクラスの実装であり、新しい状態所有者を増やさない。

生成・ロード中は `GamePhase::Loading` が通常のゲーム更新を止める。ワーカが生成・復元するデータを、完了前に通常描画へ渡さない。進捗数値は atomic、進捗文字列は mutex で受け渡す。メインスレッドが future の結果を取得してから GPU 資源を準備し、Playing へ進む。シーン破棄時は生成タスクと経路サービスの完了を待つ。

通常プレイでは入力・建設状態・経済・車両・描画の順序を `GameScene::update` が管理する。経路サービスへは `shared_ptr<const SimGraph>` とメッセージを渡し、可変の `World` を直接共有しない。

## 道路ノード検索

`RoadNodeIndex` は XZ 平面の200 mバケットを管理する。追加ノードは `insert`、生成段階で候補集合を入れ替えるときは `clear` して再登録する。ノードを移動したときも作り直す。ネットワークから削除済みの ID は検索時に除外する。

- 地区街路の接続は `findNearest`。孤立ノードも距離だけで吸着先にする。
- 集落から街道への接続は `findBest`。未接続ノード・自分自身を除外し、進入方向を含む `SettlementPlacement::roadAccessCost` で評価する。
- 半径ちょうどの点を除く。同費用なら既存の走査順で最初の候補を保つ。地形の高さを検索距離へ混ぜない。

## 信号の座標

`RoadGeometry::signalAnchor` が車道端の XZ 座標・道路上の基準点・向きを返す。A/B端それぞれの切り欠き長さと非対称断面を使う。描画とクリック選択の両方が同じ関数を呼ぶため、片方だけ別の路肩へずれない。

地上・高架の高さ選択は描画側が行う。道路形状の編集後は既存の無効化通知で信号キャッシュを破棄する。現示の更新と柱の幾何キャッシュは異なる頻度で更新するため統合しない。

## 保存と検証

`GameScene_Storage` はゲーム全体の保存対象をまとめ、形式ごとの直列化は `save/` と `railway/RailwayStorage` が担当する。新しい保存先へ書き、再読込みを検証してから公開する既存の順序を保つ。

ビルド対象は City と Test の各プロジェクトに登録する。Test はシーンを起動せずに核となる処理と描画を検証するため、両者のソース集合は完全には一致しない。ファイルの追加時は重複・欠落・IDE の分類も確認する。

今回の調査範囲と確認結果は [統合リファクタリング](../REFACTOR.md)、未解決の動作上の問題は [ISSUE.md](../ISSUE.md) を参照する。
