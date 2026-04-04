# 実装計画

## 現在の状態

**実装フェーズ**: Phase 1〜5 実装完了（Phase 6 未着手）
**Main.cpp**: GameApp::run() を呼ぶだけの状態
**次のアクション**: Phase 6（セーブ・地名・オーディオ・ビジュアル仕上げ）

---

## ファイル構成

```
City/
├── Main.cpp                    # エントリーポイント → GameApp::run()
├── stdafx.h / stdafx.cpp       # プリコンパイル済みヘッダー
├── src/
│   ├── GameApp.hpp/.cpp        # シーン管理（Title/Game）
│   ├── scene/
│   │   ├── SceneCommon.hpp     # SceneState / SceneData
│   │   ├── TitleScene.hpp/.cpp # タイトル・セーブ選択
│   │   ├── GameScene.hpp       # ゲームシーン本体（メンバ・宣言）
│   │   ├── GameScene.cpp       # 初期化・ロード・更新ループ・セーブ
│   │   ├── GameScene_Input.cpp # 入力処理・モード別ハンドラ
│   │   ├── GameScene_Render.cpp # 3D描画・車両座標計算・オーバーレイ
│   │   └── GameScene_Panels.cpp # パネルUI（Edge/Node/Vehicle/NameList）
│   ├── world/
│   │   ├── Chunk.hpp           # チャンクデータ構造
│   │   └── World.hpp/.cpp      # チャンク管理・Perlin生成
│   ├── road/
│   │   ├── RoadTypes.hpp       # RoadEdge / RoadNode / Lane 等
│   │   ├── RoadEnums.hpp       # OpState / LaneDir / LaneType 等
│   │   ├── RoadPartTypes.hpp   # RoadPartType / RoadPart
│   │   ├── RoadNetwork.hpp/.cpp # グラフ管理（追加・削除・交差・LaneConnection）
│   │   ├── BezierUtil.hpp/.cpp  # ベジェ曲線・弧長パラメータ化
│   │   ├── RoadPartRegistry.hpp/.cpp # TOML+OBJ アセット
│   │   └── ObjParser.hpp/.cpp   # OBJ ファイル解析
│   ├── traffic/
│   │   ├── Vehicle.hpp          # 車両構造体・IDMParams・enum
│   │   ├── TrafficCommon.hpp    # 共通ユーティリティ（IDM・車線変更・定数）
│   │   ├── VehicleManager.hpp/.cpp  # Main Thread 車両管理
│   │   ├── TrafficManager.hpp/.cpp  # Sim Thread 車両管理
│   │   ├── TrafficGraph.hpp/.cpp    # Dijkstra 経路探索グラフ
│   │   ├── TrafficLight.hpp/.cpp    # 信号機
│   │   └── BusRoute.hpp        # バス路線
│   ├── sim/
│   │   ├── SimThread.hpp/.cpp   # バックグラウンド経路探索スレッド
│   │   ├── SimGraph.hpp         # RoadNetwork の軽量コピー
│   │   ├── SimMessages.hpp      # RouteRequest / RouteResponse
│   │   └── MessageQueue.hpp     # lock-free MPSC キュー
│   ├── railway/
│   │   ├── TrackTypes.hpp       # TrackNode / TrackEdge / Schedule
│   │   ├── Train.hpp            # 列車構造体
│   │   ├── TrainNetwork.hpp/.cpp # 線路ネットワーク
│   │   └── TrainManager.hpp/.cpp # 列車運行管理
│   ├── gen/
│   │   ├── MapGenerator.hpp/.cpp    # 地区配置・道路自動生成
│   │   ├── RoadPathfinder.hpp/.cpp  # A*（道路配置用）
│   │   ├── PlaceNameGenerator.hpp/.cpp # TOML ベース地名生成
│   │   └── TerrainType.hpp          # バイオーム定義
│   ├── zone/
│   │   ├── ZoneTypes.hpp        # ZoneType enum
│   │   ├── Building.hpp         # Building 構造体
│   │   └── ZoneManager.hpp/.cpp # ゾーン管理・建物生成
│   ├── economy/
│   │   └── Economy.hpp/.cpp     # 月次収支
│   ├── event/
│   │   └── EventSystem.hpp/.cpp # 季節・ランダムイベント（データ駆動）
│   ├── time/
│   │   └── GameClock.hpp        # GameTime / GameClock
│   ├── render/
│   │   ├── WorldRenderer.hpp/.cpp   # 地形・建物
│   │   ├── RoadRenderer.hpp/.cpp    # 道路メッシュ・LOD
│   │   ├── VehicleRenderer.hpp/.cpp # 車両（モデル/Box）
│   │   ├── TrainRenderer.hpp/.cpp   # 線路・列車
│   │   ├── UIRenderer.hpp/.cpp      # HUD
│   │   └── PlaceNameRenderer.hpp/.cpp # 地名ビルボード
│   ├── ui/
│   │   ├── Camera.hpp/.cpp      # 3カメラモード
│   │   ├── PanelManager.hpp/.cpp # パネルUI管理
│   │   └── PanelWidget.hpp      # パネルウィジェット
│   ├── debug/
│   │   ├── DebugRenderer.hpp/.cpp # F3 デバッグ表示
│   │   ├── DebugLog.hpp         # ログ
│   │   └── PerfStats.hpp        # パフォーマンス計測
│   └── save/
│       └── RoadBinary.hpp/.cpp  # 道路バイナリ保存
└── plan/
    └── *.md                     # 仕様書群
```

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
- [x] 車線変更ロジック（キープレフト優先、安全ギャップ確認）
  - 参照: `02_technical_spec.md §3`, `09_vehicle_spec.md §2`

#### 2-4. 信号機システム
- [x] `TrafficLight` / `SignalPhase` の実装
- [x] 信号待ち停止線での IDM 適用（仮想停止リーダー方式）
- [x] 3 本以上エッジがある交差点への自動信号機設置（2フェーズ）
- [x] `expectedWaitTime()` を Dijkstra コストへ組み込む
- [x] `TempOp` 期限切れ検出によるグラフ自動再構築トリガー
  - 参照: `02_technical_spec.md §4`

#### 2-5. 車両生成ロジック
- [x] ランダム出発・目的地の選択（T キーで生成）
- [x] ゾーン別・時間帯別の車種構成比に基づいた生成
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
- [x] 建物 3D 描画（種別・成長段階別 Box）
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
- [x] `BusStop` 構造体・バス停の設置 UI（B キーでモード切替・左クリックで配置）
- [x] `BusRoute`（停留所リスト・運行間隔）の定義と管理
- [x] バス車両の特殊挙動（停車・待機 5 秒）
- [ ] ドア開閉アニメーション（Phase 6 以降）
  - 参照: `06_ui_spec.md §7`, `09_vehicle_spec.md §6`

#### 4-1. プロシージャル地形生成
- [x] Perlin ノイズ（6 オクターブ、振幅 18m）による高さマップ生成
- [ ] 地形分類・初期旧道ネットワーク自動生成（Phase 6 以降）
  - 参照: `03_procedural_generation_spec.md`

#### 4-2. イベントシステム
- [x] 季節イベント 8 種・ランダムイベント 11 種（月次ロール）
- [x] 通知 UI（画面右上カード表示）
- [ ] 災害による実際の道路閉鎖 TempOp 生成（簡易実装のみ）
  - 参照: `04_gameplay_detail_spec.md §6`

#### 4-3. 一人称視点
- [x] F キーによる視点切り替え（俯瞰 / 車両追従 / 一人称）
- [x] 車両追従カメラ・一人称カメラ
  - 参照: `06_ui_spec.md §11`

#### 4-4. 地形変更ツール
- [x] G キーで地形編集モード（左=盛土・右=掘削）
- [x] Ctrl+ホイールでブラシサイズ変更・コサイン減衰ブラシ
- [x] HeightMap 編集・チャンクメッシュ再生成
- [ ] トンネル/橋梁（Phase 6 以降）
  - 参照: `02_technical_spec.md §6`, `06_ui_spec.md §8`

---

### Phase 5: 鉄道・高度機能

**目標**: 鉄道を建設して公共交通として機能させる

#### 5-1. 鉄道ネットワーク
- [x] `TrackNode`, `TrackEdge`（ベジェ曲線・閉塞区間管理）
- [x] 駅（TrackNodeType::Station）・X キーで線路描画モード
- [ ] ホーム有効長・信号所の詳細実装（Phase 6 以降）
  - 参照: `10_railway_spec.md §1〜3`

#### 5-2. ダイヤ・閉塞制御
- [x] `TrainSchedule`, `StopEntry`（停車時間・ダイヤ間隔）
- [x] 単線閉塞占有ルール（tryOccupy/releaseOccupy）
- [ ] 遅延伝播・行き違い手順の詳細実装（Phase 6 以降）
  - 参照: `10_railway_spec.md §6〜7`

#### 5-3. 踏切
- [ ] `LevelCrossing`（TempOp で道路を遮断）
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

## 今後の実装予定（Phase 6 以降）

### 道路・交通
- 車線分岐・合流システム
  - RoadNode に LaneConnection テーブル（車線単位の接続制御）
  - RoadEdge に lateralOffset（横方向オフセット、出口ランプ等の端寄せ）
  - 空なら従来通り全対全接続（後方互換）
- 信号システム強化
  - 矢印信号(→)による制御
  - ユーザーがフェーズを自分で編集できるUI
- 交差点での適切な軌跡（交差点内をベジェ曲線で車両が旋回）
- 車の車線走行（車線内の横位置制御）
- 道路標識
  - 速度制限等の規制標識
  - 青看板（方面及び方向、方面及び距離）の自動生成+編集UI

### 地形・自然
- 川・湖（自然水系の生成）
- 木などの自然オブジェクト

### 建物
- 建物の道路沿い配置（道路から一定セットバック）

### プレイヤー操作・視点
- 車の運転機能（自分で車を操作）
- 1人称視点での歩行（街を歩き回る）

### UI
- ミニマップ

### システム
- セーブ/ロード（チャンク永続化・オートセーブ・メタデータ）

### 追加提案（検討中）
- 歩行者（駅〜建物、バス停間）
- 駐車場・駐車スペース
- 天候システム（雨・雪・台風）
- 音響（エンジン音・踏切警報・環境音）

---

## 変更ログ

| 日付 | 変更内容 |
|------|---------|
| 2026-03-16 | 初版作成 |
| 2026-03-16 | Gemini レビュー反映: 弧長逆引きテーブル・TempOp/PlannedChange を 1-4 に追加、信号機スタブ注記・isNodePassable・LaneConnection 注意点を Phase 2 に追加、バス路線システム(4-0)を追加、signal/ を traffic/ に統合、Building.hpp を分離、BusRoute.hpp を追加 |
| 2026-03-16 | Phase 2 実装: PathfindingGraph(2-1)・Dijkstra(2-2)・IDM(2-3)・TrafficLight(2-4)・ランダム生成(2-5 一部) |
| 2026-03-16 | Phase 3 実装: ZoneType/Building/ZoneManager(3-1)・月次建物生成(3-2)・Economy(3-3)・ZoneOverlay描画・HUD拡張 |
| 2026-03-17 | Phase 2残・4・5実装: TempOpグラフ更新(2-4)・建物3D描画(3-2)・Sky/Perlin地形(4-1)・カメラモード(4-3)・バス路線(4-0)・イベント(4-2)・地形編集(4-4)・鉄道システム(5-1〜5-2)・add_bom.py |
| 2026-03-22 | 「今後の実装予定」セクション追加（メモリから移行） |
| 2026-04-04 | 総合リファクタリング: TrafficCommon 共通ユーティリティ抽出（IDM・車線変更・信号フェーズ生成）、GameScene 4分割（Input/Render/Panels）、RoadRenderer キャッシュ無効化統合、RoadNetwork LaneConnection 計算共通化（calcLaneEndpoint）、EventSystem データ駆動テーブル化、VehicleRenderer 属性テーブル統合。ファイル構成・仕様書を現行実装に同期 |
