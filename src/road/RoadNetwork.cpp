#include "RoadNetwork.hpp"

int RoadNetwork::addNode(Vec3 pos, NodeType type)
{
	RoadNode n;
	n.id       = m_nextNodeId++;
	n.position = pos;
	n.type     = type;
	m_nodes << n;
	return n.id;
}

int RoadNetwork::addEdge(int nodeA, int nodeB,
	Vec3 ctrlA, Vec3 ctrlB,
	RoadType rt, int numLanes)
{
	RoadEdge e;
	e.id       = m_nextEdgeId++;
	e.nodeA    = nodeA;
	e.nodeB    = nodeB;
	e.ctrlA    = ctrlA;
	e.ctrlB    = ctrlB;
	e.roadType = rt;

	// 弧長を CubicBezier から計算する
	const RoadNode* nA = getNode(nodeA);
	const RoadNode* nB = getNode(nodeB);
	if (nA && nB)
	{
		CubicBezier bezier{ nA->position, ctrlA, ctrlB, nB->position };
		e.length = bezier.totalLength;
	}

	e.lanes        = buildDefaultLanes(numLanes, rt);
	e.laneVehicles = Array<Array<int>>(e.lanes.size());

	// 両ノードの edgeIds に登録する
	if (RoadNode* na = getNode(nodeA)) na->edgeIds << e.id;
	if (RoadNode* nb = getNode(nodeB)) nb->edgeIds << e.id;

	m_edges << e;
	return e.id;
}

void RoadNetwork::removeEdge(int edgeId)
{
	const int idx = edgeIndex(edgeId);
	if (idx < 0) return;

	RoadEdge& e = m_edges[idx];

	// 両ノードの edgeIds から削除する
	if (RoadNode* na = getNode(e.nodeA))
		na->edgeIds.remove(edgeId);
	if (RoadNode* nb = getNode(e.nodeB))
		nb->edgeIds.remove(edgeId);

	e.id = -1;
}

void RoadNetwork::removeNode(int nodeId)
{
	const int idx = nodeIndex(nodeId);
	if (idx < 0) return;
	m_nodes[idx].id = -1;
}

RoadEdge* RoadNetwork::getEdge(int id)
{
	const int idx = edgeIndex(id);
	return (idx >= 0) ? &m_edges[idx] : nullptr;
}

const RoadEdge* RoadNetwork::getEdge(int id) const
{
	const int idx = edgeIndex(id);
	return (idx >= 0) ? &m_edges[idx] : nullptr;
}

RoadNode* RoadNetwork::getNode(int id)
{
	const int idx = nodeIndex(id);
	return (idx >= 0) ? &m_nodes[idx] : nullptr;
}

const RoadNode* RoadNetwork::getNode(int id) const
{
	const int idx = nodeIndex(id);
	return (idx >= 0) ? &m_nodes[idx] : nullptr;
}

Optional<int> RoadNetwork::findNodeNear(Vec3 pos, float radius) const
{
	Optional<int> best = none;
	float bestDist = radius;

	for (const auto& n : m_nodes)
	{
		if (n.id == -1) continue;
		const float dist = static_cast<float>((n.position - pos).length());
		if (dist <= bestDist)
		{
			bestDist = dist;
			best     = n.id;
		}
	}
	return best;
}

int RoadNetwork::addEdgeWithIntersection(int nodeA, int nodeB,
	Vec3 ctrlA, Vec3 ctrlB,
	RoadType rt, int numLanes)
{
	// Phase 1: 単純に addEdge を呼ぶ（交差分割は Phase 2 以降）
	return addEdge(nodeA, nodeB, ctrlA, ctrlB, rt, numLanes);
}

Optional<CubicBezier> RoadNetwork::getBezier(int edgeId) const
{
	const RoadEdge* e = getEdge(edgeId);
	if (!e) return none;

	const RoadNode* nA = getNode(e->nodeA);
	const RoadNode* nB = getNode(e->nodeB);
	if (!nA || !nB) return none;

	return CubicBezier{ nA->position, e->ctrlA, e->ctrlB, nB->position };
}

int RoadNetwork::edgeIndex(int id) const
{
	for (int i = 0; i < static_cast<int>(m_edges.size()); ++i)
	{
		if (m_edges[i].id == id) return i;
	}
	return -1;
}

int RoadNetwork::nodeIndex(int id) const
{
	for (int i = 0; i < static_cast<int>(m_nodes.size()); ++i)
	{
		if (m_nodes[i].id == id) return i;
	}
	return -1;
}

void RoadNetwork::addTempOp(int edgeId, TempOp op)
{
	RoadEdge* e = getEdge(edgeId);
	if (e)
		e->tempOps << std::move(op);
}

bool RoadNetwork::clearExpiredTempOps(GameTime now)
{
	bool changed = false;
	for (auto& edge : m_edges)
	{
		if (edge.id < 0) continue;
		const int before = static_cast<int>(edge.tempOps.size());
		edge.tempOps.remove_if([now](const TempOp& op) { return op.end < now; });
		if (static_cast<int>(edge.tempOps.size()) != before)
			changed = true;
	}
	return changed;
}

Array<Lane> RoadNetwork::buildDefaultLanes(int numLanes, RoadType rt)
{
	float laneWidth = 3.5f;
	if (rt == RoadType::Expressway || rt == RoadType::Highway)
		laneWidth = 3.75f;

	// Forward レーン数: 過半数（偶数なら半分、奇数なら切り上げ）
	const int forwardCount = (numLanes + 1) / 2;

	Array<Lane> lanes;
	lanes.reserve(numLanes);

	for (int i = 0; i < numLanes; ++i)
	{
		Lane lane;
		lane.index = i;
		lane.build = BuildState::Built;
		lane.type  = LaneType::Normal;
		lane.width = laneWidth;
		lane.op    = OpState::Open;
		lane.dir   = (i < forwardCount) ? LaneDir::Forward : LaneDir::Backward;
		lanes << lane;
	}
	return lanes;
}
