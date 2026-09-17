#include "TrafficGraph.hpp"
#include "TrafficCommon.hpp"
#include <queue>

namespace
{
	float computeForwardCost(const SimGraph::Edge& edge)
	{
		const float speedMS  = edge.speedLimit / 3.6f;
		const float effSpeed = speedMS * (1.0f - edge.congestion * 0.8f);
		return (effSpeed > 0.0f) ? (edge.length / effSpeed) : 1e6f;
	}
}

// ===== rebuild =====

void TrafficGraph::rebuild(const SimGraph& graph, [[maybe_unused]] GameTime now,
                               const HashTable<int, TrafficLight>& lights)
{
	// 車線単位の探索グラフを毎回作り直し、前進・車線変更・交差点遷移を明示辺として張り直す。
	m_laneNodes.clear();
	m_borderNodes.clear();
	m_entryNodeIds.clear();
	m_edgeEntries.clear();
	m_exitNodeIds.clear();
	m_nextNodeId = 0;

	// --- Step 1: 各エッジの走行可能な車線ごとに入口・出口 LaneNode を生成 ---
	for (const auto& [eid, edge] : graph.edges)
	{
		for (int i = 0; i < static_cast<int>(edge.lanes.size()); ++i)
		{
			const Lane& L = edge.lanes[i];
			if (!(edge.isRoadbedBuilt() && (L.allows(TransportMode::Road) && (L.op == OpState::Open || L.op == OpState::Provisional)))) continue;

			const float entryArc = (L.dir == LaneDir::Forward) ? 0.0f : edge.length;
			const float exitArc  = (L.dir == LaneDir::Forward) ? edge.length : 0.0f;

			LaneNode entryNode;
			entryNode.id        = allocId();
			entryNode.edgeId    = edge.id;
			entryNode.laneIndex = i;
			entryNode.arcPos    = entryArc;

			LaneNode exitNode;
			exitNode.id        = allocId();
			exitNode.edgeId    = edge.id;
			exitNode.laneIndex = i;
			exitNode.arcPos    = exitArc;

			const float fwdCost  = computeForwardCost(edge);
			entryNode.outgoing << GraphEdge{ GraphEdgeType::Forward, exitNode.id, fwdCost };

			const int64 key = laneKey(edge.id, i);
			m_entryNodeIds[key] = entryNode.id;
			m_edgeEntries[edge.id] << entryNode.id;
			m_exitNodeIds[key]  = exitNode.id;
			m_laneNodes[entryNode.id] = std::move(entryNode);
			m_laneNodes[exitNode.id]  = std::move(exitNode);
		}
	}

	// --- Step 2: 同一エッジの隣接車線間に LaneChange 辺を追加 ---
	constexpr float kLaneChangeCost = 5.0f;

	for (const auto& [eid, edge] : graph.edges)
	{
		for (int i = 0; i < static_cast<int>(edge.lanes.size()) - 1; ++i)
		{
			const int j = i + 1;
			const Lane& Li = edge.lanes[i];
			const Lane& Lj = edge.lanes[j];
			if (!(edge.isRoadbedBuilt() && (Li.allows(TransportMode::Road) && (Li.op == OpState::Open || Li.op == OpState::Provisional)))
				|| !(edge.isRoadbedBuilt() && (Lj.allows(TransportMode::Road) && (Lj.op == OpState::Open || Lj.op == OpState::Provisional)))) continue;
			if (Li.dir != Lj.dir) continue;

			const auto eiIt = m_entryNodeIds.find(laneKey(edge.id, i));
			const auto ejIt = m_entryNodeIds.find(laneKey(edge.id, j));
			const auto xiIt = m_exitNodeIds.find(laneKey(edge.id, i));
			const auto xjIt = m_exitNodeIds.find(laneKey(edge.id, j));

			if (eiIt != m_entryNodeIds.end() && ejIt != m_entryNodeIds.end())
			{
				m_laneNodes[eiIt->second].outgoing << GraphEdge{ GraphEdgeType::LaneChange, ejIt->second, kLaneChangeCost };
				m_laneNodes[ejIt->second].outgoing << GraphEdge{ GraphEdgeType::LaneChange, eiIt->second, kLaneChangeCost };
			}
			if (xiIt != m_exitNodeIds.end() && xjIt != m_exitNodeIds.end())
			{
				m_laneNodes[xiIt->second].outgoing << GraphEdge{ GraphEdgeType::LaneChange, xjIt->second, kLaneChangeCost };
				m_laneNodes[xjIt->second].outgoing << GraphEdge{ GraphEdgeType::LaneChange, xiIt->second, kLaneChangeCost };
			}
		}
	}

	// --- Step 3: 各ノードで Transition 辺を追加 ---
	// RoadNode.laneConnections に実在する旋回パスにのみ辺を張る。
	// これにより「左折専用レーンから直進」のような物理不可能な経路が
	// 探索結果に含まれないことが保証される。
	// 詳細は plan/08_pathfinding_spec.md §3 参照。
	for (const auto& [nid, node] : graph.nodes)
	{
		for (const LaneConnection& conn : node.laneConnections)
		{
			const SimGraph::Edge* fromE = graph.getEdge(conn.fromEdgeId);
			const SimGraph::Edge* toE   = graph.getEdge(conn.toEdgeId);
			if (!fromE || !toE) continue;

			// from 側車線が走行可能か
			if (conn.fromLaneIndex < 0 || conn.fromLaneIndex >= static_cast<int>(fromE->lanes.size())) continue;
			const Lane& fromLane = fromE->lanes[conn.fromLaneIndex];
			if (!(fromE->isRoadbedBuilt() && (fromLane.allows(TransportMode::Road) && (fromLane.op == OpState::Open || fromLane.op == OpState::Provisional)))) continue;

			// to 側車線が走行可能か
			if (conn.toLaneIndex < 0 || conn.toLaneIndex >= static_cast<int>(toE->lanes.size())) continue;
			const Lane& toLane = toE->lanes[conn.toLaneIndex];
			if (!(toE->isRoadbedBuilt() && (toLane.allows(TransportMode::Road) && (toLane.op == OpState::Open || toLane.op == OpState::Provisional)))) continue;

			// 進入エッジの exit ノード ID と退出エッジの entry ノード ID を取得
			const auto exitIt = m_exitNodeIds.find(laneKey(conn.fromEdgeId, conn.fromLaneIndex));
			if (exitIt == m_exitNodeIds.end()) continue;
			const auto entryIt = m_entryNodeIds.find(laneKey(conn.toEdgeId, conn.toLaneIndex));
			if (entryIt == m_entryNodeIds.end()) continue;

			const TurnType turn = TrafficCommon::classifyTurn(graph, conn);
			float cost          = TrafficCommon::costTransition(turn);

			const auto tlIt = lights.find(node.id);
			if (tlIt != lights.end())
				cost += tlIt->second.expectedWaitTime(conn.id);

			m_laneNodes[exitIt->second].outgoing << GraphEdge{ GraphEdgeType::Transition, entryIt->second, cost };
		}
	}

	buildUnionFind();
}

void TrafficGraph::updateMovedIntersectionNode(const SimGraph& graph, const Array<int>& dirtyNodeIds)
{
	HashSet<int> dirtyEdgeIds;
	for (const int nodeId : dirtyNodeIds)
	{
		const SimGraph::Node* node = graph.getNode(nodeId);
		if (!node) continue;
		for (const int edgeId : node->edgeIds)
			dirtyEdgeIds.insert(edgeId);
	}

	for (const int edgeId : dirtyEdgeIds)
	{
		const SimGraph::Edge* edge = graph.getEdge(edgeId);
		if (!edge) continue;

		for (int laneIndex = 0; laneIndex < static_cast<int>(edge->lanes.size()); ++laneIndex)
		{
			const auto entryIt = m_entryNodeIds.find(laneKey(edgeId, laneIndex));
			const auto exitIt = m_exitNodeIds.find(laneKey(edgeId, laneIndex));
			if (entryIt == m_entryNodeIds.end() || exitIt == m_exitNodeIds.end()) continue;

			LaneNode* entryNode = nullptr;
			LaneNode* exitNode = nullptr;
			if (auto it = m_laneNodes.find(entryIt->second); it != m_laneNodes.end()) entryNode = &it->second;
			if (auto it = m_laneNodes.find(exitIt->second); it != m_laneNodes.end()) exitNode = &it->second;
			if (!entryNode || !exitNode) continue;

			const Lane& lane = edge->lanes[laneIndex];
			entryNode->arcPos = (lane.dir == LaneDir::Forward) ? 0.0f : edge->length;
			exitNode->arcPos = (lane.dir == LaneDir::Forward) ? edge->length : 0.0f;

			for (auto& outgoing : entryNode->outgoing)
			{
				if (outgoing.type != GraphEdgeType::Forward) continue;
				outgoing.toNodeId = exitNode->id;
				outgoing.cost = computeForwardCost(*edge);
				break;
			}
		}
	}
}

// ===== Union-Find =====

int TrafficGraph::ufFind(int x) const
{
	while (true)
	{
		const auto it = m_ufParent.find(x);
		if (it == m_ufParent.end() || it->second == x) return x;
		// 経路圧縮
		const int root = ufFind(it->second);
		it->second = root;
		return root;
	}
}

void TrafficGraph::ufUnion(int a, int b)
{
	const int ra = ufFind(a);
	const int rb = ufFind(b);
	if (ra == rb) return;

	const int rankA = m_ufRank[ra];
	const int rankB = m_ufRank[rb];
	if (rankA < rankB)
		m_ufParent[ra] = rb;
	else if (rankA > rankB)
		m_ufParent[rb] = ra;
	else
	{
		m_ufParent[rb] = ra;
		m_ufRank[ra]++;
	}
}

void TrafficGraph::buildUnionFind()
{
	// 探索前に連結成分を作っておき、到達不能なゴールは Dijkstra 前に即座に弾けるようにする。
	m_ufParent.clear();
	m_ufRank.clear();

	// 全ノードを初期化
	for (const auto& [id, _] : m_laneNodes)
	{
		m_ufParent[id] = id;
		m_ufRank[id]   = 0;
	}
	for (const auto& [id, _] : m_borderNodes)
	{
		m_ufParent[id] = id;
		m_ufRank[id]   = 0;
	}

	// 全辺を走査して union
	for (const auto& [id, node] : m_laneNodes)
	{
		for (const auto& edge : node.outgoing)
			ufUnion(id, edge.toNodeId);
	}
	for (const auto& [id, node] : m_borderNodes)
	{
		for (const auto& edge : node.outgoing)
			ufUnion(id, edge.toNodeId);
	}
}

bool TrafficGraph::sameComponent(int nodeA, int nodeB) const
{
	if (m_ufParent.find(nodeA) == m_ufParent.end()) return false;
	if (m_ufParent.find(nodeB) == m_ufParent.end()) return false;
	return ufFind(nodeA) == ufFind(nodeB);
}

// ===== dijkstra =====

PathResult TrafficGraph::dijkstra(int startLaneNodeId, int goalEdgeId, int goalLane) const
{
	// 車線ノード上で Dijkstra を回し、ゴールエッジへ入った最初のノードから経路を復元する。
	PathResult result;

	if (m_laneNodes.find(startLaneNodeId) == m_laneNodes.end())
		return result;

	// 連結成分チェック: ゴールエッジの任意の entry ノードと start が同成分か
	{
		bool reachable = false;
		if (const auto entries = m_edgeEntries.find(goalEdgeId); entries != m_edgeEntries.end())
		{
			for (const int node : entries->second)
			{
				const auto* laneNode = getLaneNode(node);
				if (goalLane >= 0 && laneNode->laneIndex != goalLane) { continue; }
				if (sameComponent(startLaneNodeId, node)) { reachable = true; break; }
			}
		}
		if (!reachable)
		{
			result.graphSize = static_cast<int>(m_laneNodes.size() + m_borderNodes.size());
			return result;
		}
	}

	int visited = 0;

	HashTable<int, float> dist;
	HashTable<int, int>   prev;

	using Entry = std::pair<float, int>;
	std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> frontier;

	dist[startLaneNodeId] = 0.0f;
	frontier.push({ 0.0f, startLaneNodeId });

	int goalNode = -1;

	while (!frontier.empty())
	{
		const auto [d, u] = frontier.top();
		frontier.pop();

		const auto distIt = dist.find(u);
		if (distIt == dist.end() || d > distIt->second) continue;

		++visited;

		const LaneNode* lNode = getLaneNode(u);
		if (lNode && lNode->edgeId == goalEdgeId && (goalLane < 0 || lNode->laneIndex == goalLane))
		{
			goalNode = u;
			break;
		}

		const Array<GraphEdge>* edges = outgoingEdges(u);
		if (!edges) continue;

		for (const auto& ge : *edges)
		{
			if (!isNodePassable(ge.toNodeId)) continue;

			const float nc = d + ge.cost;
			const auto nIt = dist.find(ge.toNodeId);
			if (nIt == dist.end() || nc < nIt->second)
			{
				dist[ge.toNodeId] = nc;
				prev[ge.toNodeId] = u;
				frontier.push({ nc, ge.toNodeId });
			}
		}
	}

	result.nodesVisited = visited;
	result.graphSize    = static_cast<int>(m_laneNodes.size() + m_borderNodes.size());

	if (goalNode == -1) return result;

	result.found     = true;
	result.totalCost = dist.find(goalNode)->second;

	int cur = goalNode;
	while (true)
	{
		result.nodeIds << cur;
		if (cur == startLaneNodeId) break;
		const auto it = prev.find(cur);
		if (it == prev.end()) break;
		cur = it->second;
	}
	std::reverse(result.nodeIds.begin(), result.nodeIds.end());

	return result;
}

// ===== クエリ =====

int TrafficGraph::entryNodeId(int edgeId, int laneIdx) const
{
	const auto it = m_entryNodeIds.find(laneKey(edgeId, laneIdx));
	return (it != m_entryNodeIds.end()) ? it->second : -1;
}

int TrafficGraph::exitNodeId(int edgeId, int laneIdx) const
{
	const auto it = m_exitNodeIds.find(laneKey(edgeId, laneIdx));
	return (it != m_exitNodeIds.end()) ? it->second : -1;
}

const Array<GraphEdge>* TrafficGraph::outgoingEdges(int nodeId) const
{
	{
		const auto it = m_laneNodes.find(nodeId);
		if (it != m_laneNodes.end()) return &it->second.outgoing;
	}
	{
		const auto it = m_borderNodes.find(nodeId);
		if (it != m_borderNodes.end()) return &it->second.outgoing;
	}
	return nullptr;
}

bool TrafficGraph::isNodePassable(int nodeId) const
{
	return m_laneNodes.contains(nodeId) || m_borderNodes.contains(nodeId);
}

const LaneNode* TrafficGraph::getLaneNode(int nodeId) const
{
	const auto it = m_laneNodes.find(nodeId);
	return (it != m_laneNodes.end()) ? &it->second : nullptr;
}

const BorderNode* TrafficGraph::getBorderNode(int nodeId) const
{
	const auto it = m_borderNodes.find(nodeId);
	return (it != m_borderNodes.end()) ? &it->second : nullptr;
}

// ターン判定とコスト関数は TrafficCommon::classifyTurn / TrafficCommon::costTransition に移動した
