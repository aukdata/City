# コード構成と所有権

## 変更箇所の入口

| 変更したいこと | 主な実装 | 境界 |
| --- | --- | --- |
| 一フレームの進行・入力と描画の順序 | `scene/GameScene.cpp` | ゲーム状態を所有して各サービスを呼ぶ。生成・保存の詳細は持たない。 |
| ロード画面・非同期生成の完了判定 | `GameScene_Loading.cpp` | future の完了を取得し、失敗時は Loading に留める。 |
| 初期生成の順序と事後調整 | `GameScene_Generation.cpp` | gen のアルゴリズムを順番に呼び、描画準備へ渡せるデータにする。 |
| ゲーム全体の保存・復元 | `GameScene_Storage.cpp` | save の形式別処理と SaveTransaction に委譲する。 |
| 再現用撮影・走行確認 | `GameScene_Capture.cpp` / 既存の `*_Review.cpp`, `*_Playtest.cpp` | 通常更新に検証用カメラやログの詳細を混在させない。 |
| 常時表示・詳細タブ・周辺地図の配置 | `ui/CityHud`, `render/UIRenderer` | テスト済みHUDへ描画・入力を委譲し、時刻変更は GameScene 側で適用する。 |
| 道路断面・計画・路線の編集パネル | `GameScene_Panels.cpp` | RoadPlanDraft / RoadConstructionStart などの共通処理を使う。 |
| 建物・車両・敷地などの情報 | `GameScene_InspectorPanels.cpp` | GameScene の選択状態と PanelManager を使用する。 |
| 信号の現示編集と模式図 | `GameScene_SignalPanel.cpp` | 道路断面と模式図の色は `ui/RoadDiagramStyle.hpp` で共有する。 |
| 道路描画・路面メッシュ・区画線・沿道設備 | `RoadRenderer.cpp`, `RoadRenderer_Mesh.cpp`, `RoadRenderer_Markings.cpp`, `RoadRenderer_Furniture.cpp` | キャッシュは RoadRenderer が所有し、内部断面計算は RoadRenderDetail、汎用メッシュ演算は MeshGeometry で共有する。 |
| 信号の見た目と選択輪郭 | `RoadRenderer_Signals.cpp` | 配置は RoadGeometry、現示は交通シミュレーションを参照する。 |
| 地形・建物・木・遠景の表示 | `WorldRenderer_*`, `TerrainSurfaceGeometry`, `TreeGeometry` | CPU 上の幾何生成と GPU 資源の管理を区別する。 |
| 地区街路・集落の接続候補検索 | `gen/RoadNodeIndex.hpp` | 空間検索だけを共有し、距離・費用・除外条件は呼出側が決める。 |
| 初期集落・農地・家並み | `MapGenerator`, `SettlementPlacement`, `DistrictRoads`, `SettlementDevelopment`, `AgriculturalLayout` | 生成順序と外部設定の依存を維持する。 |
| 車両・運転・経路 | `traffic/VehicleManager*`, `DrivingController`, `sim/SimThread` | 所有する可変車両と、読取り専用の経路グラフを分ける。 |
| 歩行者・乗換 | `pedestrian/PedestrianNetwork`, `PedestrianManager`, `PedestrianTrips`, `PedestrianTransfers` | 徒歩網・住民の移動状態・乗換予約を分け、車と列車の実到着で更新 |
| 鉄道・ダイヤ・車庫 | `railway/TrainNetwork`, `TrainManager`, `RailTimetable`, `RailDepotBuilder` | 編集データと運行中の状態を区別する。 |

## 生成と通常実行の境界

`GameScene` はワールド、道路、鉄道、選択状態、描画器を所有する。分割された `.cpp` は同じクラスの実装であり、新しい状態所有者を増やさない。

生成・ロード中は `GamePhase::Loading` が通常のゲーム更新を止める。ワーカが生成・復元するデータを、完了前に通常描画へ渡さない。進捗数値は atomic、進捗文字列は mutex で受け渡す。メインスレッドが future の結果を取得してから GPU 資源を準備し、Playing へ進む。地形生成・地形ロード・広域地形の CPU 処理は `sim/ParallelJobs.hpp` で実行し、全ワーカを合流させてから例外を呼出側へ返す。シーン破棄時は生成・ロードタスクの完了を先に待ち、そのタスクが開始した経路サービスを停止する。

通常プレイでは入力・建設状態・経済・車両・描画の順序を `GameScene::update` が管理する。経路サービスへは `shared_ptr<const SimGraph>` とメッセージを渡し、可変の `World` を直接共有しない。`SimGraph` の全件・差分更新は同じ抽出処理を使い、差分更新は編集前後の接続先を更新する。ノードの撤去はそのノードを端点に持つ辺と付帯情報も撤去し、付け替え済みの辺は保持する。

## 道路ノード検索

`RoadNodeIndex` は XZ 平面の200 mバケットを管理する。追加ノードは `insert`、生成段階で候補集合を入れ替えるときは `clear` して再登録する。ノードを移動したときも作り直す。ネットワークから削除済みの ID は検索時に除外する。

- 地区街路の接続は `findNearest`。孤立ノードも距離だけで吸着先にする。
- 集落から街道への接続は `findBest`。未接続ノード・自分自身を除外し、進入方向を含む `SettlementPlacement::roadAccessCost` で評価する。
- 半径ちょうどの点を除く。同費用なら既存の走査順で最初の候補を保つ。地形の高さを検索距離へ混ぜない。

## 信号の座標

`RoadGeometry::signalAnchor` が車道端の XZ 座標・道路上の基準点・向きを返す。A/B端それぞれの切り欠き長さと非対称断面を使う。描画とクリック選択の両方が同じ関数を呼ぶため、片方だけ別の路肩へずれない。

地上・高架の高さ選択は描画側が行う。道路形状の編集後は既存の無効化通知で信号キャッシュを破棄する。現示の更新と柱の幾何キャッシュは異なる頻度で更新するため統合しない。

## 保存と検証

`GameScene_Storage` はゲーム全体の保存対象をまとめ、形式ごとの直列化は `save/` と `railway/RailwayStorage` が担当する。新しい保存先へ書き、再読込みを検証してから公開する既存の順序を保つ。`RoadBinary` はチャンクと全体で一つのデコーダを使い、全レコードと拡張領域が読めた場合にだけ呼出側へ反映する。文字列は UTF-8 の uint16 長さ上限を保存前に検査する。単体の鉄道保存も欠番 ID を保持し、重複 ID は復元を中止する。ゲームのセーブ形式4では `ChunkDevelopmentStorage` が建物・農地・ゾーン・市街化区域・区画ポリゴンを保存し、ロードで配置を再生成しない。復元の全レコードを検証してからチャンクへ反映し、経路サービスは配置の復元後に開始する。道路バイナリ形式19のノード・制御点・計画点と区画ポリゴンの頂点はdouble精度を保存する。旧ゲームセーブの移行は行わない。

ビルド対象は City と Test の各プロジェクトに登録する。Test はシーンを起動せずに核となる処理と描画を検証するため、両者のソース集合は完全には一致しない。ファイルの追加時は重複・欠落・IDE の分類も確認する。

今回の調査範囲と確認結果は [包括的リファクタリングの記録](../artifacts/comprehensive_refactor_20261006/REVIEW.md)、未解決の動作上の問題は [ISSUE.md](../ISSUE.md) を参照する。

## 道路と鉄道の共通インフラ（2026-09-17）

本体では `TrainNetwork::bind(&m_network)` により、鉄道が `RoadNetwork` の実エッジを参照する。形状の別コピーや描画専用の路盤は作らない。`TrackEdge` は `RoadEdge` の別名、軌道は `LaneType::Rail`。駅・ダイヤ・車庫は鉄道側、部品・車線・設計縦断は共通網が所有する。

`TransportCrossSection` が断面と軌道位置を共通化し、`TransportMode` が経路の交通資格を表す。道路の編集通知は鉄道参照と静的メッシュも更新する。列車の閉塞は車線単位で、複線の上下線を互いに塞がない。道路専用の処理（家の接道、農道の分岐候補、自動車標識、車の進入）は `hasRoadLanes` / `Lane::allows` で対象を限定する。

保存では共通網が形状と断面、鉄道メタデータがID参照と運行設定を担当する。描画は道路側が路盤と構造物、鉄道側がレール・枕木・駅・車両を担当する。検証は `SharedTransportTests.cpp` と [改修記録](../artifacts/shared_transport_20260917/REVIEW.md)。

## 診断ログの所有権

本体の診断は `DebugLog::print` / `DBG_LOG` へ集約する。`GameApp::run` がアセット登録前にログを初期化し、シーンとワーカの破棄後にセッションを閉じる。履歴とファイルは同じ mutex で保護し、描画側は履歴のコピーを一度だけ取得する。ワーカの時刻は steady_clock を使い、Siv3D の可変時計を参照しない。追記時の UTF-8 BOM はファイル先頭だけに置く。


## 描画の時間分散と遠景

建物のCPU形状はチャンク本体と周囲8チャンクの高さ・建物、道路切削、交通施設の不変の写しから生成する。モデルの寸法と遠景形状もロード中にCPU専用の写しへ分離し、ワーカはGPU資源や可変Worldを読まない。完成時はepochとrevisionを検証し、GPU転送をフレームあたり約2msの単位に分け、全バッチが揃った結果だけを公開する。単一のGPU転送そのものは中断できないため、2msは厳密な最大値ではない。準備中と遠景は位置・回転・寸法を保った代理形状を使う。

遠景の道路は既存の簡略路面をチャンク・材質単位で結合する。道路のID、削除前の端点、CPU形状を所有元ごとに保持し、編集されたバッチを無効化する。無効なバッチは通常の道路描画へ戻し、再結合は一度に1チャンクとする。広域では微細な設備と1800m超の視点距離の影描画を省略する。

Chunk::markMeshDirtyは描画フラグとcontentRevisionを更新する。HousingCapacityCacheはrevisionが変わったチャンクだけ住宅数を再集計し、描画器がmeshDirtyを解除しても変更を失わない。建物・地形・土地利用の編集後はこの関数を呼ぶ。

道路の表示範囲と初期路面、HUD・地名・標識の文字とテクスチャはロード中に準備する。WorldStage / ChunkStage / RoadCacheCostは20msを超えた処理だけをDebugLogへ記録し、視点・道路ID・チャンクから残る停止を調べられる。


### 初回準備と転送の分割（2026-10-06 追加）

`MeshUploadSlices` はCPU形状を最大16,384頂点・8,192三角形の転送単位へ分ける。三角形の順序と頂点属性を維持し、地形・建物・道路設備に同じ分割器を使う。転送時間枠は個々のGPU呼び出しを中断する上限ではなく、次の呼び出しを開始する条件として扱う。

道路設備・規制標識・案内標識は、道路属性・ベジェ曲線・沿道の高さ・境界名・標識メタデータの写しからCPUワーカーで生成する。`HeightSnapshot` は必要な矩形内の高さを所有し、地形の三角形補間を共用する。最大2ジョブを実行し、GPU転送は本体スレッドの約1msの枠で進める。epoch/revisionが一致し、設備全体が揃った結果だけを公開する。失効したfutureは完了後に破棄し、編集フレームでfutureの破棄待ちを発生させない。最初の周辺設備、線路・鉄道施設、車両・歩行者のモデルとシェーダー、診断文字はロード中に準備する。

道路と交差点の空間索引は1,024mのセルに保守的な範囲を登録し、通常描画・計画線・信号で利用する。長い道路は触れる全セルへ登録し、編集時は所有元のセルだけを更新する。地形編集はcontentRevisionと高さの署名を確認し、編集チャンクを横断する道路も失効させる。遠景道路の結合はチャンク別の所有元集合を使う。

`MapTerrainLayer` は地形のサンプリングと陰影生成を行単位・約1msの枠で進め、生成途中は完成済みの画像と座標を維持する。同期経路と同じ解像度・色を使い、失効時は途中結果を破棄する。車両ID索引は追加時に登録し、車両を削除して配列を詰めた場合だけ位置を更新する。

診断CSVは描画前準備、3D flush、resolve、画面転送、UI、交通の内訳とSystem::Updateを分ける。`benchmark_view` の明示的な `performanceOnly` 指定では、性能計測に必要な進捗と交通統計だけを状態ファイルへ出力する。通常の操作検証は詳細な状態出力を維持する。計測用出力自身の時間も記録し、CPU描画時間と実フレーム間隔を区別する。
