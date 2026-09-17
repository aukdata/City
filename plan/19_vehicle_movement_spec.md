# 19. 車両移動・車線走行仕様

2026-09-13：都心/郊外の沿道店舗・田舎の民家、道路種別の線形制限、自動交通、信号と標識モデルの現行仕様は [27 沿道の暮らし](27_roadside_life_spec.md) を参照。

## 1. 概要

車両は常にベジェ曲線の上を走行する。道路の車線上でも交差点内でも例外なく同一のモデルで扱う。

---

## 2. セグメント

車両が走行するベジェ曲線を **セグメント** と呼ぶ。2種類ある。

### 2.1 Lane セグメント

道路エッジ上の1車線分のパス。路盤中心ベジェの4制御点を車線中心オフセット分だけ横にずらして導出する。

```
P0_lane = P0_road + perpA * centerA
P1_lane = P1_road + perpA * centerA
P2_lane = P2_road + perpB * centerB
P3_lane = P3_road + perpB * centerB
```

- `perpA`: nodeA 端におけるベジェ接線の水平垂直方向 `(tangent.z, 0, -tangent.x).normalized()`
- `perpB`: nodeB 端における同上
- `centerA = (lane.offsetA_L + lane.offsetA_R) / 2`
- `centerB = (lane.offsetB_L + lane.offsetB_R) / 2`
- 路盤ベジェの向きが正規方向と逆の場合（`shouldFlipOffsets`）、オフセットの符号を反転する

### 2.2 Connection セグメント

交差点内の旋回パス。出発車線の出口点と目標車線の入口点を結ぶベジェ曲線。

```
P0 = 出発車線の出口ワールド座標
P1 = P0 + 出口接線方向 * handleLength
P2 = P3 - 入口接線方向 * handleLength
P3 = 目標車線の入口ワールド座標
```

- `handleLength`: P0-P3 間の直線距離の 1/3 程度（滑らかな旋回を保証）
- 接線方向は出発/目標の各車線ベジェの端点における接線
- Connection は静的データ。道路変更時にのみ再生成する

---

## 3. 交差点のグラフ構造

交差点を1つのノードとして表現せず、各道路の端をノードとする。

### 例: 四差路

```
      EdgeA
        |
    NodeA_exit ──conn1──→ NodeB_entry
        |  └──conn2──→ NodeC_entry
        |
    NodeA_entry ←──conn3── NodeD_exit
```

- 交差点に接続する各エッジの各車線が exit ノードと entry ノードを持つ
- exit → entry を結ぶ Connection エッジが進行可能な方向を表現
- 進行不可（右折禁止等）は Connection エッジが存在しないだけ

### 利点

- 車両の経路は常に「セグメントの列」で表現できる
- 経路探索グラフと車両の走行パスが 1:1 対応
- 信号機は Connection エッジの通行可否として表現（赤 = blocked）
- 車両は Connection の入口手前で停止。既存の「エッジ端で停止」ロジックを流用可能

---

## 4. 車両の位置表現

### 4.1 状態

```cpp
enum class VehicleLocation : uint8
{
    OnLane,        ///< 車線セグメント上
    OnConnection,  ///< 交差点接続セグメント上
    ChangingLane,  ///< 車線変更中（2つの Lane セグメント間をブレンド）
};
```

### 4.2 OnLane

```
位置 = laneBezier.positionAt(arcPos)
```

- `edgeId`, `laneIndex`, `arcPos` で一意に決まる
- `arcPos` は Lane セグメントの弧長上の位置

### 4.3 OnConnection

```
位置 = connectionBezier.positionAt(arcPos)
```

- `connectionId`, `arcPos` で一意に決まる
- 車両は Connection の始点（arcPos=0）から終点（arcPos=length）まで走行

### 4.4 ChangingLane

```
位置 = lerp(laneFrom.positionAt(arcPos),
            laneTo.positionAt(arcPos),
            blendWeight)
```

- `edgeId`, `laneFrom`, `laneTo`, `arcPos`, `blendWeight` で決まる
- `blendWeight` が 0→1 に推移する間、`arcPos` は進み続ける
- 完了時に `laneIndex = laneTo`, `location = OnLane` に遷移

---

## 5. 経路のウェイポイント

Dijkstra の結果はセグメントの列として返される。

```cpp
struct RouteWaypoint
{
    enum class Type : uint8 { Lane, Connection };

    Type  type;
    int   id;               ///< Lane: edgeId, Connection: connectionId
    int   laneIndex;        ///< Lane 時の車線番号（Connection 時は未使用）
    float entryArcPos;      ///< セグメント上の入口位置
    float length;           ///< セグメントのベジェ弧長 [m]
    float estimatedTimeSec; ///< 推定通過時間 [game sec]
};
```

車両はウェイポイントを順に消化する:
1. 現在のセグメント上で `arcPos` を進める
2. セグメント端到達 → 次のウェイポイントを pop
3. 新しいセグメントの `entryArcPos` から走行開始

Lane と Connection で分岐しない。同一ロジック。

---

## 6. 信号機

信号は **LaneConnection 単位**で通行可否を制御する。

### 基本モデル

```cpp
struct SignalPhaseDef {
    float      duration;            ///< フェーズ持続時間 [ゲーム秒]
    Array<int> greenConnectionIds;  ///< このフェーズで青になる LaneConnection の ID リスト
};
```

- フェーズは「青になる LaneConnection ID のリスト」を保持する
- エッジ単位ではなく旋回パス単位なので、以下が表現可能:
  - 時差式右折青矢印（右折 LaneConnection のみ青、直進・左折は赤）
  - 歩車分離（車両 LaneConnection は全青または全赤）
  - スクランブル交差点（全 LaneConnection 赤 + 歩行者青）
- 黄色フェーズは持たず、描画時にフェーズ終了 3 秒前から派生表示する
- 全赤期間（クリアランス）は初期実装では持たない（将来拡張）

### 停止位置と判定タイミング

- 車両は Connection の入口手前で停止（`kSignalStopDist = 15m` 付近で減速開始）
- 判定は「これから使う LaneConnection が青かどうか」で行う
- 停止線到達時点で、車両は次に使う LaneConnection を先読みして判定する（後述「経路先読み」）
- Connection に進入したあと（`location == OnConnection`）は信号状態を参照しない（進入済みは突っ切る）

### フェーズ編集 UI

- `signal_edit` パネルの右ペインに交差点図を描画し、各 LaneConnection の Bezier を可視化
- 各フェーズで LaneConnection ごとに青/赤をクリックでトグル
- フェーズ一覧表示では、フェーズごとに LaneConnection の青数を小アイコンで表示

### 矢印サブランプの描画

- 旧仕様の `SignalPlacement.subLampStates`（edgeId ごとの矢印状態手動設定）は**廃止**
- 矢印ランプの点灯はフェーズから**自動導出**する:
  - 進入エッジに属する LaneConnection のうち、現在青のものを `classifyTurn()`（8章参照 / `08_pathfinding_spec.md`）で旋回分類
  - 左折の LaneConnection が青 ⇔ 左矢印ランプ点灯
  - 右折の LaneConnection が青 ⇔ 右矢印ランプ点灯
  - 直進の LaneConnection が青 ⇔ 本体ランプ（青）点灯

---

## 7. 車線変更

### トリガーの優先順位

車線変更は以下の優先順位で判断する。経路駆動（必要車線変更）が常に優先される。

1. **経路駆動（必須）**: 次の交差点で必要な LaneConnection に乗るための車線変更
2. **追い越し（任意）**: 前方車両が遅い場合の追い越し。経路駆動と逆方向への車線変更は禁止
3. **キープレフト（任意）**: 必要車線変更がなく追い越しも不要な場合、左寄せ

### 経路駆動車線変更（先読み）

車両は現在走行中エッジ上で、`routeWaypoints[routeIdx]` の次のウェイポイント（**1 waypoint 先**）が要求する車線 = `targetLane` を常に把握する。

`targetLane` の決定手順:

```
次ウェイポイント wp = routeWaypoints[routeIdx]
候補 = node.laneConnections を走査して
  { conn | conn.fromEdgeId == currentEdge
        && conn.toEdgeId   == wp.edgeId
        && conn.toLaneIndex == wp.laneIndex }
候補のうち、現在車線 currentLane に最も近い conn.fromLaneIndex を targetLane とする
（完全一致があればそれ、なければ |fromLaneIndex - currentLane| が最小のもの）
```

**先読み範囲は 1 waypoint まで**。複数先を見ない理由はグラフが車線ノード単位で構築されているので総合コストに反映済みであること。

### 緊急度（urgency）モデル

残り距離に応じて車線変更の切迫度を計算する。

```
d            = エッジ終端までの残り距離 [m]
needed       = |currentLane - targetLane|   // 必要な車線変更回数
kLaneChangePerNeedDist = 60.0f              // 1 車線変更あたりの余裕距離 [m]

urgency = clamp01(1 - d / (needed * kLaneChangePerNeedDist))
  // d が needed * 60m 以上 → urgency = 0 （余裕）
  // d が 0 付近           → urgency = 1 （ギリギリ）
  // d が不足              → urgency > 1 （距離不足）
```

`needed == 0` のとき urgency は定義せず、経路駆動変更は発動しない（追い越し・キープレフトのみ）。

### urgency に応じた挙動（段階的緩和 = 強引モード）

| urgency 範囲 | 挙動 |
|---|---|
| 0.0 ～ 0.3 | 通常の安全基準 (`isLaneChangeSafe` デフォルト) で試行。ギャップが無ければ待機 |
| 0.3 ～ 0.7 | 安全マージン（`s0`・`T`）を段階的に縮小して試行 |
| 0.7 ～ 1.0 | **強引モード**: 安全チェックをスキップして割り込む。自車は軽減速、ターゲット車線の後続車は既存 IDM により急減速で受け入れ |
| 1.0 超 | 強制スイッチ: `location = ChangingLane` に即座遷移。周囲車両は IDM が自然に反応（一時的に車体が重なって見えても許容） |

強引モードの実装原則:
- 既存 IDM（追従モデル）を一切改造しない
- 「割り込み挿入 → 後続車が次フレームから新しい前車として自車を追従 → 急減速」という自然な連鎖で譲り合いを表現
- 割り込まれた後続車が一時的に安全距離を下回っても、IDM の強いブレーキ項が距離を回復する

### 追い越し・キープレフト（気まぐれ車線変更の統合）

経路駆動車線変更が不要（`needed == 0`）な場合に限り、以下を発動する:

- **追い越し**: 前方車両の速度が `currentSpeedLimit * 0.7` 未満かつ右車線が空いていれば右へ車線変更
- **キープレフト**: 追い越しも不要なら左車線が空いていれば左へ車線変更

**禁止条件**:
- 経路駆動による `targetLane` と逆方向への気まぐれ変更は禁止（例: `targetLane` が左側なのに追い越しで右へ行くのは NG）
- 交差点手前 `kNoLaneChangeNearIntersection`（仮 20m）以内での気まぐれ変更は禁止
- 強引モード発動中は気まぐれ変更の判定をスキップ

### 実行フロー（共通）

1. `location = ChangingLane`, `laneFrom = currentLane`, `laneTo = targetLane`
2. 毎フレーム `blendWeight += dt / changeDuration`
3. 位置は `lerp(laneFrom.posAt(arc), laneTo.posAt(arc), blend)`
4. `blendWeight >= 1.0` で `location = OnLane`, `laneIndex = laneTo`

### 車線整合性の保証条件

経路先読み車線変更が正しく機能するには、経路探索側が以下を満たしている必要がある:

1. 経路探索グラフの Transition 辺は `RoadNode.laneConnections` に実在する接続にのみ張られていること（`08_pathfinding_spec.md` 参照）
2. `routeWaypoints` の `laneIndex` は `RoadNode.laneConnections` を使って辿れる車線のみを含むこと

これが保証されていない場合、車両は「物理的に行けない車線」に誘導されフォールバック（テレポート）が多発する。

---

## 8. Active / Dormant モード

Active/Dormant は表示対象の分類だけに使う。どちらも車間・車線変更・信号・交差点を更新し、実際の弧長で進む。画面外でタイマーだけ進める経路消化と、視点から離れた車の回収は行わない。

前後車は車線・交差点経路ごとの索引から二分探索する。同フレームの合流先も予約し、車体間の距離で重なりを防ぐ。建物側の車線にある発着点を経路の両端とし、目的地では減速して到着する。詳細は [生成設定・LOD・大量交通](28_generation_assets_lod_traffic.md)。

---

## 9. スレッドモデル

```
Main スレッド:
  - 全 Vehicle の所有・更新（IDM, 車線変更, 信号）
  - Lane/Connection ベジェ評価（Active のみ）
  - 描画

Sim スレッド（経路計算サービス）:
  - Main からの RouteRequest を受信
  - Dijkstra 実行 → RouteWaypoint 列を返信
  - メッセージキュー通信。shared_mutex なし
```

---

## 10. データ構造の変更点

### 追加

```cpp
/// @brief 交差点内の車線接続
struct LaneConnection
{
    int         id;
    int         nodeId;          ///< 所属する交差点ノード
    int         fromEdgeId;
    int         fromLaneIndex;
    int         toEdgeId;
    int         toLaneIndex;
    TurnType    turn;
    CubicBezier path;            ///< 旋回パスのベジェ曲線
};
```

### Vehicle への追加フィールド

```cpp
VehicleLocation location = VehicleLocation::OnLane;

// OnConnection 時
int   connectionId = -1;

// ChangingLane 時
int   laneFrom       = -1;
int   laneTo         = -1;
float laneChangeBlend = 0.0f;
```

### RoadNode への追加

```cpp
Array<LaneConnection> laneConnections;  ///< この交差点の車線接続リスト
```

---

## 11. Connection セグメントの自動生成

道路変更時に各交差点ノードで自動生成する。

### アルゴリズム

1. ノードに接続する全エッジを列挙
2. 各エッジの exit 車線と、他エッジの entry 車線の組み合わせを生成
3. ターン種別（直進/左折/右折/Uターン）を判定
4. 各組み合わせについて Connection ベジェを生成:
   - P0: exit 車線の出口ワールド座標
   - P3: entry 車線の入口ワールド座標
   - P1, P2: 各端点の接線方向 × handleLength

### 生成しない組み合わせ

- 同一エッジの exit → entry（Uターンを許可しない場合）
- 車線の方向が合わない組み合わせ（Forward の exit → Forward の entry のみ等）

---

## 12. 段階的実装計画

| 段階 | 内容 | 状態 |
|---|---|---|
| A | Lane セグメントの bezier 導出。車両を車線上に描画 | 実装済み |
| B | Connection セグメントの自動生成。交差点データ構造追加 | 実装済み |
| C | 車両の OnConnection 状態。交差点内走行 | 実装済み |
| D | 車線変更の ChangingLane 状態。ブレンド描画 | 実装済み |
| E | 信号機の Connection ベース制御 | 部分実装（既存の Edge ベース信号が動作） |
| F | 経路探索との統合（Waypoint に Connection を含める） | 部分実装（transitToNextWaypoint 内で Connection を自動検索） |
| G | **経路探索の車線整合性修正**: `TrafficGraph::rebuild()` の Transition 辺を `RoadNode.laneConnections` に実在するものだけに制限。`classifyTurn()` ヘルパー（45° ルール）を `TrafficCommon.hpp` に実装 | 未実装 |
| H | **経路駆動車線変更（先読み + 強引モード）**: 1 waypoint 先の `targetLane` を計算、urgency モデルで段階的緩和、気まぐれ変更を経路駆動に統合 | 未実装 |
| I | **信号の LaneConnection 単位化**: `SignalPhaseDef.greenEdgeIds` → `greenConnectionIds`。`TrafficLight::isGreen(connectionId)`。`subLampStates` 廃止してフェーズから描画自動導出 | 未実装 |
| J | **信号編集 UI の LaneConnection 対応**: `signal_edit` パネルで旋回パス Bezier を可視化、LaneConnection ごとの青/赤トグル | 未実装 |

段階 G → H → I → J の順に進める。G と H は車線整合性の修正でセットで必要（G だけだと経路通りに走れず fallback が増える）。I と J は信号仕様の変更でセット。I は G/H が先に完了していないと信号判定の意味が薄い。

---

## 13. 他仕様書との関係

| 仕様書 | 関係 |
|---|---|
| `07_road_lane_spec.md` | Lane の offsetA/B を使用して Lane セグメントを導出 |
| `08_pathfinding_spec.md` | グラフ構造の変更（交差点ノード → エッジ端ノード + Connection） |
| `09_vehicle_spec.md` | Vehicle 構造体の拡張（VehicleLocation, connectionId 等） |
| `16_road_cross_section_spec.md` | 路盤部品と車線の分離原則 |
| `17_road_node_spec.md` | ノードの幾何（フィレット曲線等）と Connection パスの関係 |
