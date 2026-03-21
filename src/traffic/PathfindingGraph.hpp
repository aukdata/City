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
	GraphEdgeType type      = GraphEdgeType::Forward;
	int           toNodeId  = -1;
	float         cost      = 0.0f;
	bool          dirty     = false;
};

// ===== ノード =====

/// @brief 車線ノード（あるエッジの、ある車線の、ある端点）
struct LaneNode
{
	int   id        = -1;
	int   edgeId    = -1;
	int   laneIndex = 0;
	float arcPos    = 0.0f;   ///< 0=エッジ始点, length=エッジ終点

	Array<GraphEdge> outgoing;
};

/// @brief 境界ノード（エッジがチャンク境界をまたぐ地点）
struct BorderNode
{
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

/// @brief 交差点ノードにおける車線間接続
struct LaneConnection
{
	int      nodeId;
	int      fromEdgeId;
	int      fromLaneIdx;
	int      toEdgeId;
	int      toLaneIdx;
	TurnType turn;
};

// ===== 経路探索結果 =====

/// @brief Dijkstra の結果
struct PathResult
{
	Array<int> nodeIds;              ///< LaneNode / BorderNode の ID 列（出発→ゴール順）
	float      totalCost = 1e30f;
	bool       found     = false;
};

// ===== PathfindingGraph =====

/// @brief 経路探索グラフ（Phase 1: 単一フラットグラフ方式）
/// @details ネットワーク全体の LaneNode + BorderNode を一つのグラフで管理する
class PathfindingGraph
{
public:
	/// @brief SimGraph からグラフを再構築する
	/// @param lights 信号機マップ（nodeId → TrafficLight）。Transition コストに待ち時間を加算する
	void rebuild(const SimGraph& graph, GameTime now,
	             const HashTable<int, TrafficLight>& lights);

	/// @brief 出発 LaneNode から目標エッジへの経路を探索する
	PathResult dijkstra(int startLaneNodeId, int goalEdgeId) const;

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

private:
	HashTable<int, LaneNode>   m_laneNodes;
	HashTable<int, BorderNode> m_borderNodes;
	HashTable<int64, int>      m_entryNodeIds;  ///< laneKey(edgeId, laneIdx) → nodeId
	HashTable<int64, int>      m_exitNodeIds;   ///< laneKey(edgeId, laneIdx) → nodeId
	int m_nextNodeId = 0;

	int  allocId() { return m_nextNodeId++; }

	/// @brief ハッシュキー: (edgeId, laneIdx) → int64
	static int64 laneKey(int edgeId, int laneIdx)
	{
		return ((int64)edgeId << 16) | (int64)(uint16)laneIdx;
	}

	/// @brief 進入・退出方向からターン種別を判定する（SimGraph の接線角を使用）
	TurnType calcTurnType(
		const SimGraph& graph,
		int fromEdgeId, LaneDir fromDir,
		int toEdgeId,   LaneDir toDir,
		int nodeId) const;

	/// @brief ターン種別に対応する交差点コストを返す（信号なし）
	float costTransition(TurnType turn) const;
};
