# 経路探索仕様書

## 概要

車線単位のダイクストラ法により、車両の走行経路（どのエッジの、どの車線を通るか）を決定する。

フェーズ1から始め、フェーズ2（階層型）へ移行できるよう、**BorderNode の概念を最初から組み込む**。

---

## 1. グラフの構成要素

### LaneNode（車線ノード）

経路探索グラフの基本ノード。「あるエッジの、ある車線の、ある端点」を表す。

```cpp
struct LaneNode {
    int   edgeId;
    int   laneIndex;
    float arcPos;     // エッジ上の弧長位置 [m]（0.0 = 始点, length = 終点）
    int   chunkId;    // 所属チャンク（arcPos から計算）

    // グラフの辺（接続先 LaneNode or BorderNode の ID リスト）
    Array<GraphEdge> outgoing;
};
```

### BorderNode（境界ノード）

RoadEdge がチャンク境界をまたぐ地点に自動生成されるノード。LaneNode と同じ経路探索グラフに参加する。フェーズ2でキャッシュを持つようになる。

```cpp
struct BorderNode {
    int   edgeId;
    int   laneIndex;
    float arcPos;      // チャンク境界と交差する弧長位置
    int   chunkFrom;   // arcPos より手前のチャンク
    int   chunkTo;     // arcPos より先のチャンク
    Vec3  position;    // 3D 座標

    // フェーズ2で使用（フェーズ1では常に dirty=true のまま放置でよい）
    float cachedCostFrom; // chunkFrom 側の最短コストキャッシュ
    float cachedCostTo;   // chunkTo 側の最短コストキャッシュ
    bool  dirty;          // キャッシュ無効フラグ
};
```

### GraphEdge（グラフの辺）

```cpp
enum class GraphEdgeType {
    Forward,        // 同一車線を弧長方向に前進
    LaneChange,     // 同一エッジ内の隣接車線へ横移動
    Transition,     // LaneConnection 経由で次エッジの車線へ
    BorderCross,    // BorderNode を経由してチャンクをまたぐ
};

struct GraphEdge {
    GraphEdgeType type;
    int           toNodeId;    // 接続先 LaneNode or BorderNode の ID
    float         cost;        // 移動コスト [秒]
    bool          dirty;       // 混雑度変化などでコスト再計算が必要
};
```

---

## 2. コスト関数

```cpp
// Forward コスト: 区間距離 ÷ 実効速度
float costForward(int edgeId, int laneIndex, float arcFrom, float arcTo) {
    auto& edge = edges[edgeId];
    float dist  = arcTo - arcFrom;
    float speed = edge.speedLimit
                * (1.0f - edge.congestion * 0.8f)   // 混雑による減速
                * laneSpeedFactor(edge.lanes[laneIndex].type); // 車線種別補正
    return dist / speed;
}

// LaneChange コスト: 固定ペナルティ
const float kLaneChangeCost = 5.0f;   // 秒

// 交差点ノードにおける車線間接続を表す
struct LaneConnection {
    int       nodeId;       // 交差点 RoadNode の ID
    int       fromEdgeId;   // 進入エッジ
    int       fromLaneIdx;  // 進入車線インデックス
    int       toEdgeId;     // 退出エッジ
    int       toLaneIdx;    // 退出車線インデックス
    TurnType  turn;         // Straight / Left / Right / UTurn
};

enum class TurnType : uint8 {
    Straight,
    Left,
    Right,
    UTurn,
};

// Transition コスト: 交差点遅延
float costTransition(const LaneConnection& conn) {
    if (hasSignal(conn.nodeId))
        return expectedWaitTime(conn.nodeId, conn.inLane, conn.turn);
    switch (conn.turn) {
        case TurnType::Straight: return 2.0f;
        case TurnType::Left:     return 5.0f;
        case TurnType::Right:    return 8.0f;  // 対向をまたぐ右折
        case TurnType::UTurn:    return 15.0f;
    }
}

// BorderCross コスト: 0（BorderNode は位置の区切りであり移動ではない）
const float kBorderCrossCost = 0.0f;
```

---

## 3. グラフの構築

### エッジ登録時の BorderNode 自動生成

道路エッジを追加するとき、ベジェ曲線とチャンク境界との交点を検出し BorderNode を生成する。

```
RoadEdge 追加
  ↓
ベジェ曲線を N 分割してチャンク境界との交差判定
  ↓
交差点ごとに BorderNode を生成（車線ごと）
  ↓
LaneNode → BorderNode → LaneNode の順に GraphEdge を接続
```

```
[エッジ A: チャンク1→2→3 をまたぐ]

ChunkBoundary(1|2)         ChunkBoundary(2|3)
       │                          │
(lane0, arcPos=0) ──[F]──> BN(1|2, lane0) ──[F]──> BN(2|3, lane0) ──[F]──> (lane0, arcPos=length)
                                  │                          │
                          chunkFrom=1               chunkFrom=2
                          chunkTo=2                 chunkTo=3
```

### LaneConnection の自動生成

RoadNode に接続する RoadEdge の組み合わせごとに LaneConnection を生成する。

TurnType の判定:
  進入エッジの終端方向ベクトル と 退出エッジの始端方向ベクトル の内積・外積から計算
  内積 > 0.7   → Straight
  外積 > 0     → Left（日本: 左折は対向なし）
  外積 < 0     → Right（日本: 右折は対向をまたぐ）
  内積 < -0.7  → UTurn

車線の対応:
  進入エッジの右端車線（最右 open lane）→ 右折の LaneConnection
  進入エッジの左端車線（最左 open lane）→ 左折の LaneConnection
  それ以外 → Straight に接続

禁止接続:
  同一エッジへの折返し（UTurn 禁止交差点）はコスト = ∞
  大型車・バスの右端車線への接続はコスト加算（laneUsagePenalty を参照）

### グラフ更新のトリガー

| イベント | 処理 |
|---------|------|
| 道路新設（RoadPlan 開通） | 新エッジの LaneNode・BorderNode を追加 |
| 道路廃止 | 対応ノードを削除、接続を切断 |
| `tempOp` 適用・解除 | 対象エッジの open 車線が変化 → LaneNode の有効/無効を更新 |
| `PlannedChange` 発火 | 上に同じ |
| 混雑度変化 | `GraphEdge.dirty = true`（コストの遅延再計算） |

---

## 4. フェーズ1：単一グラフ方式

アクティブチャンク内の全 LaneNode + BorderNode を**ひとつのグラフ**として扱い、ダイクストラを実行する。

```cpp
struct PathfindingGraph {
    HashMap<int, LaneNode>   laneNodes;    // id → LaneNode
    HashMap<int, BorderNode> borderNodes;  // id → BorderNode

    // フェーズ1: アクティブ範囲の全ノードが格納されている
    // フェーズ2: 追加で chunkGraph を保持（下記参照）
};
```

### ダイクストラの実行

```cpp
struct PathResult {
    Array<int> nodeIds;   // LaneNode / BorderNode の ID 列
    float      totalCost;
};

PathResult dijkstra(int startLaneNodeId, int goalEdgeId) {
    // 優先度付きキュー（コスト昇順）
    PriorityQueue<pair<float, int>> queue;
    HashMap<int, float> dist;
    HashMap<int, int>   prev;

    dist[startLaneNodeId] = 0.0f;
    queue.push({0.0f, startLaneNodeId});

    while (!queue.empty()) {
        auto [d, u] = queue.top(); queue.pop();
        if (d > dist[u]) continue;

        for (auto& ge : outgoingEdges(u)) {
            // closed / unusable な車線への辺はスキップ
            if (!isNodePassable(ge.toNodeId)) continue;

            float c = d + currentCost(ge);   // dirty なら再計算
            if (c < dist[ge.toNodeId]) {
                dist[ge.toNodeId] = c;
                prev[ge.toNodeId] = u;
                queue.push({c, ge.toNodeId});
            }
        }
    }
    return reconstructPath(prev, startLaneNodeId, goalEdgeId);
}
```

### 計算量の見積もり

```
アクティブチャンク: 5×5 = 25チャンク
チャンク内 LaneNode 平均: 400
BorderNode 平均: 40 / チャンク境界辺
→ 総ノード数: 25×400 + 25×4×40 / 2 ≒ 12,000

ダイクストラ 1回: O((V+E) log V) ≒ O(40,000 × 14) ≒ 560K 操作 ≈ 0.5ms
車両 300台 × 毎分 1 回再探索 → 5 回/秒 ≒ 2.5ms/秒

→ フレーム予算内で十分
```

---

## 5. フェーズ2：階層型への移行

フェーズ1のコードに**チャンク間グラフ（Level 0）を追加する**だけで移行できる。既存の LaneNode・BorderNode の構造は変更不要。

### チャンク間グラフ（Level 0）

```cpp
struct ChunkGraph {
    // ノード: BorderNode の ID（フェーズ1から引き継ぎ）
    // 辺: チャンク内の最短経路コスト（事前計算・キャッシュ）
    HashMap<pair<int,int>, float> intraChunkCost;
    // key: (borderNodeIdA, borderNodeIdB)  ← 同一チャンク内の2境界ノード間
    // value: 最短コスト（dirty フラグで無効化・再計算）
};
```

### フェーズ2の経路探索フロー

```
① Level 0（チャンク間）: チャンク間グラフで大まかな経由チャンク列を決定
        BorderNode のみを使って Dijkstra
        → 経由 BorderNode 列: [BN_A, BN_B, BN_C]

② Level 1（チャンク内）: 各チャンク内を詳細に探索
        BN_A → BN_B のチャンク内パス
        BN_B → BN_C のチャンク内パス
        出発点 → BN_A のチャンク内パス
        BN_C → 目的地 のチャンク内パス

③ スティッチング: 各チャンク内パスを BorderNode でつなげて完全な経路を生成
```

### キャッシュの無効化

```
道路変化イベント発生（新設・閉鎖・tempOp）
  ↓
影響チャンクの全 BorderNode に dirty=true をセット
  ↓
次回 Level 0 参照時に intraChunkCost を再計算
  ↓
Level 0 グラフの対応辺コストを更新
```

---

## 6. 車両の経路表現

```cpp
struct Vehicle {
    // 現在位置
    int   currentEdge;
    int   currentLane;    // Lane.index
    float arcPos;

    // 経路（ダイクストラ結果）
    Array<int> routeNodeIds;   // LaneNode / BorderNode の ID 列
    int        routeProgress;  // 現在どこまで消化したか

    // 再探索管理
    GameTime  lastReroute;
    float rerouteSpeedThreshold;  // この速度以下で再探索を検討
};
```

### 経路の消化

```
現在ノードに到達
  ↓
routeNodeIds[routeProgress] が BorderNode なら:
  → チャンク境界を越えた記録（フェーズ2でキャッシュ活用）
  → currentChunk を更新
routeNodeIds[routeProgress] が LaneNode なら:
  → currentEdge・currentLane・arcPos を更新
  → IDM で加速度計算
routeProgress += 1
```

---

## 7. 再探索のトリガーと分散

全車両が同一フレームで再探索するとスパイクが起きるため、分散させる。

```cpp
void TrafficManager::update(float dt) {
    // 再探索キューを毎フレーム N 台ずつ処理
    const int kReroutePerFrame = 10;
    int processed = 0;

    for (auto& v : rerouteQueue) {
        if (processed >= kReroutePerFrame) break;
        v.route = dijkstra(v.currentLaneNodeId(), v.goalEdgeId);
        v.lastReroute = gameClock.now();
        processed++;
    }
}

// 再探索キューへの追加条件
void Vehicle::considerReroute() {
    // 条件1: 速度が閾値以下（渋滞）かつランダム判定
    if (speed < rerouteSpeedThreshold && randomChance(0.02f))
        addToRerouteQueue(this);

    // 条件2: tempOp 変化イベントを受信
    if (receivedNetworkChangeEvent)
        addToRerouteQueue(this);

    // 条件3: 前回再探索から一定時間経過（定期更新）
    if (gameClock.now() - lastReroute > kPeriodicRerouteInterval)
        addToRerouteQueue(this);
}
```

---

## 8. フェーズ移行チェックリスト

フェーズ1実装時に必ず行うこと（フェーズ2移行を妨げないために）:

- [ ] `BorderNode` を道路新設時に必ず生成する（フェーズ1では `dirty=true` のまま放置でよい）
- [ ] `LaneNode` / `BorderNode` の ID を統一したルックアップで管理する（`PathfindingGraph` に集約）
- [ ] ダイクストラを `PathfindingGraph` 経由で呼び出す（グラフ実装に依存しないインターフェース）
- [ ] グラフ更新をイベントドリブンにする（`dirty` フラグ + 差分更新）

フェーズ2への移行は `ChunkGraph` を追加し、`dijkstra()` の内部実装を 2 レベルに切り替えるだけ。外部（Vehicle AI）のコードは変更不要。
