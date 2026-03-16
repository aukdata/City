# 実装計画

## 現在の状態

**実装フェーズ**: Phase 1 完了・Phase 2 実装済み・Phase 3 実装済み
**Main.cpp**: GameApp::run() を呼ぶだけの状態
**次のアクション**: Phase 4-0（バス路線システム）から着手する

---

## ファイル構成方針

```
City/
├── Main.cpp              # エントリーポイント。GameApp::run() を呼ぶだけ
├── stdafx.h / stdafx.cpp # プリコンパイル済みヘッダー（Siv3D インクルード）
├── src/
│   ├── GameApp.hpp/.cpp       # ゲームループ統括（Update/Simulate/Render の呼び出し）
│   ├── world/
│   │   ├── Chunk.hpp/.cpp     # チャンクデータ構造
│   │   ├── World.hpp/.cpp     # チャンク管理・ロード/アンロード
│   │   └── TerrainMesh.hpp/.cpp # ハイトマップ → メッシュ変換
│   ├── road/
│   │   ├── RoadTypes.hpp      # RoadEdge / RoadNode / RoadPlan / Lane の構造体定義
│   │   ├── RoadNetwork.hpp/.cpp  # グラフ管理（追加・削除・交差検出）
│   │   └── BezierUtil.hpp/.cpp   # ベジェ曲線・弧長パラメータ化
│   ├── traffic/
│   │   ├── PathfindingGraph.hpp/.cpp # LaneNode / BorderNode / GraphEdge
│   │   ├── Pathfinding.hpp/.cpp      # Dijkstra 実装
│   │   ├── Vehicle.hpp/.cpp          # 車両エージェント・IDM
│   │   ├── TrafficManager.hpp/.cpp   # 車両生成・更新・再探索分散
│   │   ├── TrafficLight.hpp/.cpp     # 信号機（RoadNode に紐づくため traffic/ に配置）
│   │   └── BusRoute.hpp/.cpp         # BusStop / BusRoute データ構造
│   ├── zone/
│   │   ├── ZoneTypes.hpp      # ZoneType enum
│   │   ├── Building.hpp       # Building 構造体・成長段階（構造が複雑なため分離）
│   │   └── ZoneManager.hpp/.cpp # ゾーン評価・建物生成
│   ├── economy/
│   │   └── Economy.hpp/.cpp
│   ├── time/
│   │   └── GameClock.hpp/.cpp
│   ├── render/
│   │   ├── WorldRenderer.hpp/.cpp  # 描画レイヤー統括
│   │   ├── RoadRenderer.hpp/.cpp
│   │   ├── VehicleRenderer.hpp/.cpp
│   │   └── UIRenderer.hpp/.cpp
│   └── ui/
│       ├── Camera.hpp/.cpp    # BasicCamera3D ラッパー・操作
│       └── HUD.hpp/.cpp
└── plan/
    └── IMPLEMENTATION_PLAN.md  ← このファイル
```

> 注意: ファイル構成は実装の進行に合わせて変化しうる。上記はあくまで出発点の目安。

---

## 実装フェーズ

各タスクの状態: `[ ]` 未着手 / `[~]` 進行中 / `[x]` 完了

---

### Phase 1: 基盤（まず動かす）

**目標**: 地形が描画され、手動で道路を引いて車が走る最低限の状態

#### 1-1. プロジェクト基盤
- [x] `Main.cpp` を整理して `GameApp::run()` を呼ぶだけにする
- [x] `GameApp` クラスを作成し `update(dt)` / `render()` を用意
- [x] `GameClock` の実装（速度 ×1/×2/×4/Pause、`GameTime = double`）
  - 参照: `02_technical_spec.md §8`

#### 1-2. チャンクシステム
- [x] `Chunk` 構造体（HeightMap, TerrainMap, buildings, zoneMap, state）
- [x] `World` クラス（チャンク HashMap、カメラ周辺 5×5 のアクティブ管理）
- [x] フラットな HeightMap でチャンクを生成して表示（Perlin ノイズは後回し）
  - 参照: `02_technical_spec.md §1`

#### 1-3. カメラ
- [x] `BasicCamera3D` による見下ろし視点
- [x] WASD 移動・右ドラッグ回転・ホイールズーム
  - 参照: `06_ui_spec.md §3`

#### 1-4. ベジェ道路の描画
- [x] `BezierUtil`: 3次ベジェ曲線の評価・弧長パラメータ化（50サンプル/区間）
  - 弧長→t の逆引きテーブルを事前構築する（車両位置計算に使用。`arcLength → t → position` の変換）
- [x] `RoadTypes.hpp`: `Lane`, `RoadEdge`, `RoadNode`, `RoadPlan` の構造体
  - `TempOp` / `PlannedChange` も同ファイルで定義する（Phase 1 から車線状態変更規則を守るため必須）
  - 参照: `07_road_lane_spec.md`（Lane/RoadEdge/TempOp/PlannedChange の完全な定義）
- [x] `RoadNetwork`: エッジ・ノードの追加、交差点への分割処理
- [x] 道路の描画（ポリゴン帯生成、車線区画線）
  - 参照: `02_technical_spec.md §2`, `07_road_lane_spec.md`

#### 1-5. 簡易車両走行
- [x] `Vehicle` 構造体（位置・速度・currentEdge・arcPos）
- [x] 経路探索なし・IDM なしの単純な道路上移動
- [x] `VehicleRenderer`: 車種別の簡易3D形状
  - 参照: `02_technical_spec.md §3`

---

### Phase 2: 交通シミュレーション

**目標**: 車両が経路を選択して渋滞が発生する状態

#### 2-1. 経路探索グラフ
- [x] `LaneNode`, `BorderNode`, `GraphEdge` の実装
- [x] 道路新設時に LaneNode 自動生成（BorderNode は Phase 2 以降）
- [x] `LaneConnection` の自動生成（TurnType 判定含む）
- [x] `isNodePassable(nodeId)` の実装
  - 参照: `08_pathfinding_spec.md §1〜3`

#### 2-2. Dijkstra 経路探索
- [x] `PathfindingGraph::dijkstra(startLaneNodeId, goalEdgeId)` の実装
- [x] コスト関数: Forward / LaneChange / Transition（固定ペナルティ）
- [x] 再探索の分散（毎フレーム 10 台ずつ処理）
  - 参照: `08_pathfinding_spec.md §2〜4, §7`

#### 2-3. IDM 追従モデル
- [x] IDM 加速度計算の実装（パラメータは種別ごとに設定）
- [x] 前方車両検出・車頭距離計算（Forward/Backward 方向対応）
- [ ] 車線変更ロジック（隣接車線への空き確認）
  - 参照: `02_technical_spec.md §3`, `09_vehicle_spec.md §2`

#### 2-4. 信号機システム
- [x] `TrafficLight` / `SignalPhase` の実装
- [x] 信号待ち停止線での IDM 適用（仮想停止リーダー方式）
- [x] 3 本以上エッジがある交差点への自動信号機設置（2フェーズ）
- [ ] `expectedWaitTime()` を Dijkstra コストへ組み込む（現在は固定ペナルティ）
- [ ] `TempOp` による車線変化 → グラフ更新トリガー
  - 参照: `02_technical_spec.md §4`

#### 2-5. 車両生成ロジック
- [x] ランダム出発・目的地の選択（T キーで生成）
- [ ] ゾーン別・時間帯別の車種構成比に基づいた生成
  - 参照: `09_vehicle_spec.md §7`

---

### Phase 3: 街の発展

**目標**: ゾーンを塗ると建物が自動生成・成長し、経済が動く

#### 3-1. ゾーニングシステム
- [x] `ZoneType` enum（7種）、`ZoneManager` クラス
- [x] ゾーン塗りツール（ブラシ/矩形）の UI（左クリック=ブラシ、Shift+左ドラッグ=矩形）
- [x] ゾーンオーバーレイ描画（半透明カラー、Tab でトグル）
  - 参照: `05_zoning_spec.md`, `02_technical_spec.md §5`

#### 3-2. 建物自動生成
- [x] 発展スコア計算（道路アクセス係数）
- [x] 月次評価ループ（生成・アップグレード・衰退）
- [x] 成長段階（Stage 0〜2）
- [ ] 建物 3D 描画（現在は描画なし）
  - 参照: `05_zoning_spec.md`, `02_technical_spec.md §5`

#### 3-3. 経済システム
- [x] `Economy` 構造体（資金・人口・幸福度）
- [x] 交付金収入計算（人口規模・幸福度ボーナス）
- [x] 道路維持費計算（種別 × 延長）
- [x] 月次収支の HUD 表示（資金・人口）
  - 参照: `04_gameplay_detail_spec.md §2〜3`, `02_technical_spec.md §9`

---

### Phase 4: 体験の深化

**目標**: プロシージャル生成の街で本来のゲームループが成立する状態

#### 4-0. バス路線システム
- [ ] `BusStop` 構造体・バス停の設置 UI
- [ ] `BusRoute`（停留所リスト・時刻表・運行間隔）の定義と管理
- [ ] バス車両の特殊挙動（停車・乗降・ドア開閉アニメーション）
- [ ] バス路線設定フロー（停留所選択 → 運行間隔設定）
  - 参照: `06_ui_spec.md §7`, `09_vehicle_spec.md §6`

#### 4-1. プロシージャル地形生成
- [ ] Perlin ノイズ（多重オクターブ）による高さマップ生成
- [ ] 地形分類（山岳/丘陵/平野/河川/海岸）
- [ ] 初期旧道ネットワークの自動生成（最小全域木 + A*）
- [ ] 旧道沿いの初期建物配置
  - 参照: `03_procedural_generation_spec.md`

#### 4-2. イベントシステム
- [ ] 季節イベント8種・ランダムイベント11種
- [ ] 通知UI・災害による道路閉鎖処理
  - 参照: `04_gameplay_detail_spec.md §6`

#### 4-3. 一人称視点
- [ ] F キーによる視点切り替え
- [ ] 車両追従カメラ
  - 参照: `06_ui_spec.md §11`

#### 4-4. 地形変更ツール
- [ ] 掘削/盛土/トンネル/橋梁のツール
- [ ] HeightMap 編集・周辺チャンクのメッシュ再生成
  - 参照: `02_technical_spec.md §6`, `06_ui_spec.md §8`

---

### Phase 5: 鉄道・高度機能

**目標**: 鉄道を建設して公共交通として機能させる

#### 5-1. 鉄道ネットワーク
- [ ] `TrackNode`, `TrackEdge`（RoadEdge と同形式のベジェ曲線）
- [ ] 駅（Platform・ホーム有効長）・信号所
  - 参照: `10_railway_spec.md §1〜3`

#### 5-2. ダイヤ・閉塞制御
- [ ] `Schedule`, `StopEntry`（着発時刻・遅延伝播）
- [ ] Block 単線占有ルール・行き違い手順
  - 参照: `10_railway_spec.md §6〜7`

#### 5-3. 踏切
- [ ] `LevelCrossing`（TempOp で道路を遮断）
- [ ] 経路探索コスト加算・立体交差化
  - 参照: `10_railway_spec.md §8`

#### 5-4. 路線収支
- [ ] 駅勢圏・利用者数・月次収支計算
  - 参照: `10_railway_spec.md §10`

---

### Phase 6: 仕上げ

#### 6-1. セーブ・ロード
- [ ] `saves/{name}/meta + global/ + chunks/{cx}_{cy}/` 構造
- [ ] オートセーブ（リアル5分、3スロットローテーション）
- [ ] チャンク差分保存（dirty フラグ）
  - 参照: `14_save_spec.md`

#### 6-2. 地名システム
- [ ] 集落地名の確定的生成（シード値依存）
- [ ] 道路計画名・駅名の自動命名・手動リネーム
  - 参照: `11_placename_spec.md`

#### 6-3. オーディオ
- [ ] アンビエント層・イベント層・BGM 層の3層実装
- [ ] 設定パネル（マスター/BGM/効果音/アンビエント）
  - 参照: `13_sound_spec.md`

#### 6-4. ビジュアル仕上げ
- [ ] LOD 最適化（近距離/中距離/遠距離の切り替え）
- [ ] 時刻連動ライティング・天候エフェクト
- [ ] 青看板・信号機・電柱の自動配置
  - 参照: `12_visual_spec.md`

---

## 重要な設計上の注意点

### フェーズ2移行のための Phase 1 制約

経路探索の Phase 1 実装時に必ず守ること（`08_pathfinding_spec.md §8` より）:
- `BorderNode` を道路新設時に必ず生成する（`dirty=true` のまま放置してよい）
- `LaneNode` / `BorderNode` の ID を `PathfindingGraph` に集約して統一管理
- `dijkstra()` を `PathfindingGraph` 経由で呼ぶ（グラフ実装に依存させない）
- グラフ更新はイベントドリブン（`dirty` フラグ + 差分更新）

### 車線状態の変更規則

`07_road_lane_spec.md §9` より:
- `Lane.index` は作成後に変更禁止
- `build` の変更は必ず `PlannedChange` 経由（直接書き換え禁止）
- `dir`・`op` の変更は `TempOp` か `PlannedChange` のみ
- `build != Built` の車線の `dir`・`op` は参照禁止

### 時刻型の統一

`02_technical_spec.md §8` より:
- 全仕様書の `Date` 型は `GameTime`（`double`、ゲーム開始からの経過ゲーム秒）に統一
- `07_road_lane_spec.md`, `10_railway_spec.md` の `Date` も同様

### Phase 2 実装時の注意

- `TempOp` による車線状態変化（Open↔Closed）はグラフ更新トリガーであり、再探索キューへの追加もセットで実装する（2-4 で対応）
- `isNodePassable()` は Dijkstra の内部で使用するが、`effectiveLane()` の結果に依存するため TempOp スタック管理と密結合になる。先に `effectiveLane()` を実装してから Dijkstra に組み込む順番にする

---

## 変更ログ

| 日付 | 変更内容 |
|------|---------|
| 2026-03-16 | 初版作成 |
| 2026-03-16 | Gemini レビュー反映: 弧長逆引きテーブル・TempOp/PlannedChange を 1-4 に追加、信号機スタブ注記・isNodePassable・LaneConnection 注意点を Phase 2 に追加、バス路線システム(4-0)を追加、signal/ を traffic/ に統合、Building.hpp を分離、BusRoute.hpp を追加 |
| 2026-03-16 | Phase 2 実装: PathfindingGraph(2-1)・Dijkstra(2-2)・IDM(2-3)・TrafficLight(2-4)・ランダム生成(2-5 一部) |
| 2026-03-16 | Phase 3 実装: ZoneType/Building/ZoneManager(3-1)・月次建物生成(3-2)・Economy(3-3)・ZoneOverlay描画・HUD拡張 |
