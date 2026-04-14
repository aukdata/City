# 道路・車線データ設計仕様書

## 概要

日本の道路整備における以下を再現できるデータ構造を定義する。

| 再現したい状態 | 具体例 |
|--------------|--------|
| 暫定2車線（1+1） | 4車線路盤済み・外側2車線のみ供用（oxxo） |
| 施工時の車線シフト | 暫定1+1 → 2+0 にシフト |
| 段階的供用変化 | oooo → ooxx（右2車線を工事で閉鎖） |
| イカの耳 | ICランプの路盤・橋台のみ先行建設、供用なし |
| 延伸端部 | 本線開通済み、端部に橋台・擁壁のみ先行施工 |
| テーパー車線 | 合流・分岐で車線幅が連続的に変化 |

---

## 1. 物理軸・運用軸の分離

道路の状態を **物理軸（RoadPart）** と **運用軸（Lane）** に分離する。

```
物理軸 = RoadPart の配列（路盤・歩道・中央分離帯など）
  → 建設状態（BuildState）を持つ
  → 原則として不可逆に進行

運用軸 = Lane の配列（論理的な車線）
  → 方向・供用状態・区画線を持つ
  → 頻繁に変化しうる

o/x 表記の定義:
  o = 車両が走行できる車線（供用中）
  x = 走行できない車線（閉鎖・予約・路盤未建設など）

oxxo の内訳:
  物理: 路盤パーツ = Built（全車線分の路盤が完成）
  運用: Open  Reserved  Reserved  Open（外側2車線のみ供用）

oooo → ooxx の変化:
  物理: 変化なし（路盤パーツは Built のまま）
  運用: Open Open Open Open → Open Open Closed Closed
```

この分離により「物理インフラを変えずに運用だけ変える（一時閉鎖・シフト）」と「物理インフラを拡張しつつ運用も変える（拡幅工事）」を明確に区別できる。

---

## 2. 列挙型の定義

```cpp
// 物理状態: 路盤・構造物の建設状態（原則として不可逆に進行）
// RoadPart に所属する（07 と 16 で共用）
enum class BuildState : uint8 {
    NotBuilt,          // 路盤なし（計画のみ）
    UnderConstruction, // 施工中（路盤未完成）
    Built,             // 路盤完成（供用可能な状態）
    StubEnd,           // 延伸端・イカの耳（構造物はあるが未接続）
};

// 運用状態: 現在の交通への供用状態（頻繁に変化しうる）
// Lane に所属する
enum class OpState : uint8 {
    Open,        // 供用中（通常白線・標識）
    Provisional, // 暫定供用（白線・センターラインが暫定仕様）
    Closed,      // 閉鎖（コーン・バリケード設置）
    Reserved,    // 将来供用のため確保（車両進入不可、コーンなし）
};

// 向き: 運用軸に属する（シフト時に変更可能）
enum class LaneDir : uint8 {
    Forward,   // A → B
    Backward,  // B → A
};

// 機能種別
enum class LaneType : uint8 {
    Normal,
    Overtaking,
    Acceleration,    // 合流部加速車線
    Deceleration,    // 分岐部減速車線
    TurnLeft,
    TurnRight,
    Bus,
    ParkingBay,
    EmergencyStop,
};

// 区画線種別
enum class LineType : uint8 {
    None,           // 線なし
    SolidWhite,     // 白実線（車線変更禁止）
    DashedWhite,    // 白破線（車線変更可）
    SolidYellow,    // 黄実線（追い越し禁止）
    DoubleYellow,   // 黄二重線
};
```

---

## 3. Lane 構造体

```cpp
struct Lane {
    // --- 幾何（A端・B端で異なる位置を持てる → テーパー車線対応） ---
    float   offsetA_L;     // A端: 道路中心からの左端 [m]（左がマイナス）
    float   offsetA_R;     // A端: 道路中心からの右端 [m]
    float   offsetB_L;     // B端: 道路中心からの左端 [m]
    float   offsetB_R;     // B端: 道路中心からの右端 [m]

    // --- 運用 ---
    LaneDir   dir;         // 走行方向（Forward: A→B / Backward: B→A）
    OpState   op;          // 現在の供用状態

    // --- 車線変更 ---
    bool      canChangeLaneLeft;   // 左隣の車線への変更が可能か
    bool      canChangeLaneRight;  // 右隣の車線への変更が可能か

    // --- 区画線 ---
    LineType  lineLeft;    // 左側の区画線種別
    LineType  lineRight;   // 右側の区画線種別

    // --- ゲームプレイ ---
    float     nominalWidth;  // 公称幅 [m]（容量計算・UI表示用）
    LaneType  type;          // 機能種別
};
```

### 幾何の補間

任意の弧長位置 `s`（0 = A端、length = B端）における車線の左右端:

```cpp
float t = s / edge.length;
float left  = Lerp(lane.offsetA_L, lane.offsetB_L, t);
float right = Lerp(lane.offsetA_R, lane.offsetB_R, t);
// 幅 = right - left
```

これにより合流車線・分岐車線のテーパー形状を自然に表現できる。

### テーパー車線の例

```
合流加速車線:
  A端:  offsetA_L=5.5  offsetA_R=9.0   → 幅 3.5m
  B端:  offsetB_L=9.0  offsetB_R=9.0   → 幅 0m（消滅）
  type = Acceleration

分岐減速車線:
  A端:  offsetA_L=5.5  offsetA_R=5.5   → 幅 0m（出現）
  B端:  offsetB_L=5.5  offsetB_R=9.0   → 幅 3.5m
  type = Deceleration
```

### o/x との対応

```cpp
bool isPassable(const RoadEdge& edge, int laneIndex) {
    // 路盤パーツが Built であること
    bool roadbedBuilt = false;
    for (const auto& part : edge.parts) {
        if (part.type() == RoadPartType::Roadbed && part.build == BuildState::Built) {
            roadbedBuilt = true;
            break;
        }
    }
    const auto& lane = edge.lanes[laneIndex];
    return roadbedBuilt
        && (lane.op == OpState::Open || lane.op == OpState::Provisional);
}
// true → o   false → x
```

---

## 4. RoadEdge 構造体

```cpp
struct RoadEdge {
    int      id;
    int      nodeA, nodeB;
    Vec3     ctrlA, ctrlB;       // ベジェ制御点
    RoadType roadType;
    float    speedLimit;
    float    length;              // 弧長 [m]
    int      planId;              // 所属 RoadPlan（-1 = 既存道路）

    // 物理構造（左端から右端の順）
    // 正式定義は 16_road_cross_section_spec.md 参照
    Array<RoadPart> parts;

    // 車線配列（左端から右端の順）
    Array<Lane> lanes;

    // 一時的な運用変更（工事・イベント・シフト）
    Array<TempOp> tempOps;

    // 将来の計画的変化
    Array<PlannedChange> planned;

    // 交通状態
    Array<Array<int>> laneVehicles;  // [laneIndex] → vehicleIds
};
```

---

## 5. 一時的な運用変更（TempOp）

運用軸のみを一時的に上書きする。物理軸（RoadPart）は変更しない。

```cpp
struct LaneOpOverride {
    int     laneIndex;   // 対象車線
    LaneDir newDir;      // 向きの変更（変更しない場合は現在値）
    OpState newOp;       // 運用状態の変更
};

enum class TempOpKind : uint8 {
    Construction,   // 工事による車線閉鎖・シフト
    CrossingClose,  // 踏切閉鎖（全車線 Closed）
    Event,          // 祭り・交通規制など
};

struct TempOp {
    TempOpKind            kind;
    Array<LaneOpOverride> overrides;
    GameTime              start;
    GameTime              end;        // 経過後は自動的に lanes[] の基本値に戻る
    String                reason;     // 表示用（"施工中・片側2車線通行" 等）
};
```

### TempOp スタック管理

RoadEdge は複数の TempOp を同時に保持できる。適用優先度は以下の順（高→低）:

  1. CrossingClose  // 踏切閉鎖（LevelCrossing 専用）
  2. Construction   // 工事
  3. Event          // イベント・規制

### 有効な車線状態の取得

```cpp
Lane RoadEdge::effectiveLane(int i) const {
    Lane L = lanes[i];
    // tempOps を kind の優先度順にソートして適用
    auto sorted = tempOps;
    sorted.sort([](const TempOp& a, const TempOp& b) {
        return (int)a.kind < (int)b.kind;
    });
    for (auto& op : sorted) {
        if (op.start <= now && now <= op.end) {
            for (auto& ov : op.overrides) {
                if (ov.laneIndex == i) {
                    L.dir = ov.newDir;
                    L.op  = ov.newOp;
                }
            }
        }
    }
    return L;
}

Array<int> RoadEdge::openLanes(LaneDir dir) const {
    Array<int> result;
    for (int i = 0; i < lanes.size(); ++i) {
        auto L = effectiveLane(i);
        if (isPassable(*this, i) && L.dir == dir)
            result << i;
    }
    return result;
}
```

---

## 6. 将来の計画的変化（PlannedChange）

物理軸（RoadPart の BuildState）と運用軸（Lane の dir・op）の両方を変更できる。RoadPlan の着工・開通トリガーで発火する。

```cpp
struct PartChange {
    int                    partIndex;
    Optional<BuildState>   newBuild;   // None = 変更しない
};

struct LaneChange {
    int                    laneIndex;
    Optional<LaneDir>      newDir;     // None = 変更しない
    Optional<OpState>      newOp;      // None = 変更しない
};

enum class Trigger { OnConstruction, OnOpen };

struct PlannedChange {
    int                planId;
    Trigger            trigger;
    Array<PartChange>  partChanges;
    Array<LaneChange>  laneChanges;
};
```

---

## 7. 状態表現の例

### 記法

```
parts: [type: build] を左から右に並べて表記
lanes: [dir op] を左から右に並べて表記
build: B=Built U=UnderConstruction N=NotBuilt S=StubEnd
op:    O=Open P=Provisional C=Closed R=Reserved
dir:   →=Forward ←=Backward
```

---

### oxxo: 4車線路盤済み、外側2車線のみ供用（暫定1+1）

```
parts:
  [Slope:B] [Roadbed:B] [Slope:B]
  → 路盤は全幅建設済み

lanes:
  [← P]   [← R]   [→ R]   [→ P]
    o        x        x        o

外側2車線のみ暫定供用（Provisional）、内側2車線は Reserved
```

---

### ooxx: 左2車線のみ供用

```
parts:
  [Slope:B] [Roadbed:B] [Slope:B]

lanes:
  [← O]   [→ O]   [→ C]   [← C]
    o        o        x        x

右2車線は閉鎖中（Closed）
```

---

### oooo → ooxx: 全供用から右2車線を閉鎖（施工開始）

```
基本構成（oooo）:
  lanes: [← O]  [→ O]  [→ O]  [← O]

tempOp 適用後（ooxx）:
  overrides:
    { laneIndex=2, newDir=→, newOp=Closed }
    { laneIndex=3, newDir=←, newOp=Closed }
  reason: "拡幅工事開始"

結果:
  lanes: [← O]  [→ O]  [→ C]  [← C]
           o       o       x       x

工事終了後は tempOp が無効化され、自動的に oooo に戻る。
```

---

### oxxo → 2+0 シフト（施工中に上り2車線に集約）

```
基本構成（oxxo）:
  lanes: [← P]  [← R]  [→ R]  [→ P]

tempOp（上り2車線化）:
  overrides:
    { laneIndex=1, newDir=←, newOp=Open  }  ← Reserved → Open に昇格
    { laneIndex=2, newDir=←, newOp=Open  }  ← 向きも Forward → Backward に転換
    { laneIndex=3, newDir=→, newOp=Closed}  ← 下り車線を閉鎖

結果（←←←×）:
  lanes: [← P]  [← O]  [← O]  [→ C]
           o       o       o       x
（下りは片交信号制御 or 別ルート誘導）
```

---

### イカの耳（stub）

```
parts:
  [Roadbed:S]   ← 路盤・橋台あり、未接続（StubEnd）

lanes:
  [← R]  [→ R]

BuildState=StubEnd なので isPassable() = false → 車両進入なし
視覚: 橋台・法面・バリケードを描画
```

---

### 延伸端部（NotBuilt）

```
parts:
  [Roadbed:N]   ← 路盤なし（計画のみ）

lanes:
  [← R]  [← R]  [→ R]  [→ R]

planId で対応する延伸 RoadPlan に紐づく
planned:
  { planId=15, trigger=OnConstruction,
    partChanges=[{partIndex=0, newBuild=UnderConstruction}] }
  { planId=15, trigger=OnOpen,
    partChanges=[{partIndex=0, newBuild=Built}],
    laneChanges=[
      {laneIndex=0..3, newOp=Open}
    ] }
```

---

### 暫定1+1 → 本格2+2 への段階供用

```
基本構成（oxxo / 暫定）:
  lanes: [← P]  [← R]  [→ R]  [→ P]

planned（「飯松バイパス拡幅計画」開通時）:
  { planId=12, trigger=OnOpen,
    laneChanges=[
      { laneIndex=0, newOp=Open },   // Provisional → Open（本供用）
      { laneIndex=1, newOp=Open },   // Reserved → Open
      { laneIndex=2, newOp=Open },
      { laneIndex=3, newOp=Open },
    ] }

開通後（oooo / 本格供用）:
  lanes: [← O]  [← O]  [→ O]  [→ O]
```

---

## 8. 道路容量への影響

```cpp
int nForward  = edge.openLanes(LaneDir::Forward).size();
int nBackward = edge.openLanes(LaneDir::Backward).size();

float capForward  = nForward  * laneCapacity(edge.roadType);  // 台/h
float capBackward = nBackward * laneCapacity(edge.roadType);
```

- `oxxo` → Forward 1車線、Backward 1車線
- `ooxx` → Forward 1車線（or 2）、Backward 0（経路探索が自動的に迂回）
- 2+0 シフト時 → Forward 0 → 下り方向の車両が全て迂回路へ

---

## 9. 設計上の制約と注意点

- `RoadPart.build` の変更は必ず `PlannedChange` 経由（直接書き換えは禁止）
- `tempOps` は1エッジに複数保持可能。適用優先度は CrossingClose > Construction > Event（§5 参照）
- `Lane.dir`・`Lane.op` の直接変更は `TempOp` か `PlannedChange` のみ（シミュレーション中の直接書き換えは禁止）
- 路盤パーツの `build != Built` のとき、その上の車線は全て通行不可（`isPassable()` = false）
- 車線の `offsetA_L` / `offsetA_R` / `offsetB_L` / `offsetB_R` は路盤パーツの幅の範囲内であること
- `LineType` は車線ごとに左右個別に指定する。隣接車線の境界では、左の車線の `lineRight` と右の車線の `lineLeft` が同じ位置に描画されるため、一方を `None` にするか同じ値にすること
- ノードでの接続（継ぎ目・交差点・分岐合流）の詳細は `17_road_node_spec.md` を参照


---

## 10. 路面標示矢印（RoadArrow）

交差点進入時の進路指示用に、車線中心線上に矢印メッシュを描画する。

### 矢印タイプ

```cpp
enum class RoadArrowType : uint8
{
    None,             ///< 矢印なし
    Straight,         ///< 直進 (ht2 相当)
    Left,             ///< 左折 (ht1 相当)
    Right,            ///< 右折 (ht1 を上下反転)
    StraightLeft,     ///< 直進+左折 (ht3 相当)
    StraightRight,    ///< 直進+右折 (ht3 を上下反転)
    LeftRight,        ///< 左折+右折 (将来)
    All,              ///< 直進+左折+右折 (将来)
    UTurn,            ///< U ターン (将来)
};
```

### 形状データの出典

`reference/204.ht{1,2,3}.gif` から OpenCV `findContours` + `approxPolyDP` で抽出した多角形を `src/road/RoadArrow.cpp` 内に定数として埋め込む。
縦横とも等方スケール（1px ≈ 1cm）で正規化する。横寸法（perpendicular）は各矢印の px Y 範囲 × 1cm/px から自動決定される（直進 ≈ 0.529m、左/右折 ≈ 0.773m、直進+左/右折 ≈ 0.928m）。

### 配置ルール

- 表示条件: 3本以上のエッジが集まる交差点ノードのみ（行き止まり・単純継ぎ目には表示しない）
- 配置単位: ノード進入側 entry レーン1本につき1個
- 配置位置: ノード境界（cutoff 位置）から進行方向と逆向きに 8.0m
- 向き: レーン中心線の接線方向（矢印先端 = +X = 進行方向）
- 描画条件: `Lane.op == Open || Provisional` のレーンのみ

### 自動推論

`RoadArrow::InferType(network, edgeId, laneIndex, towardNodeId)` が以下の手順で矢印タイプを決定する。

1. 当該レーンから出る `LaneConnection` を `node.laneConnections` から抽出
2. 各 connection の旋回種別を `TrafficCommon::classifyTurnByAngles`（45° ルール）で分類
3. UTurn は `conn.toEdgeId == conn.fromEdgeId` のトポロジ条件を優先
4. 出口の方向集合 → ArrowType マッピング:
   - {Straight} → Straight
   - {Left} → Left
   - {Right} → Right
   - {Straight, Left} → StraightLeft
   - {Straight, Right} → StraightRight
   - {Left, Right} → LeftRight
   - {Straight, Left, Right} → All
   - 接続なし → None

### キャッシュ

`RoadRenderer::m_laneArrowCache[nodeId]` に `Array<LaneLineBatch>` として保持。`eraseNodeCaches(nodeId)` で停止線と一緒に無効化される。

### 描画

`drawNodeCap()` 内で停止線の直後に白色（`ColorF{1,1,1}`）で描画する。LOD 距離 800m 以遠では描画スキップ（停止線と同様）。


---

## 11. 道路標識（RoadSign）

物理的な道路標識（ポール+看板）を `RoadEdge.signs` に持たせる。

### 標識タイプ

```cpp
enum class RoadSignType : uint8
{
    None,
    Stop,           ///< 一時停止 (規制標識 330) - 逆三角形・赤地
    SpeedLimit,     ///< 最高速度 - 円形・白地・赤縁 (将来)
    NoEntry,        ///< 進入禁止 (将来)
    OneWay,         ///< 一方通行 (将来)
    Yield,          ///< 譲れ (将来)
};
```

### 配置データ

```cpp
struct RoadSignPlacement
{
    RoadSignType type          = RoadSignType::None;
    int          nodeEndId     = -1;     ///< edge.nodeA か edge.nodeB のどちらか
    float        arcOffset     = 0.0f;   ///< nodeEnd 端からの追加弧長 [m]（0=cutoff 位置）
    float        lateralOffset = 0.0f;   ///< 道路中心からの横方向 [m] (負=左側 in A→B 方向)
    float        poleHeight    = 2.5f;   ///< ポール高さ [m]
    float        yawOffset     = 0.0f;   ///< 看板の向き調整 [rad]
    int          auxValue      = 0;      ///< SpeedLimit の速度値など
    bool         autoGenerated = true;   ///< 自動生成エントリか手動配置か
};
```

`RoadEdge` に `Array<RoadSignPlacement> signs` を追加。

### 自動生成ルール（v1: Stop のみ）

`attachment.control == TrafficControl::Stop` のノード端について、
進入車から見て **左側**（=道路端 + lateralMargin）の **cutoff 位置** に Stop 標識を配置する。

```
/// nodeEnd で攻撃方向側(driver の左)を判定:
///   nodeEndId == edge.nodeB → driver は A→B 方向 → driver の左 = lateralOffset 負
///   nodeEndId == edge.nodeA → driver は B→A 方向 → driver の左 = lateralOffset 正
```

### 永続化

- セーブ形式 v8 で `signs` を read/write
- 旧 v7 セーブも読み込み可（signs フィールドは空配列で初期化、`recomputeAutoSigns()` で自動補完）

### 自動再生成のタイミング

`RoadNetwork::recomputeAutoSignsForEdge(edgeId)` を以下で呼び出す:

- セーブロード後（v7 移行を含む）
- `attachment.control` 変更時（GameScene_Panels の信号トグル等）
- エッジ追加・削除時

手動配置（`autoGenerated = false`）のエントリは保持され、自動エントリのみ再生成される。

### v1 メッシュ

- ポール: シリンダー φ60mm × 高さ poleHeight、灰色
- 看板: 一辺 0.8m の **逆三角形**（頂点が下）、赤地
- 看板中心高: ポール頂上付近（仮: 路面 + poleHeight - 0.4m）
- 文字「止まれ」は v2 でテクスチャ化

### 描画

`RoadRenderer::m_signMeshCache[edgeId]` に `Array<SignMesh>` を保持。`drawEdge()` で描画。
LOD 800m 以遠ではスキップ。
