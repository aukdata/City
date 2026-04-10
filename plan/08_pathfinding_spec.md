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

// 交差点ノードにおける車線間接続（実データは RoadTypes.hpp の LaneConnection を参照）
// ここではコスト計算に必要な最小構造のみ示す
struct LaneConnection {
    int         nodeId;
    int         fromEdgeId;
    int         fromLaneIndex;
    int         toEdgeId;
    int         toLaneIndex;
    CubicBezier path;         // 旋回パス
};

enum class TurnType : uint8 {
    Straight,
    Left,
    Right,
    UTurn,
};

// Transition コスト: 交差点遅延（LaneConnection 自体にコストは持たせない。
// 極端な遠回りを避けるための目安であって、厳密な通過時間ではない）
float costTransition(const LaneConnection& conn, const RoadNetwork& network) {
    const TurnType turn = classifyTurn(conn, network);  // 45° ルール（下記参照）
    float base;
    switch (turn) {
        case TurnType::Straight: base = 2.0f;  break;
        case TurnType::Left:     base = 5.0f;  break;
        case TurnType::Right:    base = 8.0f;  break;  // 対向をまたぐ右折
        case TurnType::UTurn:    base = 15.0f; break;
    }
    if (hasSignal(conn.nodeId))
        base += expectedWaitTime(conn.nodeId, conn);
    return base;
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

### LaneConnection の参照と Transition 辺の生成

`TrafficGraph::rebuild()` は、経路探索グラフの Transition 辺を **`RoadNode.laneConnections` に実在する接続にのみ** 張る。

```
for (node in allNodes)
    for (conn in node.laneConnections)
        turn = classifyTurn(conn, roadNetwork)
        cost = costTransition(conn, roadNetwork)
        辺を登録:
            from = LaneNode(conn.fromEdgeId, conn.fromLaneIndex, exit 側)
            to   = LaneNode(conn.toEdgeId,   conn.toLaneIndex,   entry 側)
            type = GraphEdgeType::Transition
```

**重要な不変条件**: `RoadNode.laneConnections` に存在しない車線間遷移は、経路探索グラフにも存在しない。これにより「左折専用レーンから直進」のような物理的に不可能な経路が探索結果に含まれないことが保証される。

- LaneConnection の「生成」（どの進入車線からどの退出車線へ接続するか）は道路ネットワーク編集時の幾何的決定であり、経路探索の責務ではない。詳細は `17_road_node_spec.md` を参照
- 経路探索は「与えられた `laneConnections` を尊重する」だけで、接続を作り出さない
- `laneConnections` が 0 個のノードは経路探索上「通過不可」として扱う（将来、実害が出た場合に対応）

### `classifyTurn()` — 旋回分類ヘルパー

```
classifyTurn(conn, network):
    fromTangent = conn.fromEdge の conn.fromLaneIndex 車線が
                  ノードに到達する直前の進行方向ベクトル
    toTangent   = conn.toEdge   の conn.toLaneIndex   車線が
                  ノードを出た直後の進行方向ベクトル
    θ = 符号付き水平角度(fromTangent → toTangent)   // 左が正、右が負

    |θ| <= 45°                 → Straight
    45° < θ  < 135°            → Left
    -135° < θ < -45°           → Right
    |θ| >= 135°                → UTurn
```

「前方 45° を閾値」とする 45° ルール。直進の判定を厳しめに取ることで、急カーブの交差点で「実質曲がっているのに直進扱い」になる誤分類を防ぐ。

このヘルパーは `src/traffic/TrafficCommon.hpp::classifyTurn()` に実装し、以下の 3 箇所で共通使用する:
1. 経路探索 (`TrafficGraph::rebuild()`) — Transition 辺のコスト決定
2. 信号フェーズ自動生成 — 「対向直進を同一フェーズに」等の分類
3. 信号描画 — 矢印サブランプの自動表示判定（`19_vehicle_movement_spec.md` §6 参照）

### 設計原則

- **LaneConnection 自体にコストは持たせない**。Transition 辺のコストは TurnType に基づく固定値 + 信号待ち時間の加算のみ。車線別の細かい重み付けはしない
- 経路のコストは「極端に遠回りしない程度」の精度で十分と割り切る
- 大型車・バス等の車両種別による車線制限は、別途 LaneNode の通行可否（`isNodePassable`）で表現する

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
