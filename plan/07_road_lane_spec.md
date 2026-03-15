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

---

## 1. 2軸分離の設計思想

各車線の状態を **物理軸** と **運用軸** に分離する。

```
o/x 表記の定義:
  o = 車両が走行できる車線（供用中）
  x = 走行できない車線（閉鎖・予約・未建設など）

oxxo の内訳:
  物理: Built  Built  Built  Built   ← 路盤は全車線完成
  運用: Open  Closed Closed  Open   ← 外側2車線のみ供用

oooo → ooxx の変化:
  物理: 変化なし（全車線 Built のまま）
  運用: Open Open Open Open → Open Open Closed Closed
```

この分離により「物理インフラを変えずに運用だけ変える（一時閉鎖・シフト）」と「物理インフラを拡張しつつ運用も変える（拡幅工事）」を明確に区別できる。

---

## 2. 列挙型の定義

```cpp
// 物理状態: 路盤・構造物の建設状態（原則として不可逆に進行）
enum class BuildState : uint8 {
    NotBuilt,          // 路盤なし（計画のみ）
    UnderConstruction, // 施工中（路盤未完成）
    Built,             // 路盤完成（供用可能な状態）
    StubEnd,           // 延伸端・イカの耳（構造物はあるが未接続）
};

// 運用状態: 現在の交通への供用状態（頻繁に変化しうる）
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

// 機能種別: 物理軸に属する（構造に紐づく）
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
    StubReserved,    // イカの耳専用
};
```

---

## 3. Lane 構造体

```cpp
struct Lane {
    // --- 物理軸（原則として変更されない） ---
    int        index;     // 左端=0 の物理位置（不変）
    BuildState build;     // 路盤・構造物の建設状態
    LaneType   type;      // 機能種別
    float      width;     // 車線幅 [m]

    // --- 運用軸（頻繁に変わりうる） ---
    LaneDir    dir;       // 現在の向き（シフト時に変更）
    OpState    op;        // 現在の運用状態

    // build != Built のとき dir・op は無効
};
```

### o/x との対応

```cpp
bool isPassable(const Lane& lane) {
    return lane.build == BuildState::Built
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
    Vec3     ctrlA, ctrlB;
    RoadType roadType;
    float    speedLimit;
    float    length;            // 弧長 [m]
    int      planId;            // 所属 RoadPlan（-1 = 既存道路）

    // 車線配列（左端=index 0 の物理順）
    Array<Lane> lanes;

    // 一時的な運用変更（工事・イベント・シフト）
    Array<TempOp> tempOps;

    // 将来の計画的変化
    Array<PlannedChange> planned;

    // 交通状態
    Array<Array<int>> laneVehicles;  // [index] → vehicleIds
};
```

---

## 5. 一時的な運用変更（TempOp）

運用軸のみを一時的に上書きする。物理軸は変更しない。

```cpp
struct LaneOpOverride {
    int     index;       // 対象車線
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
    GameTime              end;          // 経過後は自動的に lanes[] の基本値に戻る
    String                reason;       // 表示用（"施工中・片側2車線通行" 等）
};

Array<TempOp> tempOps;  // lanes[] と同じ RoadEdge のフィールド
```

### TempOp スタック管理

RoadEdge は複数の TempOp を同時に保持できる。適用優先度は以下の順（高→低）:

  1. CrossingClose  // 踏切閉鎖（LevelCrossing 専用）
  2. Construction   // 工事
  3. Event          // イベント・規制

### 有効な車線状態の取得

`effectiveLane(int i)` の処理:
  `tempOps` を kind の優先度順にソートし、上位の override を適用する。
  CrossingClose は全車線を Closed に上書きするため、工事状態を消さない。

```cpp
Lane RoadEdge::effectiveLane(int i) const {
    Lane L = lanes[i];
    // tempOps を kind の優先度順にソートして適用
    auto sorted = tempOps;
    sorted.sort([](const TempOp& a, const TempOp& b) {
        return (int)a.kind < (int)b.kind;  // CrossingClose(1) > Construction(0) > Event(2)
    });
    for (auto& op : sorted) {
        if (op.start <= now && now <= op.end) {
            for (auto& ov : op.overrides) {
                if (ov.index == i) {
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
        if (isPassable(L) && L.dir == dir)
            result << i;
    }
    return result;
}
```

---

## 6. 将来の計画的変化（PlannedChange）

物理軸・運用軸の両方を変更できる。RoadPlan の着工・開通トリガーで発火する。

```cpp
struct LaneChange {
    int                    index;
    Optional<BuildState>   newBuild;   // None = 変更しない
    Optional<LaneDir>      newDir;     // None = 変更しない
    Optional<OpState>      newOp;      // None = 変更しない
};

enum class Trigger { OnConstruction, OnOpen };

struct PlannedChange {
    int           planId;
    Trigger       trigger;
    Array<LaneChange> changes;
};
```

---

## 7. 状態表現の例

### 記法

```
[index: build dir op] を左から右に並べて表記
build: B=Built U=UnderConstruction N=NotBuilt S=StubEnd
op:    O=Open P=Provisional C=Closed R=Reserved
dir:   →=Forward ←=Backward
```

---

### ◆ oxxo：4車線路盤済み、外側2車線のみ供用（暫定1+1）

```
lanes:
  [0: B → P]   o  ← 暫定供用（Forward）
  [1: B → R]   x  ← 将来供用用に確保
  [2: B ← R]   x  ← 将来供用用に確保
  [3: B ← P]   o  ← 暫定供用（Backward）

    o  x  x  o
   [→][→][←][←]  ← 路盤の物理的な向きの設計意図
    P  R  R  P   ← 運用状態
```

---

### ◆ ooxx：左2車線のみ供用

```
lanes:
  [0: B → O]   o
  [1: B ← O]   o
  [2: B → C]   x  ← 閉鎖中（コーン設置）
  [3: B ← C]   x  ← 閉鎖中

    o  o  x  x
    O  O  C  C
```

---

### ◆ oooo → ooxx：全供用から右2車線を閉鎖（施工開始）

```
基本構成（oooo）:
  [0: B → O]  [1: B ← O]  [2: B → O]  [3: B ← O]

tempOp 適用後（ooxx）:
  overrides:
    { index=2, newDir=→, newOp=Closed }
    { index=3, newDir=←, newOp=Closed }
  reason: "拡幅工事開始"

結果:
  [0: B → O]  [1: B ← O]  [2: B → C]  [3: B ← C]
       o            o            x            x
```

工事終了後は tempOp が無効化され、自動的に oooo に戻る。

---

### ◆ oxxo → 2+0 シフト（施工中に上り2車線に集約）

```
基本構成（oxxo）:
  [0: B → P]  [1: B → R]  [2: B ← R]  [3: B ← P]

tempOp（上り2車線化）:
  overrides:
    { index=1, newDir=→, newOp=Open  }  ← Reserved → Open に昇格
    { index=2, newDir=→, newOp=Open  }  ← 向きも Backward → Forward に転換
    { index=3, newDir=←, newOp=Closed}  ← 唯一の下り車線も閉鎖

結果（→→→×）:
  [0: B → P]  [1: B → O]  [2: B → O]  [3: B ← C]
       o            o            o            x
（下りは片交信号制御 or 別ルート誘導）
```

---

### ◆ イカの耳（stub）

```
lanes:
  [0: S → R]   x  ← 路盤・橋台あり、未接続
  [1: S ← R]   x

build=StubEnd なので isPassable() = false → 車両進入なし
視覚: 橋台・法面・バリケードを描画
```

---

### ◆ 延伸端部（NotBuilt）

```
lanes:
  [0: N → R]   x  ← 路盤なし（将来の延伸予定）
  [1: N → R]   x
  [2: N ← R]   x
  [3: N ← R]   x

planId で対応する延伸 RoadPlan に紐づく
planned:
  { planId=15, trigger=OnConstruction, changes=[
      {index=0..3, newBuild=UnderConstruction}
  ]}
  { planId=15, trigger=OnOpen, changes=[
      {index=0..3, newBuild=Built, newOp=Open}
  ]}
```

---

### ◆ 暫定1+1 → 本格2+2 への段階供用

```
基本構成（oxxo / 暫定）:
  [0: B → P]  [1: B → R]  [2: B ← R]  [3: B ← P]

planned（「飯松バイパス拡幅計画」開通時）:
  { planId=12, trigger=OnOpen, changes=[
      { index=0, newOp=Open },   // Provisional → Open（本供用）
      { index=1, newOp=Open },   // Reserved → Open
      { index=2, newOp=Open },
      { index=3, newOp=Open },
  ]}

開通後（oooo / 本格供用）:
  [0: B → O]  [1: B → O]  [2: B ← O]  [3: B ← O]
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
- 2+0 シフト時 → Backward 0 → 下り方向の車両が全て迂回路へ

---

## 9. 設計上の制約と注意点

- `index` は作成後に変更しない
- `build` の変更は必ず `PlannedChange` 経由（直接書き換えは禁止）
- `tempOps` は1エッジに複数保持可能。適用優先度は CrossingClose > Construction > Event（§5 参照）
- `dir`・`op` の直接変更は `tempOp` か `PlannedChange` のみ（シミュレーション中の直接書き換えは禁止）
- `build != Built` の車線は `dir`・`op` が未定義（参照禁止）
