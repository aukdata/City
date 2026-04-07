#include "RoadNetwork.hpp"
#include "../world/World.hpp"

namespace
{
	/// @brief XZ 平面での 2D 線分交差判定
	/// @param a1,a2  線分1の端点 (x = world-X, y = world-Z)
	/// @param b1,b2  線分2の端点
	/// @param s      出力: 線分1上の交差パラメータ [0,1]
	/// @param t      出力: 線分2上の交差パラメータ [0,1]
	/// @return 内点で交差する場合 true（端点付近 < EPS は false）
	bool segIntersect2D(Vec2 a1, Vec2 a2, Vec2 b1, Vec2 b2, float& s, float& t)
	{
		const double dxr = a2.x - a1.x, dyr = a2.y - a1.y;
		const double dxs = b2.x - b1.x, dys = b2.y - b1.y;
		const double denom = dxr * dys - dyr * dxs;
		if (std::abs(denom) < 1e-8) return false; // 平行
		const double qx = b1.x - a1.x, qy = b1.y - a1.y;
		s = static_cast<float>((qx * dys - qy * dxs) / denom);
		t = static_cast<float>((qx * dyr - qy * dxr) / denom);
		constexpr float EPS = 1e-4f;
		return s > EPS && s < 1.0f - EPS && t > EPS && t < 1.0f - EPS;
	}

} // namespace

int RoadNetwork::addNode(Vec3 pos, NodeType type)
{
	RoadNode n;
	n.id       = m_nextNodeId++;
	n.position = pos;
	n.type     = type;

	if (!m_freeNodeSlots.isEmpty())
	{
		const int idx = m_freeNodeSlots.back();
		m_freeNodeSlots.pop_back();
		m_nodes[idx] = n;
		m_nodeIdToIdx[n.id] = idx;
	}
	else
	{
		m_nodeIdToIdx[n.id] = static_cast<int>(m_nodes.size());
		m_nodes << n;
	}
	return n.id;
}

Optional<int> RoadNetwork::addEdge(int nodeA, int nodeB,
	Vec3 ctrlA, Vec3 ctrlB,
	RoadType rt, int numLanes)
{
	// 自己ループ禁止
	if (nodeA == nodeB) return none;

	// 同一ノードペア間の重複エッジ禁止
	const RoadNode* chkA = getNode(nodeA);
	const RoadNode* chkB = getNode(nodeB);
	if (chkA)
	{
		for (const auto& att : chkA->attachments)
		{
			const RoadEdge* ex = getEdge(att.edgeId);
			if (!ex) continue;
			if ((ex->nodeA == nodeA && ex->nodeB == nodeB) ||
			    (ex->nodeA == nodeB && ex->nodeB == nodeA))
				return none;
		}
	}

	// 1ノードあたりのエッジ上限
	constexpr int kMaxEdgesPerNode = 6;
	if ((chkA && static_cast<int>(chkA->attachments.size()) >= kMaxEdgesPerNode) ||
	    (chkB && static_cast<int>(chkB->attachments.size()) >= kMaxEdgesPerNode))
		return none;

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
	buildDefaultParts(e);

	// 両ノードの edgeIds に登録する
	if (RoadNode* na = getNode(nodeA)) na->addEdge(e.id);
	if (RoadNode* nb = getNode(nodeB)) nb->addEdge(e.id);

	if (!m_freeEdgeSlots.isEmpty())
	{
		const int idx = m_freeEdgeSlots.back();
		m_freeEdgeSlots.pop_back();
		m_edges[idx] = e;
		m_edgeIdToIdx[e.id] = idx;
	}
	else
	{
		m_edgeIdToIdx[e.id] = static_cast<int>(m_edges.size());
		m_edges << e;
	}

	rebuildNodeConnectivity(nodeA, nodeB);
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
	if (RoadNode* na = getNode(nA)) na->removeEdge(edgeId);
	if (RoadNode* nb = getNode(nB)) nb->removeEdge(edgeId);

	e.id = -1;
	m_edgeIdToIdx.erase(edgeId);
	m_freeEdgeSlots << idx;
	removeObjectsByEdge(edgeId);

	rebuildNodeConnectivity(nA, nB);
}

void RoadNetwork::removeNode(int nodeId)
{
	const int idx = nodeIndex(nodeId);
	if (idx < 0) return;
	m_nodes[idx].id = -1;
	m_nodeIdToIdx.erase(nodeId);
	m_freeNodeSlots << idx;
}

void RoadNetwork::addNodeRaw(const RoadNode& node)
{
	if (m_nodeIdToIdx.contains(node.id)) return;   // 既に存在する場合はスキップ

	if (!m_freeNodeSlots.isEmpty())
	{
		const int idx = m_freeNodeSlots.back();
		m_freeNodeSlots.pop_back();
		m_nodes[idx] = node;
		m_nodeIdToIdx[node.id] = idx;
	}
	else
	{
		m_nodeIdToIdx[node.id] = static_cast<int>(m_nodes.size());
		m_nodes << node;
	}
	if (node.id >= m_nextNodeId) m_nextNodeId = node.id + 1;
}

void RoadNetwork::addEdgeRaw(const RoadEdge& edge)
{
	if (m_edgeIdToIdx.contains(edge.id)) return;   // 既に存在する場合はスキップ

	if (!m_freeEdgeSlots.isEmpty())
	{
		const int idx = m_freeEdgeSlots.back();
		m_freeEdgeSlots.pop_back();
		m_edges[idx] = edge;
		m_edgeIdToIdx[edge.id] = idx;
	}
	else
	{
		m_edgeIdToIdx[edge.id] = static_cast<int>(m_edges.size());
		m_edges << edge;
	}
	if (RoadNode* na = getNode(edge.nodeA)) na->addEdge(edge.id);
	if (RoadNode* nb = getNode(edge.nodeB)) nb->addEdge(edge.id);
	if (edge.id >= m_nextEdgeId) m_nextEdgeId = edge.id + 1;

	// parts が空なら RoadType からデフォルト部品を生成
	{
		RoadEdge* e = getEdge(edge.id);
		if (e && e->parts.isEmpty())
			buildDefaultParts(*e);
	}

	updateNodeCutoffs(edge.nodeA);
	updateNodeCutoffs(edge.nodeB);
	rebuildLaneConnections(edge.nodeA);
	rebuildLaneConnections(edge.nodeB);
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
		const float dx = static_cast<float>(n.position.x - pos.x);
		const float dz = static_cast<float>(n.position.z - pos.z);
		const float dist = std::sqrt(dx * dx + dz * dz);
		if (dist <= bestDist)
		{
			bestDist = dist;
			best     = n.id;
		}
	}
	return best;
}

Optional<std::pair<int, float>> RoadNetwork::findEdgeNearDetailed(Vec3 pos, float maxDist) const
{
	Optional<std::pair<int, float>> best;
	float bestDist = maxDist;

	for (const auto& edge : m_edges)
	{
		if (edge.id < 0) continue;
		const auto bez = getBezier(edge.id);
		if (!bez) continue;

		// 20 分割でサンプリング
		constexpr int N = 20;
		int bestIdx = -1;
		for (int i = 0; i <= N; ++i)
		{
			const float s = bez->totalLength * (static_cast<float>(i) / N);
			const Vec3 p = bez->positionAt(s);
			const float dx = static_cast<float>(p.x - pos.x);
			const float dz = static_cast<float>(p.z - pos.z);
			const float dist = std::sqrt(dx * dx + dz * dz);
			if (dist < bestDist)
			{
				bestDist = dist;
				bestIdx = i;
				best = std::pair<int, float>{ edge.id, s };
			}
		}

		// 隣接区間内で二分探索精緻化
		if (bestIdx >= 0 && best && best->first == edge.id)
		{
			const float sLo = bez->totalLength * (Max(bestIdx - 1, 0) / static_cast<float>(N));
			const float sHi = bez->totalLength * (Min(bestIdx + 1, N) / static_cast<float>(N));
			float lo = sLo, hi = sHi;
			for (int iter = 0; iter < 8; ++iter)
			{
				const float m1 = lo + (hi - lo) / 3.0f;
				const float m2 = hi - (hi - lo) / 3.0f;
				const Vec3 p1 = bez->positionAt(m1);
				const Vec3 p2 = bez->positionAt(m2);
				const float d1 = static_cast<float>((p1.x - pos.x) * (p1.x - pos.x) + (p1.z - pos.z) * (p1.z - pos.z));
				const float d2 = static_cast<float>((p2.x - pos.x) * (p2.x - pos.x) + (p2.z - pos.z) * (p2.z - pos.z));
				if (d1 < d2) hi = m2; else lo = m1;
			}
			best->second = (lo + hi) * 0.5f;
		}
	}
	return best;
}

void RoadNetwork::applyEdgeTemplate(int edgeId, const RoadEdge& tmpl)
{
	RoadEdge* e = getEdge(edgeId);
	if (!e) return;
	e->speedLimit   = tmpl.speedLimit;
	e->parts        = tmpl.parts;
	e->lanes        = tmpl.lanes;
	e->laneVehicles = Array<Array<int>>(e->lanes.size());
}

int RoadNetwork::splitEdgeAt(int edgeId, float arcLength)
{
	RoadEdge* edge = getEdge(edgeId);
	if (!edge) return -1;

	const RoadNode* nA = getNode(edge->nodeA);
	const RoadNode* nB = getNode(edge->nodeB);
	if (!nA || !nB) return -1;

	CubicBezier bez{ nA->position, edge->ctrlA, edge->ctrlB, nB->position };
	const float t = bez.tFromArcLength(arcLength);
	if (t <= 0.01f || t >= 0.99f) return -1;

	const auto [bezA, bezB] = bez.split(t);
	const Vec3 splitPos = bez.evaluate(t);

	// 元エッジの属性を保存（removeEdge で無効化される前にコピー）
	const int origNodeA  = edge->nodeA;
	const int origNodeB  = edge->nodeB;
	const RoadType rt    = edge->roadType;
	const int numLanes   = static_cast<int>(edge->lanes.size());
	const RoadEdge tmpl  = *edge;  // テンプレートとして属性を丸ごとコピー

	removeEdge(edgeId);

	const int midNodeId = addNode(splitPos, NodeType::Joint);

	if (auto eidA = addEdge(origNodeA, midNodeId, bezA.p1, bezA.p2, rt, numLanes))
		applyEdgeTemplate(*eidA, tmpl);

	if (auto eidB = addEdge(midNodeId, origNodeB, bezB.p1, bezB.p2, rt, numLanes))
		applyEdgeTemplate(*eidB, tmpl);

	return midNodeId;
}

Optional<int> RoadNetwork::addEdgeWithIntersection(int nodeA, int nodeB,
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
	const auto it = m_edgeIdToIdx.find(id);
	return (it != m_edgeIdToIdx.end()) ? it->second : -1;
}

int RoadNetwork::nodeIndex(int id) const
{
	const auto it = m_nodeIdToIdx.find(id);
	return (it != m_nodeIdToIdx.end()) ? it->second : -1;
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
	RoadNode* node = getNode(nodeId);
	if (!node) return;

	// 接続中の有効エッジの最大幅・幅差を求める
	float maxWidth  = 0.0f;
	float minWidth  = 1e9f;
	int   validCount = 0;
	for (const auto& att : node->attachments)
	{
		const RoadEdge* e = getEdge(att.edgeId);
		if (!e) continue;
		++validCount;
		const float w = e->totalWidth();
		maxWidth = Max(maxWidth, w);
		minWidth = Min(minWidth, w);
	}

	// ノード種別を有効エッジ数から自動判定
	if (validCount <= 1)
	{
		node->type = NodeType::Endpoint;
	}
	else if (validCount == 2)
	{
		node->type = NodeType::Joint;
	}
	else
	{
		// 3本以上: isThrough が2本あれば Diverge、なければ Intersection
		int throughCount = 0;
		for (const auto& att : node->attachments)
			if (att.isThrough) ++throughCount;
		node->type = (throughCount == 2) ? NodeType::Diverge : NodeType::Intersection;
	}

	// カットオフ値の計算
	float cutoff = 0.0f;
	switch (node->type)
	{
	case NodeType::Endpoint:
		cutoff = 0.0f;
		break;
	case NodeType::Joint:
		if (node->transition == NodeTransition::Blend)
		{
			// Blend: 幅差に応じた遷移ゾーン
			const float widthDiff = maxWidth - minWidth;
			cutoff = Max(widthDiff * 2.0f, 10.0f) * 0.5f;
		}
		else
		{
			// Abrupt: 最小限のキャップ
			cutoff = 0.1f;
		}
		break;
	case NodeType::Intersection:
	case NodeType::Diverge:
		cutoff = maxWidth * 1.5f;
		break;
	}

	// このノード端のカットオフ値を全接続エッジに書き込む
	// 短いエッジでは cutoff がエッジ長を超えないようクランプ
	for (const auto& att : node->attachments)
	{
		RoadEdge* e = getEdge(att.edgeId);
		if (!e) continue;
		const float maxCut = e->length * 0.4f;
		const float c = Min(cutoff, maxCut);
		if (e->nodeA == nodeId) e->cutoffA = c;
		else                    e->cutoffB = c;
	}
}

bool RoadNetwork::spreadIntersectionTangents()
{
	constexpr float kMinAngleDeg = 45.0;
	int adjustedNodes = 0;

	for (RoadNode& node : m_nodes)
	{
		if (node.id < 0 || node.attachments.size() < 3) continue;

		const Vec3 nodePos = node.position;

		struct EdgeEntry
		{
			int   edgeId;
			float ctrlLen; ///< node → 交差点側制御点の XZ 距離
			float ctrlY;   ///< 交差点側制御点の Y 座標
			float width;   ///< 道路幅（最も太い道路の判定用）
			float angle;   ///< 制御点方向の角度 [deg]（atan2 ベース、ソート用）
		};

		Array<EdgeEntry> entries;
		for (const auto& att : node.attachments)
		{
			const int eid = att.edgeId;
			const RoadEdge* e = getEdge(eid);
			if (!e) continue;

			const Vec3& ctrl = (e->nodeA == node.id) ? e->ctrlA : e->ctrlB;
			const float cdx = static_cast<float>(ctrl.x - nodePos.x);
			const float cdz = static_cast<float>(ctrl.z - nodePos.z);
			const float clen = std::sqrt(cdx * cdx + cdz * cdz);
			if (clen < 1e-6f) continue;

			EdgeEntry entry;
			entry.edgeId  = eid;
			entry.ctrlLen = clen;
			entry.ctrlY   = static_cast<float>(ctrl.y);
			entry.width   = e->totalWidth();
			entry.angle   = static_cast<float>(Math::ToDegrees(std::atan2(cdz, cdx)));
			entries << entry;
		}

		if (static_cast<int>(entries.size()) < 3) continue;

		// Step 3: 角度でソート（昇順 = CCW 順）
		entries.sort_by([](const EdgeEntry& a, const EdgeEntry& b){ return a.angle < b.angle; });

		const int n = static_cast<int>(entries.size());

		// Step 1 & 4: 最も太い道路を先頭に来るよう roll
		int widestIdx = 0;
		float maxWidth = -1.0f;
		for (int i = 0; i < n; ++i)
		{
			if (entries[i].width > maxWidth)
			{
				maxWidth  = entries[i].width;
				widestIdx = i;
			}
		}

		Array<EdgeEntry> rolled;
		for (int i = widestIdx; i < n; ++i) rolled << entries[i];
		for (int i = 0;         i < widestIdx; ++i) rolled << entries[i];

		// roll 後に角度を単調増加列に変換（ラップアラウンドを +360 で補正）
		Array<float> angles(n);
		angles[0] = rolled[0].angle;
		for (int i = 1; i < n; ++i)
		{
			float a = rolled[i].angle;
			while (a <= angles[i - 1]) a += 360.0f;
			angles[i] = a;
		}

		// Step 5: n 個目（n>=2）の要素について、前との差が kMinAngleDeg 未満なら補正
		bool changed = false;
		for (int i = 1; i < n; ++i)
		{
			if (angles[i] - angles[i - 1] < kMinAngleDeg)
			{
				angles[i] = angles[i - 1] + kMinAngleDeg + 1.0f;
				changed = true;
			}
		}

		// Step 6: 循環ギャップ補正（後ろから押し戻しパス）
		//
		// Step 5 の前向きパスで後方に押し出された要素が、
		// 「最後の要素 → 最初の要素（= 最太道路）」の循環ギャップを
		// kMinAngleDeg 未満に圧迫する場合、後ろから逆順に押し戻す。
		//
		// 押し戻し対象: angles[n-1] が angles[0]+360-kMinAngle より大きい場合、
		//   その値まで引き下げ、引き下げによって前の要素との差が不足すれば
		//   さらに前の要素も引き下げる（i=1 まで）。
		//
		// フォールバック: angles[1] を引き下げると angles[0]+kMinAngle を下回る
		//   （= 最太道路の手前まで詰まる）場合に限り、全要素を等間隔に配置する。
		//   これは n*kMinAngle > 360 のような極端な過密状態でのみ発生する。
		{
			// 最後の要素が収まる上限（ここを超えると循環ギャップが不足）
			const float maxLast = angles[0] + 360.0f - kMinAngleDeg;

			if (angles[n - 1] > maxLast)
			{
				changed = true;
				angles[n - 1] = maxLast;

				// 前の要素との差が不足する間、逆順に押し戻す
				bool needFallback = false;
				for (int i = n - 2; i >= 1; --i)
				{
					if (angles[i + 1] - angles[i] >= kMinAngleDeg)
						break;

					angles[i] = angles[i + 1] - kMinAngleDeg - 1.0f;

					if (angles[i] < angles[0] + kMinAngleDeg)
					{
						needFallback = true;
						break;
					}
				}

				if (needFallback)
				{
					for (int i = 1; i < n; ++i)
						angles[i] = angles[0] + static_cast<float>(i) * 360.0f / n;
				}
			}
		}

		if (!changed)
			continue;

		// Step 7: 新しい角度を制御点に反映（長さ・Y は変更しない）
		for (int i = 0; i < n; ++i)
		{
			RoadEdge* e = getEdge(rolled[i].edgeId);
			if (!e) continue;

			const float rad = static_cast<float>(Math::ToRadians(angles[i]));
			const Vec3 newCtrl{
				nodePos.x + std::cos(rad) * rolled[i].ctrlLen,
				rolled[i].ctrlY,
				nodePos.z + std::sin(rad) * rolled[i].ctrlLen
			};

			if (e->nodeA == node.id) e->ctrlA = newCtrl;
			else                     e->ctrlB = newCtrl;
		}

		++adjustedNodes;
	}

	Logger << U"[spreadIntersectionTangents] {} node(s) adjusted"_fmt(adjustedNodes);
	return adjustedNodes > 0;
}

void RoadNetwork::smoothCurveAt(int newEdgeId, int midNodeId)
{
	const RoadNode* midNode = getNode(midNodeId);
	if (!midNode) return;

	// 有効な接続エッジを列挙し、接続数が 2 でなければスキップ
	Array<int> validEdges;
	for (int eid : midNode->edgeIds())
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

void RoadNetwork::smoothAllCurves()
{
	for (const RoadNode& node : m_nodes)
	{
		if (node.id < 0 || node.attachments.size() != 2) continue;

		// 接続 2 本のうち、id が大きい方を newEdge とみなす
		const int eid0 = node.attachments[0].edgeId;
		const int eid1 = node.attachments[1].edgeId;
		const int newEdgeId = (eid0 > eid1) ? eid0 : eid1;
		smoothCurveAt(newEdgeId, node.id);
	}
}

bool RoadNetwork::removeDuplicateEdges(uint64 seed)
{
	bool removed = false;

	// ノードペア → 最初に見つけたエッジ ID のハッシュマップで O(E) に高速化
	// キー: (min(nodeA,nodeB), max(nodeA,nodeB)) をパックした int64
	HashTable<int64, int> pairMap;

	for (const RoadEdge& e : m_edges)
	{
		if (e.id < 0) continue;

		const int lo = Min(e.nodeA, e.nodeB);
		const int hi = Max(e.nodeA, e.nodeB);
		const int64 pairKey = (static_cast<int64>(lo) << 32) | static_cast<uint32>(hi);

		const auto it = pairMap.find(pairKey);
		if (it == pairMap.end())
		{
			pairMap[pairKey] = e.id;
			continue;
		}

		// 重複検出: 既存エッジと比較して道幅が小さい方を削除
		const RoadEdge* e1 = getEdge(it->second);
		if (!e1) { it->second = e.id; continue; }

		int toRemove;
		const float w1 = e1->totalWidth(), w2 = e.totalWidth();
		if (w1 < w2)
			toRemove = e1->id;
		else if (w2 < w1)
			toRemove = e.id;
		else
		{
			const uint64 elo = static_cast<uint64>(Min(e1->id, e.id));
			const uint64 ehi = static_cast<uint64>(Max(e1->id, e.id));
			const uint64 h   = (elo * 2654435761ULL ^ ehi * 2246822519ULL) ^ seed;
			toRemove = (h & 1) ? e1->id : e.id;
		}

		removeEdge(toRemove);
		removed = true;

		// 残った方のエッジ ID をマップに記録
		if (toRemove == it->second)
			it->second = e.id;
	}
	return removed;
}

bool RoadNetwork::resolveIntersections(int sinceEdgeId)
{
	// Case 1: NodeA→NodeB の直線同士が交わる → 交点で両エッジを分割
	// Case 2: Case 1 が外れ、かつ折れ線（端点→制御点1→制御点2→端点）の線分が交わる
	//         → 4 端点の平均位置にノードを生成し、両エッジを t=0.5 で分割
	// ノードマージ: 生成ノードが既存ノードと 40 m 以内ならマージ

	constexpr float MERGE_DIST = 40.0f;
	constexpr float SKIP_EPS   = 0.02f;

	// ---- 空間グリッド（AABB オーバーラップを高速化）----
	constexpr float CELL_SIZE = 256.0f;
	constexpr float INV_CELL  = 1.0f / CELL_SIZE;

	struct EdgeAABB { int id; float minX, minZ, maxX, maxZ; };

	// AABB を計算するラムダ
	auto computeAABB = [this](int edgeId) -> EdgeAABB
	{
		const RoadEdge* e = getEdge(edgeId);
		if (!e) return { -1, 0, 0, 0, 0 };
		const RoadNode* na = getNode(e->nodeA);
		const RoadNode* nb = getNode(e->nodeB);
		if (!na || !nb) return { -1, 0, 0, 0, 0 };
		const float x0 = static_cast<float>(Min({ na->position.x, nb->position.x, e->ctrlA.x, e->ctrlB.x }));
		const float x1 = static_cast<float>(Max({ na->position.x, nb->position.x, e->ctrlA.x, e->ctrlB.x }));
		const float z0 = static_cast<float>(Min({ na->position.z, nb->position.z, e->ctrlA.z, e->ctrlB.z }));
		const float z1 = static_cast<float>(Max({ na->position.z, nb->position.z, e->ctrlA.z, e->ctrlB.z }));
		return { edgeId, x0, z0, x1, z1 };
	};

	auto cellKey = [](int cx, int cz) -> int64
	{
		return (static_cast<int64>(cx) << 32) | static_cast<int64>(static_cast<uint32>(cz));
	};

	// sinceEdgeId 以上のエッジのみをダーティとして開始
	HashSet<int> dirtyEdges;
	for (const RoadEdge& e : m_edges)
		if (e.id >= sinceEdgeId) dirtyEdges.insert(e.id);

	bool everFound = false;

	while (!dirtyEdges.empty())
	{
		// ダーティリストからバッチ取得（処理中に変わるためコピー）
		Array<int> dirtyBatch(dirtyEdges.begin(), dirtyEdges.end());
		dirtyEdges.clear();

		// 空間グリッドを構築（全有効エッジの AABB をセルに登録）
		HashTable<int64, Array<int>> grid;
		HashTable<int, EdgeAABB> aabbCache;
		HashSet<int> allEdgeSet;

		for (const RoadEdge& e : m_edges)
		{
			if (e.id < 0) continue;
			allEdgeSet.insert(e.id);
			auto aabb = computeAABB(e.id);
			if (aabb.id < 0) continue;
			aabbCache[e.id] = aabb;

			const int cx0 = static_cast<int>(std::floor(aabb.minX * INV_CELL));
			const int cz0 = static_cast<int>(std::floor(aabb.minZ * INV_CELL));
			const int cx1 = static_cast<int>(std::floor(aabb.maxX * INV_CELL));
			const int cz1 = static_cast<int>(std::floor(aabb.maxZ * INV_CELL));
			for (int cz = cz0; cz <= cz1; ++cz)
				for (int cx = cx0; cx <= cx1; ++cx)
					grid[cellKey(cx, cz)] << e.id;
		}

		for (const int dirtyId : dirtyBatch)
		{
			if (!allEdgeSet.contains(dirtyId)) continue;
			const auto aabbIt = aabbCache.find(dirtyId);
			if (aabbIt == aabbCache.end()) continue;
			const auto& aabb1 = aabbIt->second;

			const RoadEdge* e1 = getEdge(dirtyId);
			if (!e1) continue;

			// ダーティエッジの AABB が重なるセルの全エッジを候補にする
			HashSet<int> candidates;
			const int cx0 = static_cast<int>(std::floor(aabb1.minX * INV_CELL));
			const int cz0 = static_cast<int>(std::floor(aabb1.minZ * INV_CELL));
			const int cx1 = static_cast<int>(std::floor(aabb1.maxX * INV_CELL));
			const int cz1 = static_cast<int>(std::floor(aabb1.maxZ * INV_CELL));
			for (int cz = cz0; cz <= cz1; ++cz)
				for (int cx = cx0; cx <= cx1; ++cx)
				{
					const auto git = grid.find(cellKey(cx, cz));
					if (git != grid.end())
						for (int eid : git->second)
							if (eid != dirtyId) candidates.insert(eid);
				}

			for (const int otherId : candidates)
			{
				if (!allEdgeSet.contains(otherId)) continue;

				const RoadEdge* e2 = getEdge(otherId);
				if (!e1 || !e2) break;

				// 隣接エッジ（共有ノードあり）はスキップ
				if (e1->nodeA == e2->nodeA || e1->nodeA == e2->nodeB ||
				    e1->nodeB == e2->nodeA || e1->nodeB == e2->nodeB) continue;

				// AABB 精密チェック
				const auto aabb2It = aabbCache.find(otherId);
				if (aabb2It == aabbCache.end()) continue;
				const auto& aabb2 = aabb2It->second;
				if (aabb1.maxX < aabb2.minX || aabb2.maxX < aabb1.minX ||
				    aabb1.maxZ < aabb2.minZ || aabb2.maxZ < aabb1.minZ) continue;

				const RoadNode* na1 = getNode(e1->nodeA); const RoadNode* nb1 = getNode(e1->nodeB);
				const RoadNode* na2 = getNode(e2->nodeA); const RoadNode* nb2 = getNode(e2->nodeB);
				if (!na1 || !nb1 || !na2 || !nb2) continue;

				// データを全てコピー（以降のポインタ失効に備える）
				const Vec3 posA1 = na1->position, posB1 = nb1->position;
				const Vec3 posA2 = na2->position, posB2 = nb2->position;
				const Vec3 cA1 = e1->ctrlA, cB1 = e1->ctrlB;
				const Vec3 cA2 = e2->ctrlA, cB2 = e2->ctrlB;
				const RoadType rt1 = e1->roadType, rt2 = e2->roadType;
				const int lanes1 = static_cast<int>(e1->lanes.size());
				const int lanes2 = static_cast<int>(e2->lanes.size());
				const int nA1 = e1->nodeA, nB1 = e1->nodeB;
				const int nA2 = e2->nodeA, nB2 = e2->nodeB;

				float bt1, bt2;
				Vec3 intPos;

				float s, t;
				if (segIntersect2D(
					{ posA1.x, posA1.z }, { posB1.x, posB1.z },
					{ posA2.x, posA2.z }, { posB2.x, posB2.z }, s, t))
				{
					bt1 = s; bt2 = t;
					if (bt1 < SKIP_EPS || bt1 > 1.0f - SKIP_EPS) continue;
					if (bt2 < SKIP_EPS || bt2 > 1.0f - SKIP_EPS) continue;
					intPos = posA1 + (posB1 - posA1) * bt1;
				}
				else
				{
					float ds, dt;
					const Vec2 p1[4] = {
						{ posA1.x, posA1.z }, { cA1.x, cA1.z },
						{ cB1.x, cB1.z }, { posB1.x, posB1.z }
					};
					const Vec2 p2[4] = {
						{ posA2.x, posA2.z }, { cA2.x, cA2.z },
						{ cB2.x, cB2.z }, { posB2.x, posB2.z }
					};
					bool cpHit = false;
					for (int si = 0; si < 3 && !cpHit; ++si)
						for (int sj = 0; sj < 3 && !cpHit; ++sj)
							cpHit = segIntersect2D(p1[si], p1[si + 1], p2[sj], p2[sj + 1], ds, dt);
					if (!cpHit) continue;

					bt1 = bt2 = 0.5f;
					intPos = Vec3{
						(posA1.x + posB1.x + posA2.x + posB2.x) * 0.25f,
						(posA1.y + posB1.y + posA2.y + posB2.y) * 0.25f,
						(posA1.z + posB1.z + posA2.z + posB2.z) * 0.25f
					};
				}

				// 既存ノードへのマージ判定（近傍セルのノードのみ検索）
				int splitNodeId = -1;
				{
					float minDist = MERGE_DIST;
					for (const RoadNode& n : m_nodes)
					{
						if (n.id < 0) continue;
						if (n.id == nA1 || n.id == nB1 || n.id == nA2 || n.id == nB2) continue;
						const float d = static_cast<float>(intPos.distanceFrom(n.position));
						if (d < minDist) { minDist = d; splitNodeId = n.id; }
					}
				}

				if (splitNodeId < 0)
					splitNodeId = addNode(intPos, NodeType::Intersection);

				const Vec3 splitPos = getNode(splitNodeId)->position;

				// E1 を分割
				removeEdge(dirtyId);
				const auto newE1a = addEdge(nA1, splitNodeId,
					posA1 + (splitPos - posA1) * (1.0 / 3.0),
					posA1 + (splitPos - posA1) * (2.0 / 3.0), rt1, lanes1);
				const auto newE1b = addEdge(splitNodeId, nB1,
					splitPos + (posB1 - splitPos) * (1.0 / 3.0),
					splitPos + (posB1 - splitPos) * (2.0 / 3.0), rt1, lanes1);

				// E2 を分割
				removeEdge(otherId);
				const auto newE2a = addEdge(nA2, splitNodeId,
					posA2 + (splitPos - posA2) * (1.0 / 3.0),
					posA2 + (splitPos - posA2) * (2.0 / 3.0), rt2, lanes2);
				const auto newE2b = addEdge(splitNodeId, nB2,
					splitPos + (posB2 - splitPos) * (1.0 / 3.0),
					splitPos + (posB2 - splitPos) * (2.0 / 3.0), rt2, lanes2);

				// 新しいエッジをダーティに登録
				if (newE1a) dirtyEdges.insert(*newE1a);
				if (newE1b) dirtyEdges.insert(*newE1b);
				if (newE2a) dirtyEdges.insert(*newE2a);
				if (newE2b) dirtyEdges.insert(*newE2b);

				// 削除済みエッジをセットから除去
				allEdgeSet.erase(dirtyId);
				allEdgeSet.erase(otherId);

				everFound = true;
				break;  // e1 は削除されたのでこのダーティエッジの処理を終了
			}
		}
	}

	return everFound;
}

bool RoadNetwork::fixSharpAngles(float minAngleDeg)
{
	const float cosThresh = static_cast<float>(Math::Cos(Math::ToRadians(minAngleDeg)));
	bool anyFixed = false;

	// 発振防止: 付け替え済みのノードペアを記録し、同じペアの再生成を防ぐ
	static HashSet<int64> s_processedPairs;
	auto pairKey = [](int a, int b) -> int64
	{
		if (a > b) std::swap(a, b);
		return (static_cast<int64>(a) << 32) | static_cast<int64>(static_cast<uint32>(b));
	};

	// イテレーション中に m_nodes の要素変更が起きるため、先に ID を収集する
	Array<int> nodeIds;
	for (const RoadNode& n : m_nodes)
		if (n.id >= 0) nodeIds << n.id;

	for (const int nodeId : nodeIds)
	{
		const RoadNode* node = getNode(nodeId);
		if (!node || node->attachments.size() < 2) continue;

		// edgeIds をコピーしておく（removeEdge / addEdge で変化するため）
		const Array<int> edgesCopy = node->edgeIds();
		// node->position は removeEdge/addEdge では m_nodes が resize されないため有効
		const Vec3 nodePos = node->position;

		bool fixed = false;
		for (int i = 0; i < static_cast<int>(edgesCopy.size()) && !fixed; ++i)
		{
			for (int j = i + 1; j < static_cast<int>(edgesCopy.size()) && !fixed; ++j)
			{
				const RoadEdge* e1 = getEdge(edgesCopy[i]);
				const RoadEdge* e2 = getEdge(edgesCopy[j]);
				if (!e1 || !e2) continue;

				// ノードからの outward タンジェント（XZ 平面）
				const Vec3 raw1 = (e1->nodeA == nodeId)
					? (e1->ctrlA - nodePos) : (e1->ctrlB - nodePos);
				const Vec3 raw2 = (e2->nodeA == nodeId)
					? (e2->ctrlA - nodePos) : (e2->ctrlB - nodePos);
				const Vec2 d1xz{ raw1.x, raw1.z };
				const Vec2 d2xz{ raw2.x, raw2.z };
				if (d1xz.length() < 1e-6f || d2xz.length() < 1e-6f) continue;

				const float cosAngle = static_cast<float>(
					d1xz.normalized().dot(d2xz.normalized()));
				if (cosAngle <= cosThresh) continue; // 角度 >= minAngleDeg → OK

				// 幅が狭い方を付け替える（同幅なら e2 を選択）
				const RoadEdge* narrower = (e1->totalWidth() < e2->totalWidth()) ? e1 : e2;
				const int  narrowerId      = narrower->id;
				const int  narrowerOtherId = (narrower->nodeA == nodeId)
					? narrower->nodeB : narrower->nodeA;
				const RoadType narrowerRt    = narrower->roadType;
				const int      narrowerLanes = static_cast<int>(narrower->lanes.size());

				// 付け替え先候補: このノードの隣接ノード（狭いエッジの他端は除外）
				Array<int> candidateIds;
				for (int eid : edgesCopy)
				{
					if (eid == narrowerId) continue;
					const RoadEdge* e = getEdge(eid);
					if (!e) continue;
					const int otherId = (e->nodeA == nodeId) ? e->nodeB : e->nodeA;
					if (otherId != narrowerOtherId)
						candidateIds << otherId;
				}
				if (candidateIds.isEmpty()) continue;

				// 最も近い候補ノードを選ぶ
				int   nearestId = -1;
				float minDist   = 1e30f;
				for (int cid : candidateIds)
				{
					const RoadNode* cn = getNode(cid);
					if (!cn) continue;
					const float d = static_cast<float>(nodePos.distanceFrom(cn->position));
					if (d < minDist) { minDist = d; nearestId = cid; }
				}
				if (nearestId < 0) continue;

				// 発振防止: 同じノードペアの付け替えが既に行われていたらスキップ
				const int64 pk = pairKey(nearestId, narrowerOtherId);
				if (s_processedPairs.contains(pk)) continue;
				s_processedPairs.insert(pk);
				// 元のペアも記録（逆方向の付け替えも防ぐ）
				s_processedPairs.insert(pairKey(nodeId, narrowerOtherId));

				// 付け替え後のノード位置を取得（ポインタは removeEdge 後に再取得）
				const RoadNode* nearestNode = getNode(nearestId);
				const RoadNode* otherNode   = getNode(narrowerOtherId);
				if (!nearestNode || !otherNode) continue;

				// 制御点: 1/3, 2/3 線形補間
				const Vec3 posA  = nearestNode->position;
				const Vec3 posB  = otherNode->position;
				const Vec3 ctrlA = posA + (posB - posA) * (1.0 / 3.0);
				const Vec3 ctrlB = posA + (posB - posA) * (2.0 / 3.0);

				removeEdge(narrowerId);
				addEdge(nearestId, narrowerOtherId, ctrlA, ctrlB, narrowerRt, narrowerLanes);

				fixed = anyFixed = true;
			}
		}
	}

	// 全イテレーション完了後にリセット（呼び出し側ループの最終回で anyFixed=false になる）
	if (!anyFixed)
		s_processedPairs.clear();

	return anyFixed;
}

void RoadNetwork::buildDefaultParts(RoadEdge& edge)
{
	edge.parts.clear();

	const float laneW = (edge.roadType == RoadType::Expressway || edge.roadType == RoadType::Highway)
		? 3.75f : 3.5f;
	const int nLanes = static_cast<int>(edge.lanes.size());
	const float roadbedWidth = nLanes * laneW;
	const float halfRoadbed = roadbedWidth * 0.5f;

	auto addPart = [&](RoadPartType type, float offset, float width, StringView defId = U"")
	{
		RoadPart p;
		p.type   = type;
		p.defId  = String{ defId };
		p.width  = width;
		p.offset = offset;
		p.build  = BuildState::Built;
		edge.parts << p;
	};

	switch (edge.roadType)
	{
	case RoadType::Expressway:
	case RoadType::Highway:
	{
		const float shoulderW = 1.0f;
		const float guardrailW = 0.5f;
		const float slopeW = 3.0f;
		const float medianW = 2.0f;
		const float halfForward = (nLanes / 2) * laneW;

		float x = -(halfRoadbed + medianW * 0.5f) - shoulderW - guardrailW - slopeW;
		addPart(RoadPartType::Slope,     x, slopeW,      U"slope_grass");          x += slopeW;
		addPart(RoadPartType::Guardrail, x, guardrailW,  U"guardrail_steel");      x += guardrailW;
		addPart(RoadPartType::Shoulder,  x, shoulderW,   U"roadbed_asphalt");      x += shoulderW;
		addPart(RoadPartType::Roadbed,   x, halfForward, U"roadbed_asphalt");      x += halfForward;
		addPart(RoadPartType::Median,    x, medianW,     U"median_concrete");      x += medianW;
		addPart(RoadPartType::Roadbed,   x, roadbedWidth - halfForward, U"roadbed_asphalt"); x += roadbedWidth - halfForward;
		addPart(RoadPartType::Shoulder,  x, shoulderW,   U"roadbed_asphalt");      x += shoulderW;
		addPart(RoadPartType::Guardrail, x, guardrailW,  U"guardrail_steel");      x += guardrailW;
		addPart(RoadPartType::Slope,     x, slopeW,      U"slope_grass");
		break;
	}
	case RoadType::Arterial:
	{
		const float sidewalkW = 2.5f;
		const float curbW = 0.2f;
		const float slopeW = 2.0f;

		float x = -halfRoadbed - curbW - sidewalkW - slopeW;
		addPart(RoadPartType::Slope,    x, slopeW,      U"slope_grass");     x += slopeW;
		addPart(RoadPartType::Sidewalk, x, sidewalkW,   U"sidewalk_tile");   x += sidewalkW;
		addPart(RoadPartType::Curb,     x, curbW,       U"curb_concrete");   x += curbW;
		addPart(RoadPartType::Roadbed,  x, roadbedWidth, U"roadbed_asphalt"); x += roadbedWidth;
		addPart(RoadPartType::Curb,     x, curbW,       U"curb_concrete");   x += curbW;
		addPart(RoadPartType::Sidewalk, x, sidewalkW,   U"sidewalk_tile");   x += sidewalkW;
		addPart(RoadPartType::Slope,    x, slopeW,      U"slope_grass");
		break;
	}
	default: // LocalRoad
	{
		const float slopeW = 2.0f;
		float x = -halfRoadbed - slopeW;
		addPart(RoadPartType::Slope,   x, slopeW,      U"slope_grass");      x += slopeW;
		addPart(RoadPartType::Roadbed, x, roadbedWidth, U"roadbed_asphalt"); x += roadbedWidth;
		addPart(RoadPartType::Slope,   x, slopeW,      U"slope_grass");
		break;
	}
	}
}

Array<Lane> RoadNetwork::buildDefaultLanes(int numLanes, RoadType rt)
{
	float laneWidth = 3.5f;
	if (rt == RoadType::Expressway || rt == RoadType::Highway)
		laneWidth = 3.75f;

	// Forward レーン数: 過半数（偶数なら半分、奇数なら切り上げ）
	const int forwardCount = (numLanes + 1) / 2;
	const float halfTotal = numLanes * laneWidth * 0.5f;

	Array<Lane> lanes;
	lanes.reserve(numLanes);

	for (int i = 0; i < numLanes; ++i)
	{
		Lane lane;

		// 運用
		lane.dir = (i < forwardCount) ? LaneDir::Forward : LaneDir::Backward;
		lane.op  = OpState::Open;

		// 幾何（A端=B端、テーパーなし）
		const float left  = i * laneWidth - halfTotal;
		const float right = left + laneWidth;
		lane.offsetA_L = left;
		lane.offsetA_R = right;
		lane.offsetB_L = left;
		lane.offsetB_R = right;
		lane.nominalWidth = laneWidth;

		// 区画線
		const bool isLeftmost  = (i == 0);
		const bool isRightmost = (i == numLanes - 1);
		const bool isCenterBoundary = (i > 0)
			&& (((i - 1 < forwardCount) && (i >= forwardCount))
			 || ((i - 1 >= forwardCount) && (i < forwardCount)));

		// 左側の線
		if (isLeftmost)
			lane.lineLeft = LineType::SolidWhite;     // 道路端
		else if (isCenterBoundary)
			lane.lineLeft = LineType::SolidYellow;     // 対向車線境界
		else
			lane.lineLeft = LineType::DashedWhite;     // 同方向車線境界

		// 右側の線
		if (isRightmost)
			lane.lineRight = LineType::SolidWhite;
		else
		{
			const bool isNextCenterBoundary = (i + 1 > 0)
				&& (((i < forwardCount) && (i + 1 >= forwardCount))
				 || ((i >= forwardCount) && (i + 1 < forwardCount)));
			if (isNextCenterBoundary)
				lane.lineRight = LineType::SolidYellow;
			else
				lane.lineRight = LineType::DashedWhite;
		}

		// 車線変更（同方向の隣接車線があれば可能）
		if (i > 0 && lanes[i - 1].dir == lane.dir)
		{
			lane.canChangeLaneLeft = true;
			lanes[i - 1].canChangeLaneRight = true;
		}

		lane.type = LaneType::Normal;
		lanes << lane;
	}
	return lanes;
}

// ─────────────────────────────────────────────────────────────────────────────
// 短エッジ結合
// ─────────────────────────────────────────────────────────────────────────────

int RoadNetwork::mergeShortEdges(float minLength)
{
	int merged = 0;
	bool changed = true;

	while (changed)
	{
		changed = false;
		for (auto& edge : m_edges)
		{
			if (edge.id < 0) continue;
			if (edge.length > minLength) continue;

			const int keepId = edge.nodeA;
			const int rmId   = edge.nodeB;
			RoadNode* keepNode = getNode(keepId);
			RoadNode* rmNode   = getNode(rmId);
			if (!keepNode || !rmNode) continue;
			if (keepId == rmId) continue;

			// rmNode の接続エッジを keepNode に移し替える
			for (const int eid : rmNode->edgeIds())
			{
				if (eid == edge.id) continue;
				RoadEdge* e = getEdge(eid);
				if (!e) continue;

				if (e->nodeA == rmId) e->nodeA = keepId;
				if (e->nodeB == rmId) e->nodeB = keepId;

				if (e->nodeA == keepId && e->nodeB == keepId)
				{
					removeEdge(eid);
					continue;
				}

				if (!keepNode->getAttachment(eid))
					keepNode->addEdge(eid);
			}

			keepNode->position = (keepNode->position + rmNode->position) * 0.5;

			removeEdge(edge.id);
			removeNode(rmId);

			++merged;
			changed = true;
			break;  // イテレータ無効化のためループ再開
		}
	}

	return merged;
}

// ===== LaneConnection 構築 =====

/// @brief カットオフ位置での車線端点（ワールド座標・接線・方向符号）を計算する
struct LaneEndpoint
{
	Vec3  worldPos;  ///< 車線中心のワールド座標
	Vec3  tangent;   ///< ベジェ接線ベクトル
	float dirSign;   ///< Forward: +1, Backward: -1
};

static LaneEndpoint calcLaneEndpoint(
	const CubicBezier& bez, const RoadEdge& edge, const Lane& lane,
	int nodeId)
{
	const bool isNodeA = (edge.nodeA == nodeId);
	const float cutoff = isNodeA ? edge.cutoffA : edge.cutoffB;
	const float arc = isNodeA
		? cutoff
		: (bez.totalLength - cutoff);

	const Vec3 pos = bez.positionAt(arc);
	const Vec3 tan = bez.tangentAt(arc);

	// 車線オフセットを適用
	const float centerA = (lane.offsetA_L + lane.offsetA_R) * 0.5f;
	const float centerB = (lane.offsetB_L + lane.offsetB_R) * 0.5f;
	const float ft = (bez.totalLength > 0.0f) ? (arc / bez.totalLength) : 0.0f;
	const float offset = centerA + (centerB - centerA) * ft;
	const Vec3 perp = Vec3{ tan.z, 0.0, -tan.x }.normalized();

	return {
		pos + perp * static_cast<double>(offset),
		tan,
		(lane.dir == LaneDir::Forward) ? 1.0f : -1.0f
	};
}

void RoadNetwork::rebuildLaneConnections(int nodeId)
{
	RoadNode* node = getNode(nodeId);
	if (!node) return;

	node->laneConnections.clear();

	const auto edgeIds = node->edgeIds();
	if (edgeIds.size() < 2) return;

	for (const int fromEid : edgeIds)
	{
		const RoadEdge* fromEdge = getEdge(fromEid);
		if (!fromEdge || !fromEdge->isRoadbedBuilt()) continue;

		for (int fi = 0; fi < static_cast<int>(fromEdge->lanes.size()); ++fi)
		{
			const Lane& fromLane = fromEdge->lanes[fi];
			if (fromLane.op != OpState::Open && fromLane.op != OpState::Provisional) continue;

			const bool exitsAtNode =
				(fromLane.dir == LaneDir::Forward  && fromEdge->nodeB == nodeId) ||
				(fromLane.dir == LaneDir::Backward && fromEdge->nodeA == nodeId);
			if (!exitsAtNode) continue;

			const auto fromBez = getBezier(fromEid);
			if (!fromBez) continue;

			const auto exitPt = calcLaneEndpoint(*fromBez, *fromEdge, fromLane, nodeId);

			for (const int toEid : edgeIds)
			{
				if (toEid == fromEid) continue;
				const RoadEdge* toEdge = getEdge(toEid);
				if (!toEdge || !toEdge->isRoadbedBuilt()) continue;

				for (int ti = 0; ti < static_cast<int>(toEdge->lanes.size()); ++ti)
				{
					const Lane& toLane = toEdge->lanes[ti];
					if (toLane.op != OpState::Open && toLane.op != OpState::Provisional) continue;

					const bool entersAtNode =
						(toLane.dir == LaneDir::Forward  && toEdge->nodeA == nodeId) ||
						(toLane.dir == LaneDir::Backward && toEdge->nodeB == nodeId);
					if (!entersAtNode) continue;

					const auto toBez = getBezier(toEid);
					if (!toBez) continue;

					const auto entryPt = calcLaneEndpoint(*toBez, *toEdge, toLane, nodeId);

					// 旋回ベジェを生成
					const double dist = (exitPt.worldPos - entryPt.worldPos).length();
					const double handle = Max(dist * 0.33, 5.0);
					const Vec3 p1 = exitPt.worldPos + (exitPt.tangent * exitPt.dirSign).normalized() * handle;
					const Vec3 p2 = entryPt.worldPos - (entryPt.tangent * entryPt.dirSign).normalized() * handle;

					LaneConnection conn;
					conn.id            = node->nextConnectionId++;
					conn.fromEdgeId    = fromEid;
					conn.fromLaneIndex = fi;
					conn.toEdgeId      = toEid;
					conn.toLaneIndex   = ti;
					conn.path          = CubicBezier{ exitPt.worldPos, p1, p2, entryPt.worldPos };

					node->laneConnections << std::move(conn);
				}
			}
		}
	}
}

// =============================================================================
// RoadObject CRUD
// =============================================================================

int RoadNetwork::addObject(RoadObject obj)
{
	obj.id = m_nextObjectId++;
	if (!m_freeObjectSlots.isEmpty())
	{
		const int idx = m_freeObjectSlots.back();
		m_freeObjectSlots.pop_back();
		m_objects[idx] = obj;
		m_objectIdToIdx[obj.id] = idx;
	}
	else
	{
		m_objectIdToIdx[obj.id] = static_cast<int>(m_objects.size());
		m_objects << obj;
	}
	return obj.id;
}

void RoadNetwork::removeObject(int objectId)
{
	const int idx = objectIndex(objectId);
	if (idx < 0) return;
	m_objects[idx].id = -1;
	m_objectIdToIdx.erase(objectId);
	m_freeObjectSlots << idx;
}

void RoadNetwork::removeObjectsByEdge(int edgeId)
{
	for (int i = 0; i < static_cast<int>(m_objects.size()); ++i)
	{
		auto& obj = m_objects[i];
		if (obj.id >= 0 && obj.parentEdgeId == edgeId)
		{
			m_objectIdToIdx.erase(obj.id);
			obj.id = -1;
			m_freeObjectSlots << i;
		}
	}
}

RoadObject* RoadNetwork::getObject(int id)
{
	const int idx = objectIndex(id);
	return (idx >= 0 && m_objects[idx].id >= 0) ? &m_objects[idx] : nullptr;
}

const RoadObject* RoadNetwork::getObject(int id) const
{
	const int idx = objectIndex(id);
	return (idx >= 0 && m_objects[idx].id >= 0) ? &m_objects[idx] : nullptr;
}

bool RoadNetwork::isNodeElevated(int nodeId) const
{
	const RoadNode* node = getNode(nodeId);
	if (!node) return false;
	for (const auto& att : node->attachments)
	{
		const RoadEdge* e = getEdge(att.edgeId);
		if (e && e->useElevation) return true;
	}
	return false;
}

void RoadNetwork::updateEdgeElevation(int edgeId, const World& world)
{
	RoadEdge* edge = getEdge(edgeId);
	if (!edge) return;
	const RoadNode* nA = getNode(edge->nodeA);
	const RoadNode* nB = getNode(edge->nodeB);
	if (!nA || !nB) return;
	const double gyA = world.computeHeight(
		static_cast<float>(nA->position.x), static_cast<float>(nA->position.z));
	const double gyB = world.computeHeight(
		static_cast<float>(nB->position.x), static_cast<float>(nB->position.z));
	edge->useElevation =
		std::abs(nA->position.y - gyA) > kElevationThreshold ||
		std::abs(nB->position.y - gyB) > kElevationThreshold;
}

void RoadNetwork::generatePiersForEdge(int edgeId, const World& world)
{
	const RoadEdge* edge = getEdge(edgeId);
	if (!edge || !edge->useElevation) return;

	// 既存橋脚を削除
	removeObjectsByEdge(edgeId);

	const auto bez = getBezier(edgeId);
	if (!bez) return;

	constexpr float kPierInterval  = 30.0f;
	constexpr float kPierThreshold = 3.0f;

	const float totalLen = bez->totalLength;
	const int steps = Max(1, static_cast<int>(totalLen / kPierInterval));

	for (int i = 1; i < steps; ++i)
	{
		const float s = kPierInterval * i;
		if (s >= totalLen) break;

		const Vec3 pos = bez->positionAt(s);
		const float terrainY = world.computeHeight(
			static_cast<float>(pos.x), static_cast<float>(pos.z));
		const float gap = static_cast<float>(pos.y) - terrainY;

		if (gap >= kPierThreshold)
		{
			RoadObject pier;
			pier.parentEdgeId = edgeId;
			pier.arcPos = s;
			pier.type = RoadObjectType::Pier;
			addObject(pier);
		}
	}
}

