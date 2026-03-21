#include "PathfindingGraph.hpp"
#include <queue>

// ===== rebuild =====

void PathfindingGraph::rebuild(const SimGraph& graph, [[maybe_unused]] GameTime now,
                               const HashTable<int, TrafficLight>& lights)
{
	m_laneNodes.clear();
	m_borderNodes.clear();
	m_entryNodeIds.clear();
	m_exitNodeIds.clear();
	m_nextNodeId = 0;

	// --- Step 1: 各エッジの走行可能な車線ごとに入口・出口 LaneNode を生成 ---
	for (const auto& [eid, edge] : graph.edges)
	{
		for (int i = 0; i < static_cast<int>(edge.lanes.size()); ++i)
		{
			const Lane& L = edge.lanes[i];
			if (!isPassable(L)) continue;

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

			const float speedMS  = edge.speedLimit / 3.6f;
			const float effSpeed = speedMS * (1.0f - edge.congestion * 0.8f);
			const float fwdCost  = (effSpeed > 0.0f) ? (edge.length / effSpeed) : 1e6f;
			entryNode.outgoing << GraphEdge{ GraphEdgeType::Forward, exitNode.id, fwdCost };

			const int64 key = laneKey(edge.id, i);
			m_entryNodeIds[key] = entryNode.id;
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
			if (!isPassable(Li) || !isPassable(Lj)) continue;
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
	for (const auto& [nid, node] : graph.nodes)
	{
		for (const int inEdgeId : node.edgeIds)
		{
			const SimGraph::Edge* inEdge = graph.getEdge(inEdgeId);
			if (!inEdge) continue;

			for (int i = 0; i < static_cast<int>(inEdge->lanes.size()); ++i)
			{
				const Lane& Lin = inEdge->lanes[i];
				if (!isPassable(Lin)) continue;

				const bool exitsAtNode =
					(Lin.dir == LaneDir::Forward  && inEdge->nodeB == node.id) ||
					(Lin.dir == LaneDir::Backward && inEdge->nodeA == node.id);
				if (!exitsAtNode) continue;

				const auto exitIt = m_exitNodeIds.find(laneKey(inEdgeId, i));
				if (exitIt == m_exitNodeIds.end()) continue;
				const int exitId = exitIt->second;

				for (const int outEdgeId : node.edgeIds)
				{
					if (outEdgeId == inEdgeId) continue;

					const SimGraph::Edge* outEdge = graph.getEdge(outEdgeId);
					if (!outEdge) continue;

					for (int j = 0; j < static_cast<int>(outEdge->lanes.size()); ++j)
					{
						const Lane& Lout = outEdge->lanes[j];
						if (!isPassable(Lout)) continue;

						const bool entersAtNode =
							(Lout.dir == LaneDir::Forward  && outEdge->nodeA == node.id) ||
							(Lout.dir == LaneDir::Backward && outEdge->nodeB == node.id);
						if (!entersAtNode) continue;

						const auto entryIt = m_entryNodeIds.find(laneKey(outEdgeId, j));
						if (entryIt == m_entryNodeIds.end()) continue;
						const int entryId = entryIt->second;

						const TurnType turn = calcTurnType(graph, inEdgeId, Lin.dir, outEdgeId, Lout.dir, node.id);
						float cost          = costTransition(turn);

						const auto tlIt = lights.find(node.id);
						if (tlIt != lights.end())
							cost += tlIt->second.expectedWaitTime(inEdgeId);

						m_laneNodes[exitId].outgoing << GraphEdge{ GraphEdgeType::Transition, entryId, cost };
					}
				}
			}
		}
	}
}

// ===== dijkstra =====

PathResult PathfindingGraph::dijkstra(int startLaneNodeId, int goalEdgeId) const
{
	PathResult result;

	if (m_laneNodes.find(startLaneNodeId) == m_laneNodes.end())
		return result;

	HashTable<int, float> dist;
	HashTable<int, int>   prev;

	using Entry = std::pair<float, int>;
	std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pq;

	dist[startLaneNodeId] = 0.0f;
	pq.push({ 0.0f, startLaneNodeId });

	int goalNode = -1;

	while (!pq.empty())
	{
		const auto [d, u] = pq.top();
		pq.pop();

		const auto distIt = dist.find(u);
		if (distIt == dist.end() || d > distIt->second) continue;

		const LaneNode* lNode = getLaneNode(u);
		if (lNode && lNode->edgeId == goalEdgeId)
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
				pq.push({ nc, ge.toNodeId });
			}
		}
	}

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

int PathfindingGraph::entryNodeId(int edgeId, int laneIdx) const
{
	const auto it = m_entryNodeIds.find(laneKey(edgeId, laneIdx));
	return (it != m_entryNodeIds.end()) ? it->second : -1;
}

int PathfindingGraph::exitNodeId(int edgeId, int laneIdx) const
{
	const auto it = m_exitNodeIds.find(laneKey(edgeId, laneIdx));
	return (it != m_exitNodeIds.end()) ? it->second : -1;
}

const Array<GraphEdge>* PathfindingGraph::outgoingEdges(int nodeId) const
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

bool PathfindingGraph::isNodePassable(int nodeId) const
{
	return m_laneNodes.contains(nodeId) || m_borderNodes.contains(nodeId);
}

const LaneNode* PathfindingGraph::getLaneNode(int nodeId) const
{
	const auto it = m_laneNodes.find(nodeId);
	return (it != m_laneNodes.end()) ? &it->second : nullptr;
}

const BorderNode* PathfindingGraph::getBorderNode(int nodeId) const
{
	const auto it = m_borderNodes.find(nodeId);
	return (it != m_borderNodes.end()) ? &it->second : nullptr;
}

// ===== ターン判定（SimGraph の接線角を使用） =====

TurnType PathfindingGraph::calcTurnType(
	const SimGraph& graph,
	int fromEdgeId, LaneDir fromDir,
	int toEdgeId,   LaneDir toDir,
	int /*nodeId*/) const
{
	const SimGraph::Edge* fromE = graph.getEdge(fromEdgeId);
	const SimGraph::Edge* toE   = graph.getEdge(toEdgeId);
	if (!fromE || !toE) return TurnType::Straight;

	// 進入方向: Forward なら nodeB 端の接線、Backward なら nodeA 端の接線を反転
	const float inAngle = (fromDir == LaneDir::Forward)
		? fromE->tangentAngleB
		: (fromE->tangentAngleA + static_cast<float>(Math::Pi));

	// 退出方向: Forward なら nodeA 端の接線、Backward なら nodeB 端の接線を反転
	const float outAngle = (toDir == LaneDir::Forward)
		? toE->tangentAngleA
		: (toE->tangentAngleB + static_cast<float>(Math::Pi));

	const float cosIn  = std::cos(inAngle),  sinIn  = std::sin(inAngle);
	const float cosOut = std::cos(outAngle), sinOut = std::sin(outAngle);

	const float dot   = cosIn * cosOut + sinIn * sinOut;
	const float cross = cosIn * sinOut - sinIn * cosOut;

	if (dot  >  0.7f) return TurnType::Straight;
	if (dot  < -0.7f) return TurnType::UTurn;
	if (cross > 0.0f) return TurnType::Left;
	return TurnType::Right;
}

float PathfindingGraph::costTransition(TurnType turn) const
{
	switch (turn)
	{
	case TurnType::Straight: return 2.0f;
	case TurnType::Left:     return 5.0f;
	case TurnType::Right:    return 8.0f;
	case TurnType::UTurn:    return 15.0f;
	}
	return 2.0f;
}
