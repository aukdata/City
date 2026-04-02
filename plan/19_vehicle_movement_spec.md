# 19. 車両移動・車線走行仕様

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

- 信号は Connection セグメントの通行可否として動作
- 赤信号 = 該当 Connection が通行不可
- 車両は Connection の入口手前（= 直前の Lane セグメントの出口付近）で IDM 停止
- 既存の `kSignalStopDist` の停止ロジックをそのまま活用

---

## 7. 車線変更

### トリガー

- キープレフト（左車線が空いていれば左へ）
- 前方車両が遅い場合（右車線が空いていれば右へ）
- 経路が次の交差点で特定車線を要求する場合

### 実行

1. `location = ChangingLane`, `laneFrom = currentLane`, `laneTo = targetLane`
2. 毎フレーム `blendWeight += dt / changeDuration`
3. 位置は `lerp(laneFrom.posAt(arc), laneTo.posAt(arc), blend)`
4. `blendWeight >= 1.0` で `location = OnLane`, `laneIndex = laneTo`

### 安全チェック

車線変更開始前に目標車線上の前方/後方ギャップを確認（既存ロジック）。交差点手前では車線変更を禁止。

---

## 8. Active / Dormant モード

画面外の車両は位置計算を省略する。

| | Active | Dormant |
|---|---|---|
| 位置計算 | 毎フレーム bezier 評価 | なし |
| 速度制御 | IDM + 信号停止 | なし |
| セグメント遷移 | arcPos がセグメント端到達 | タイマー消化で遷移 |
| 描画 | する | しない |

- `estimatedTimeSec` を使ってタイマーベースでセグメントを消化
- 画面に入ったら arcPos を復元して Active に遷移

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
| A | Lane セグメントの bezier 導出。車両を車線上に描画 | 未実装 |
| B | Connection セグメントの自動生成。交差点データ構造追加 | 未実装 |
| C | 車両の OnConnection 状態。交差点内走行 | 未実装 |
| D | 車線変更の ChangingLane 状態。ブレンド描画 | 未実装 |
| E | 信号機の Connection ベース制御 | 未実装 |
| F | 経路探索との統合（Waypoint に Connection を含める） | 未実装 |

---

## 13. 他仕様書との関係

| 仕様書 | 関係 |
|---|---|
| `07_road_lane_spec.md` | Lane の offsetA/B を使用して Lane セグメントを導出 |
| `08_pathfinding_spec.md` | グラフ構造の変更（交差点ノード → エッジ端ノード + Connection） |
| `09_vehicle_spec.md` | Vehicle 構造体の拡張（VehicleLocation, connectionId 等） |
| `16_road_cross_section_spec.md` | 路盤部品と車線の分離原則 |
| `17_road_node_spec.md` | ノードの幾何（フィレット曲線等）と Connection パスの関係 |
