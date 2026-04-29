# 技術仕様書

## アーキテクチャ概要

```
┌──────────────────────────────────────────────────────────┐
│                     MAIN THREAD                          │
│  GameScene (分割構成)                                     │
│    ├─ GameScene.cpp      (初期化・ロード・更新ループ)    │
│    ├─ GameScene_Input.cpp (入力ハンドラ・カーソル)       │
│    ├─ GameScene_Render.cpp (3D描画・車両座標計算)        │
│    └─ GameScene_Panels.cpp (パネルUI描画)               │
│                                                          │
│  ┌─────────┐  ┌──────────┐  ┌─────────────┐            │
│  │  World  │  │ RoadNet  │  │VehicleManager│           │
│  │ Chunks  │  │  Graph   │  │(IDM・Active/ │           │
│  └─────────┘  └──────────┘  │ Dormant管理) │           │
│                              └──────┬───────┘           │
│  ┌──────────┐  ┌──────────┐        │                    │
│  │ZoneManager│ │ Economy  │  TrafficCommon (共通)       │
│  └──────────┘  └──────────┘  (IDM・車線変更・信号)      │
│                                                          │
│  ┌─────────────┐  ┌───────────────┐                     │
│  │ TrainNetwork │  │  EventSystem  │                     │
│  │ TrainManager │  └───────────────┘                     │
│  └─────────────┘                                         │
│                                                          │
│  Renderers: World / Road / Vehicle / Train / UI / Debug  │
│             PlaceName                                    │
└──────────────────────┬───────────────────────────────────┘
                       │ MessageQueue (lock-free)
                       ▼
┌──────────────────────────────────────────────────────────┐
│                   SIM THREAD                             │
│  SimThread                                               │
│    ├─ TrafficGraph (Dijkstra経路探索)                    │
│    └─ SimGraph (RoadNetworkの軽量コピー)                 │
└──────────────────────────────────────────────────────────┘
```

### スレッドモデル

- **Main Thread**: ゲームループ・入力・描画・高レベル更新
- **Sim Thread**: Dijkstra経路探索（バックグラウンド）
- **通信**: lock-free MPSC メッセージキュー（shared_mutex 不要）
- **共有データ**: SimGraph（const, shared_ptr で Main から Sim に渡す）

---

## 1. ワールド・チャンクシステム

```
チャンクサイズ: 1024 × 1024 メートル
ワールドサイズ: 64 × 64 チャンク
アクティブチャンク: カメラ周辺 5×5 = 25チャンク のみ更新・描画
```

Chunk 構造の詳細は `15_chunk_data_spec.md` を参照。

### チャンクの主要データ

- HeightMap (65x65 grid, 16mセル)
- TerrainType (64x64 grid)
- ZoneMap (64x64 grid) → `05_zoning_spec.md` 参照
- BuildingGrid (64x64 grid)

### プロシージャル生成

生成パイプラインの詳細は `03_procedural_generation_spec.md` を参照。

1. **地形生成**: Perlinノイズ（多重オクターブ）で高さマップを生成（マルチスレッド）
2. **地区配置**: ポアソン分布で集落を配置（Urban/Suburbs/Rural）
3. **道路生成**: 3層階層（国道→県道→地区内道路）をA*で自動生成
4. **ポスト処理**: 鋭角修正・曲線平滑化・交差点検出・重複エッジ削除
5. **ゾーン・建物**: 地区中心からの距離に応じてゾーンと建物を自動配置

---

## 2. 道路ネットワーク

### データ構造

- **RoadNode**: 端点・継ぎ目・交差点・分岐合流 → `17_road_node_spec.md`
- **RoadEdge**: 3次ベジェ曲線の道路区間
  - **物理構造**: RoadPart の配列 → `16_road_cross_section_spec.md`
  - **車線構造**: Lane の配列 → `07_road_lane_spec.md`
  - **運用変更**: TempOp / PlannedChange → `07_road_lane_spec.md` §5-6
- **RoadPlan**: 複数エッジをまとめた計画単位
  - `edge.planId` を canonical link とし、`RoadPlan` は `name`, `routeName`, `routeId`, `viaPoints`, `totalLength`, `totalCost`, `constructionDuration`, `constructionStart`, `completionDate` を保持する
  - 着工・開通はエッジ単位ではなく計画単位で扱い、所属エッジを同時に `UnderConstruction` / `Open` へ遷移させる

### ベジェ曲線の弧長パラメータ化

- **弧長-t テーブル**: 50サンプル / エッジで事前計算
- `arcLength → t → position` で車両位置を変換

### 新設道路と既存エッジの交差処理

1. 交差点座標を検出（ベジェ×ベジェの交点計算）
2. 既存 RoadEdge を交差点で2分割
3. 交差点に RoadNode を挿入（T字/十字/斜め交差を自動判定）
4. LaneConnection を再構築
5. PlannedChange・TempOp を分割後のエッジに引き継ぎ

---

## 3. 車両AIシステム

### 概要

車両管理は2つのクラスで構成:
- **VehicleManager** (Main Thread): Active/Dormant管理、IDM物理更新、経路リクエスト生成
- **TrafficManager** (Sim Thread可): 車両更新、Dijkstra経路探索、バス路線管理
- **TrafficCommon** (共通ユーティリティ): IDM計算、車線変更安全判定、信号フェーズ生成

車種別パラメータ・構造体の詳細は `09_vehicle_spec.md` を参照。

### 経路探索

- **アルゴリズム**: Dijkstra（TrafficGraph 上、LaneNode/BorderNode ベース）
- **コスト関数**: `移動時間 = 距離 / (制限速度 × (1 - 混雑度 × 0.8))`
  - Transition コスト: Straight=2, Left=5, Right=8, UTurn=15
  - 信号コスト: 期待待ち時間を加算
- **再探索**: 毎フレーム最大10台、道路ネットワーク変更時に全車両

詳細は `08_pathfinding_spec.md` を参照。

### 追従モデル（IDM）

```
a = a_max × [1 - (v/v0)^4 - (s*(v, Δv) / s)^2]
s* = s0 + v×T + v×Δv / (2×√(a_max×b))
```

IDM パラメータは `09_vehicle_spec.md` の `getDefaultIDMParams()` で車種別に定義。

### 車線変更

- 出口までの距離が 30m 未満では車線変更しない
- 安全ギャップ判定: 前方 `s0 + 8m`、後方 `s0 + T×15 + 8m`
- キープレフト優先

---

## 4. 信号機システム

### 4.1 信号の自動設置

`rebuildLaneConnections()` 完了時に、レーン接続数 (LaneConnection) が **8 を超える** 交差点に対して自動的に信号を設置する。既に `signalPlacement` が設定済みの場合は上書きしない。

- **VehicleManager**: edgeControl が Signal のノードにのみ TrafficLight を生成

信号停止判定距離: 15m

### 4.2 デフォルト信号サイクルの自動生成

`signalPlacement` が存在するがフェーズが未定義（空）の場合、以下のアルゴリズムでデフォルトサイクルを生成する。自動設置時・ユーザー手動設置時の両方に適用。

#### Step 1: 直進ペアの構築

交差点に接続する全エッジを「直進ペア」にグループ化する。選出順は attachments の順序に従う。

1. **(a)** 未選出のエッジを attachments 順に1つ選ぶ
2. **(b)** そのエッジの反対方向（180度）から ±45度以内にある未選出エッジを列挙し、最も 180度に近いものを選ぶ
   - 該当エッジが 0 本 → そのエッジ単独で 1 ペアとして選出済みにする
3. **(c)** 2 本をペアとして選出済みにする
4. **(d)** 未選出エッジが残っていれば (a) に戻る

エッジの方向はノードにおけるベジェ接線から求める。

#### Step 2: フェーズの生成

各ペアに対して 1 フェーズを作る（ペア数 = フェーズ数）。

- **青にする接続**: `laneConnection.fromEdgeId` がペアのいずれかのエッジであるもの全て
- **フェーズ持続時間**: `max(青接続数 * 120, 300)` ゲーム秒（実時間換算: `max(接続数*2秒, 5秒)` × 60倍）
- **接続数 0 のペア**: フェーズを生成しない（スキップ）
- **右折の競合**: 現状は対向右折の衝突を許容する（将来的に右折フェーズ分離を検討）

### 4.3 交差点レーン接続の自動生成

`rebuildLaneConnections()` は交差点ノードに対して、各 Entry 車線（交差点に進入する車線）から適切な Exit 車線（交差点から退出する車線）への LaneConnection を方向別に生成する。

**用語定義:**
- **Entry 車線**: エッジ上の車線のうち、交差点ノードに向かって走行するもの（`exitsAtNode == true`）
- **Exit 車線**: エッジ上の車線のうち、交差点ノードから離れる方向に走行するもの（`entersAtNode == true`）
- **直進ペア**: §4.2 Step 1 と同じアルゴリズムで構築したエッジペア
- **左側エッジ**: 運転者の進行方向（ノードへの向き）に対して左側にあるエッジ群（外積の符号で判定）
- **右側エッジ**: 同、右側にあるエッジ群

#### Phase 1: 車線方向の割り当て

各直進ペアのエッジについて、Entry 車線に進行方向を割り当てる。

**入力パラメータ:**
- `A` = 自エッジの Entry 車線数
- `B` = ペア相手エッジの Exit 車線数（ペアなしなら 0）
- `L` = 左側全エッジの Exit 車線数の合計
- `R` = 右側全エッジの Exit 車線数の合計

**方向種別:**

| 方向 | 意味 |
|------|------|
| AllDirections | 全方向（到達可能な全 Exit 車線に接続） |
| Left | 左折専用 |
| StraightLeft | 左折 + 直進 |
| Straight | 直進専用 |
| StraightRight | 直進 + 右折 |
| Right | 右折専用 |

**割り当てルール:**

**(1) A = 1 の場合**: 唯一の車線に `AllDirections` を割り当てる。

**(2) A ≧ 2 かつ A ≦ B の場合**:
- 一番左の車線: `StraightLeft`
- 一番右の車線: `StraightRight`
- それ以外: `Straight`

**(3) A ≧ 2 かつ A > B の場合**:
1. `numLeft = min(floor((A − B) / 2), L)`
2. `numRight = min(floor((A − B + 1) / 2), R)`
3. 左から `numLeft` 本を `Left`、右から `numRight` 本を `Right`、残りを `Straight`
4. 補正: `L > 0` かつ `numLeft == 0` のとき、一番左の車線を `StraightLeft` に変更（直進から格上げ）
5. 特殊: `B = 0` のとき直進先がないため、`Straight` に割り当てられた車線を `Left` / `Right` に再分配する（左優先で交互に振り分け、`L` / `R` の上限を超えない範囲で）

#### Phase 2: 接続パスの算出

各方向種別に応じて、Entry 車線と Exit 車線の接続を生成する。

**AllDirections**: 到達可能な全 Exit 車線（自エッジ以外）に接続する。

**Straight（直進）**: ペア相手エッジの Exit 車線と接続する。

- N 本の直進車線を B 本の Exit 車線に振り分ける
- `B ≧ N` の場合: 左側の直進車線から順に、Exit 車線を `floor(B/N)` 本ずつ割り当てる。一番右の直進車線には残り全てを割り当てる
- `B < N` の場合: 左側の Exit 車線から順に、直進車線を `floor(N/B)` 本ずつ割り当てる。一番右の Exit 車線には残り全てを割り当てる

**Left（左折）/ Right（右折）**: 直進と同じ振り分けロジックを使用する。ただし:

- 対象 Exit 車線は、左側（左折）/ 右側（右折）の **全エッジの Exit 車線を角度順にプール**して扱う
- 左折の車線順序: 自分の左側から（車線配列の左端から右端へ）
- 右折の車線順序: 自分の右側から（車線配列の右端から左端へ）
- Exit 車線プールの順序: ペアエッジに近い角度のエッジから順に、各エッジ内は左端から右端

**StraightLeft / StraightRight**: `Straight` の接続と `Left` / `Right` の接続の和集合。

---

## 5. ゾーニングシステム

詳細は `05_zoning_spec.md` を参照。7種類のゾーンタイプ（Unzoned / UrbanControl / LowResidential / Residential / Commercial / Industrial / Agriculture）。

ゾーン操作ツール: ブラシ、矩形塗り。3D表示上に半透明カラーオーバーレイ。

---

## 6. 地形変更システム

| 操作 | 処理 |
|------|------|
| 盛土 | 高さマップを上げる（左クリック） |
| 掘削 | 高さマップを下げる（右クリック） |
| ブラシサイズ | Ctrl+ホイールで調整（20〜400m） |

- 地形変更はチャンクの HeightMap を直接編集
- コサイン減衰でブラシ端をなめらかに
- 変更後はチャンクのメッシュと道路キャッシュを再生成

---

## 7. レンダリング

### 描画レイヤー

1. **Sky**: Siv3D Sky クラス（昼夜・朝夕の色変化）
2. **地形メッシュ**: チャンクごとの DynamicMesh（フラスタムカリング）
3. **道路メッシュ**: ベジェ×断面部品から生成（LOD切替 800m）
4. **ノードキャップ**: 交差点のフィレット曲線メッシュ
5. **建物**: WorldRenderer で種別×色ブロック描画
6. **車両**: 近距離=OBJモデル、遠距離=OrientedBox
7. **列車**: TrainRenderer
8. **地名**: PlaceNameRenderer（ビルボード）
9. **UI**: UIRenderer + PanelManager（`18_panel_system_spec.md`）
10. **デバッグ**: DebugRenderer（F3メニュー）

### カメラモード

- **Overview**: 俯瞰（WASD移動、右ドラッグ回転、ホイールズーム）
- **Follow**: 車両追跡（チェイスビュー）
- **FirstPerson**: 一人称視点

### LOD

| 距離 | 描画内容 |
|------|---------|
| < 800m | 高品質メッシュ |
| 800m〜12km | LODメッシュ |
| > 12km | 描画しない |

---

## 8. ゲーム時間・イベント

### 時間管理

```cpp
using GameTime = double;  // ゲーム開始からの経過ゲーム秒

struct GameClock {
    GameTime now;
    int      year;
    uint8    month;    // 1〜12
    uint8    day;      // 1〜30（全月30日に簡略化）
    float    hour;     // 0.0〜24.0
    TimeSpeed speed;   // Paused / x1 / x2 / x4
    // 速度倍率: x1=60倍速（1リアル秒=1ゲーム分）
};
```

### イベントシステム

`04_gameplay_detail_spec.md` を参照。季節イベント8種 + ランダムイベント9種。
データ駆動テーブルで定義（EventSystem.cpp 内の `kSeasonalEvents[]` / `kRandomEvents[]`）。

---

## 9. 経済システム

`04_gameplay_detail_spec.md` を参照。

- 月次交付金: 人口ベース
- 月次支出: 道路維持費（種別×延長）
- 初期資金: 300億円

---

## 実装フェーズ

| Phase | 内容 | 状態 |
|-------|------|------|
| 1 | チャンク・ベジェ道路・車両走行・カメラ | 完了 |
| 2 | Dijkstra経路探索・IDM・信号・車線変更 | 完了 |
| 3 | ゾーン・建物自動生成・経済・HUD | 完了 |
| 4 | Perlin地形・イベント・一人称・地形編集 | 完了 |
| 5 | 鉄道（駅・ダイヤ・ブロック閉塞） | 完了 |
| 6 | セーブ・地名・オーディオ・ビジュアル仕上げ | 一部着手 |

詳細は `IMPLEMENTATION_PLAN.md` を参照。
