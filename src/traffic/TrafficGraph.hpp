#pragma once
#include "../sim/SimGraph.hpp"
#include "TrafficLight.hpp"

// ===== グラフ辺種別 =====

/// @brief 経路探索グラフの辺の種別
enum class GraphEdgeType : uint8
{
	Forward,      ///< 同一車線を弧長方向に前進
	LaneChange,   ///< 同一エッジ内の隣接車線へ横移動
	Transition,   ///< 交差点経由で次エッジの車線へ
	BorderCross,  ///< BorderNode を経由してチャンクをまたぐ
};

/// @brief 経路探索グラフの有向辺
struct GraphEdge
{
	// 探索辺は遷移種別とコストだけを持つ軽量表現にし、Dijkstra 側の扱いを単純化する。
	GraphEdgeType type      = GraphEdgeType::Forward;
	int           toNodeId  = -1;
	float         cost      = 0.0f;
	bool          dirty     = false;
};

// ===== ノード =====

/// @brief 車線ノード（あるエッジの、ある車線の、ある端点）
struct LaneNode
{
	// LaneNode は「エッジ端点上の特定車線」を表し、経路探索の基本単位になる。
	int   id        = -1;
	int   edgeId    = -1;
	int   laneIndex = 0;
	float arcPos    = 0.0f;   ///< 0=エッジ始点, length=エッジ終点

	Array<GraphEdge> outgoing;
};

/// @brief 境界ノード（エッジがチャンク境界をまたぐ地点）
struct BorderNode
{
	// BorderNode は将来のチャンク跨ぎ分割を見据えた拡張点で、現状も同じグラフ上で扱える形にしている。
	int   id        = -1;
	int   edgeId    = -1;
	int   laneIndex = 0;
	float arcPos    = 0.0f;
	int   chunkFrom = -1;
	int   chunkTo   = -1;
	Vec3  position;

	float cachedCostFrom = 0.0f;
	float cachedCostTo   = 0.0f;
	bool  dirty          = true;   ///< Phase 1 では常に true

	Array<GraphEdge> outgoing;
};

// ===== 交差点接続 =====

/// @brief 車線間接続の方向種別
enum class TurnType : uint8
{
	Straight,
	Left,
	Right,
	UTurn,
};

// LaneConnection は RoadTypes.hpp で定義

// ===== 経路探索結果 =====

/// @brief Dijkstra の結果
struct PathResult
{
	Array<int> nodeIds;              ///< LaneNode / BorderNode の ID 列（出発→ゴール順）
	float      totalCost = 1e30f;
	bool       found     = false;
	int        nodesVisited = 0;     ///< 探索したノード数
	int        graphSize    = 0;     ///< グラフのノード総数
};

// ===== TrafficGraph =====

/// @brief 経路探索グラフ（Phase 1: 単一フラットグラフ方式）
/// @details ネットワーク全体の LaneNode + BorderNode を一つのグラフで管理する
class TrafficGraph
{
public:
	// TrafficGraph は SimGraph から生成される探索専用グラフで、経路探索と連結性判定を担当する。
	/// @brief SimGraph からグラフを再構築する
	/// @param lights 信号機マップ（nodeId → TrafficLight）。Transition コストに待ち時間を加算する
	void rebuild(const SimGraph& graph, GameTime now,
	             const HashTable<int, TrafficLight>& lights);

	/// @brief 交差点ノード移動後に、接続エッジの Forward 辺だけを局所更新する
	void updateMovedIntersectionNode(const SimGraph& graph, const Array<int>& dirtyNodeIds);

	/// @brief 出発 LaneNode から目標エッジへの経路を探索する
	PathResult dijkstra(int startLaneNodeId, int goalEdgeId, int goalLane = -1) const;

	/// @brief エッジの車線の入口 LaneNode ID を返す（存在しなければ -1）
	int entryNodeId(int edgeId, int laneIdx) const;

	/// @brief エッジの車線の出口 LaneNode ID を返す（存在しなければ -1）
	int exitNodeId(int edgeId, int laneIdx) const;

	/// @brief 指定ノードの出力辺リストを返す
	const Array<GraphEdge>* outgoingEdges(int nodeId) const;

	/// @brief 指定ノードが通行可能か（Phase 1: グラフに存在すれば通行可能）
	bool isNodePassable(int nodeId) const;

	const LaneNode*   getLaneNode(int nodeId)   const;
	const BorderNode* getBorderNode(int nodeId) const;
	int laneNodeCount() const { return static_cast<int>(m_laneNodes.size()); }

	/// @brief 2つのノードが同じ連結成分に属するかを O(1) で判定する
	bool sameComponent(int nodeA, int nodeB) const;

private:
	HashTable<int, LaneNode>   m_laneNodes;
	HashTable<int, Array<int>> m_edgeEntries;
	HashTable<int, BorderNode> m_borderNodes;
	HashTable<int64, int>      m_entryNodeIds;  ///< laneKey(edgeId, laneIdx) → nodeId
	HashTable<int64, int>      m_exitNodeIds;   ///< laneKey(edgeId, laneIdx) → nodeId
	int m_nextNodeId = 0;

	// 連結成分（Union-Find）
	mutable HashTable<int, int> m_ufParent;
	mutable HashTable<int, int> m_ufRank;
	int ufFind(int x) const;
	void ufUnion(int a, int b);
	void buildUnionFind();

	int  allocId() { return m_nextNodeId++; }

	/// @brief ハッシュキー: (edgeId, laneIdx) → int64
	static int64 laneKey(int edgeId, int laneIdx)
	{
		return ((int64)edgeId << 16) | (int64)(uint16)laneIdx;
	}
};
