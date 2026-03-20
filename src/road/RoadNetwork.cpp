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

	// 両端ノードのカットオフを再計算する
	updateNodeCutoffs(nodeA);
	updateNodeCutoffs(nodeB);

	// 接続ノードで滑らかに繋がるよう制御点を補正する
	smoothJunction(e.id, nodeA);
	smoothJunction(e.id, nodeB);

	return e.id;
}

void RoadNetwork::removeEdge(int edgeId)
{
	const int idx = edgeIndex(edgeId);
	if (idx < 0) return;

	RoadEdge& e = m_edges[idx];

	const int nA = e.nodeA;
	const int nB = e.nodeB;

	// 両ノードの edgeIds から削除する
	if (RoadNode* na = getNode(nA)) na->edgeIds.remove(edgeId);
	if (RoadNode* nb = getNode(nB)) nb->edgeIds.remove(edgeId);

	e.id = -1;

	// edgeIds 更新後にカットオフを再計算する
	updateNodeCutoffs(nA);
	updateNodeCutoffs(nB);
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

void RoadNetwork::updateNodeCutoffs(int nodeId)
{
	const RoadNode* node = getNode(nodeId);
	if (!node) return;

	// 接続中の有効エッジの最大幅を求める
	float maxWidth  = 0.0f;
	int   validCount = 0;
	for (int eid : node->edgeIds)
	{
		const RoadEdge* e = getEdge(eid);
		if (!e) continue;
		++validCount;
		maxWidth = Max(maxWidth, e->totalWidth());
	}

	// 端点（接続 1 本以下）はカットなし
	const float cutoff = (validCount >= 2) ? maxWidth * 1.5f : 0.0f;

	// このノード端のカットオフ値を全接続エッジに書き込む
	for (int eid : node->edgeIds)
	{
		RoadEdge* e = getEdge(eid);
		if (!e) continue;
		if (e->nodeA == nodeId) e->cutoffA = cutoff;
		else                    e->cutoffB = cutoff;
	}
}

void RoadNetwork::smoothJunction(int newEdgeId, int midNodeId)
{
	const RoadNode* midNode = getNode(midNodeId);
	if (!midNode) return;

	// 有効な接続エッジを列挙し、接続数が 2 でなければスキップ
	Array<int> validEdges;
	for (int eid : midNode->edgeIds)
	{
		if (getEdge(eid)) validEdges << eid;
	}
	if (validEdges.size() != 2) return;

	// PrevRoad を特定する（新エッジでない方）
	int prevEdgeId = -1;
	for (int eid : validEdges)
	{
		if (eid != newEdgeId) { prevEdgeId = eid; break; }
	}
	if (prevEdgeId == -1) return;

	RoadEdge* newEdge  = getEdge(newEdgeId);
	RoadEdge* prevEdge = getEdge(prevEdgeId);
	if (!newEdge || !prevEdge) return;

	const Vec3 midPos = midNode->position;

	// 各道路の反対側ノード
	const int newOtherNodeId  = (newEdge->nodeA  == midNodeId) ? newEdge->nodeB  : newEdge->nodeA;
	const int prevOtherNodeId = (prevEdge->nodeA == midNodeId) ? prevEdge->nodeB : prevEdge->nodeA;
	const RoadNode* newOtherNode  = getNode(newOtherNodeId);
	const RoadNode* prevOtherNode = getNode(prevOtherNodeId);
	if (!newOtherNode || !prevOtherNode) return;

	// MidNode から各端点への方向ベクトル（XZ 平面で判定）
	const Vec3 dirNew  = (newOtherNode->position  - midPos).normalized();
	const Vec3 dirPrev = (prevOtherNode->position - midPos).normalized();

	// なす角: ドット積で判定。cos(90°) = 0 なので dot <= 0 → angle >= 90°
	const double dot = dirNew.dot(dirPrev);
	if (dot > 0.0) return; // 90度未満 → スキップ

	// PrevRoad の MidNode 側制御点 CPP
	const Vec3 cpp = (prevEdge->nodeA == midNodeId) ? prevEdge->ctrlA : prevEdge->ctrlB;

	// CPP → MidNode 方向（この延長線上に CPN を置く）
	const Vec3  cppToMid = midPos - cpp;
	const double cppToMidLen = cppToMid.length();
	if (cppToMidLen < 1e-6) return;
	const Vec3 dir = cppToMid / cppToMidLen;

	// NewRoad 両端間の直線距離の 1/2
	const double halfDist = midPos.distanceFrom(newOtherNode->position) * 0.5;

	// 新しい CPN を書き込む
	const Vec3 newCpn = midPos + dir * halfDist;
	if (newEdge->nodeA == midNodeId)
		newEdge->ctrlA = newCpn;
	else
		newEdge->ctrlB = newCpn;
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
