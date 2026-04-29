#include "RoadNetwork.hpp"
#include "RoadSign.hpp"
#include "GuideSign.hpp"
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
	e.edgeState    = EdgeState::Planned;

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

	// 所属 route から除去 or 分割（整合性フック）
	onEdgeRemovedFromRoutes(edgeId);
	onEdgeRemovedFromPlans(edgeId);

	e.id = -1;
	m_edgeIdToIdx.erase(edgeId);
	m_freeEdgeSlots << idx;
	removeObjectsByEdge(edgeId);
	removeGuideSignsByEdge(edgeId);

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

	// Route 所属のスナップショット: (routeId, position) を保存
	// removeEdge で route から除去される前に記録、後で新 edge で再挿入する
	struct RouteMember { int routeId; int pos; };
	Array<RouteMember> routeSnap;
	for (const auto& r : m_routes)
	{
		if (r.id < 0) continue;
		auto it = std::find(r.edgeIds.begin(), r.edgeIds.end(), edgeId);
		if (it != r.edgeIds.end())
			routeSnap << RouteMember{ r.id, static_cast<int>(it - r.edgeIds.begin()) };
	}

	removeEdge(edgeId);

	const int midNodeId = addNode(splitPos, NodeType::Joint);

	int newEidA = -1, newEidB = -1;
	if (auto eidA = addEdge(origNodeA, midNodeId, bezA.p1, bezA.p2, rt, numLanes))
	{
		applyEdgeTemplate(*eidA, tmpl);
		if (RoadEdge* ea = getEdge(*eidA))
		{
			ea->edgeState             = tmpl.edgeState;
			ea->constructionStartTime = tmpl.constructionStartTime;
			ea->useElevation          = tmpl.useElevation;
		}
		newEidA = *eidA;
	}

	if (auto eidB = addEdge(midNodeId, origNodeB, bezB.p1, bezB.p2, rt, numLanes))
	{
		applyEdgeTemplate(*eidB, tmpl);
		if (RoadEdge* eb = getEdge(*eidB))
		{
			eb->edgeState             = tmpl.edgeState;
			eb->constructionStartTime = tmpl.constructionStartTime;
			eb->useElevation          = tmpl.useElevation;
		}
		newEidB = *eidB;
	}

	// Route 所属を再挿入（A→B 順で挿入。route の traversal 方向が B→A の場合は
	// 手動修正が必要な場合あり。plan/22_road_route_spec.md §4 参照）
	if (newEidA >= 0 && newEidB >= 0)
	{
		for (const auto& rm : routeSnap)
		{
			// 注: removeEdge → onEdgeRemovedFromRoutes によって元 route は既に
			// 端削除 or 中間分割されている可能性がある。
			// ここではシンプルに「元 route id が残っていて、かつ新 edge を含まない」場合に
			// 新 edge をペアで追記する実装とする。
			// 中間分割された場合: 元 route は前半のみ、後半は別 route として分離済み。
			//   前半の末尾に newEidA を追加、後半の先頭に newEidB を追加
			//   → ただし分離情報が無いので、複雑。v1 では割愛。
			//   端削除の場合: route から元 edge が消えているので、先頭/末尾に挿入
			RoadRoute* r = getRoute(rm.routeId);
			if (!r) continue;
			if (r->edgeIds.contains(newEidA) || r->edgeIds.contains(newEidB)) continue;

			// 元 pos が端だった → 同じ端に再挿入、中間だった場合は挿入スキップ（分割済み）
			if (rm.pos == 0)
			{
				r->edgeIds.insert(r->edgeIds.begin(), { newEidA, newEidB });
				if (RoadEdge* ea = getEdge(newEidA)) if (!ea->routeIds.contains(r->id)) ea->routeIds << r->id;
				if (RoadEdge* eb = getEdge(newEidB)) if (!eb->routeIds.contains(r->id)) eb->routeIds << r->id;
			}
			else
			{
				// 末尾または中間（分割済み）: 末尾なら push_back、中間は skip
				r->edgeIds << newEidA;
				r->edgeIds << newEidB;
				if (RoadEdge* ea = getEdge(newEidA)) if (!ea->routeIds.contains(r->id)) ea->routeIds << r->id;
				if (RoadEdge* eb = getEdge(newEidB)) if (!eb->routeIds.contains(r->id)) eb->routeIds << r->id;
			}
		}
	}

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

Optional<int> RoadNetwork::dissolveNode(int nodeId)
{
	RoadNode* mid = getNode(nodeId);
	if (!mid || mid->attachments.size() != 2) return none;

	const int eid0 = mid->attachments[0].edgeId;
	const int eid1 = mid->attachments[1].edgeId;
	RoadEdge* e0 = getEdge(eid0);
	RoadEdge* e1 = getEdge(eid1);
	if (!e0 || !e1) return none;

	// e0 の「中間ノードでない側」が新エッジの nodeA
	const int farA = (e0->nodeA == nodeId) ? e0->nodeB : e0->nodeA;
	// e1 の「中間ノードでない側」が新エッジの nodeB
	const int farB = (e1->nodeA == nodeId) ? e1->nodeB : e1->nodeA;
	if (farA == farB) return none;

	// 制御点: farA 側は e0 の farA 側制御点、farB 側は e1 の farB 側制御点
	const Vec3 ctrlA = (e0->nodeA == nodeId) ? e0->ctrlB : e0->ctrlA;
	const Vec3 ctrlB = (e1->nodeA == nodeId) ? e1->ctrlB : e1->ctrlA;

	// 属性は e0 から引き継ぐ
	const RoadType rt = e0->roadType;
	const int numLanes = static_cast<int>(e0->lanes.size());

	// 旧エッジ・ノードを削除
	removeEdge(eid0);
	removeEdge(eid1);
	removeNode(nodeId);

	// 統合エッジを追加
	return addEdge(farA, farB, ctrlA, ctrlB, rt, numLanes);
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
		p.type      = type;
		p.defId     = String{ defId };
		p.offsetA_L = offset;
		p.offsetA_R = offset + width;
		p.offsetB_L = offset;
		p.offsetB_R = offset + width;
		p.build     = BuildState::Built;
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

	// 分離帯のある道路（Expressway/Highway）は中央に medianW の隙間を空けて配置し、
	// buildDefaultParts の Median Part と車線が重ならないようにする
	const bool hasMedian = (rt == RoadType::Expressway || rt == RoadType::Highway);
	const float medianW = hasMedian ? 2.0f : 0.0f;

	// Forward レーン数: 分離帯ありなら前半（左側）、無しなら過半数（切り上げ）
	const int forwardCount = hasMedian ? (numLanes / 2) : ((numLanes + 1) / 2);
	const float halfForward = forwardCount * laneWidth;

	Array<Lane> lanes;
	lanes.reserve(numLanes);

	for (int i = 0; i < numLanes; ++i)
	{
		Lane lane;

		// 運用
		lane.dir = (i < forwardCount) ? LaneDir::Forward : LaneDir::Backward;
		lane.op  = OpState::Open;

		// 幾何（A端=B端、テーパーなし）
		// Forward 側は [-halfForward - medianW/2, -medianW/2]、
		// Backward 側は [+medianW/2, ...] に配置する
		float left;
		if (i < forwardCount)
			left = -halfForward - medianW * 0.5f + i * laneWidth;
		else
			left = medianW * 0.5f + (i - forwardCount) * laneWidth;
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
	const float ft = (bez.totalLength > 0.0f) ? (arc / bez.totalLength) : 0.0f;
	const float offset = lane.centerAt(ft);
	const Vec3 perp = tangentToRight(tan);

	return {
		pos + perp * static_cast<double>(offset),
		tan,
		(lane.dir == LaneDir::Forward) ? 1.0f : -1.0f
	};
}

void RoadNetwork::recomputeAutoSignsForEdge(int edgeId)
{
	RoadEdge* edge = getEdge(edgeId);
	if (!edge) return;
	// 既存の自動エントリを削除（手動配置は保持）
	edge->signs.remove_if([](const RoadSignPlacement& s) { return s.autoGenerated; });
	// 自動生成
	const auto autos = RoadSign::InferAutoForEdge(*edge, *this);
	for (const auto& s : autos) edge->signs << s;
}

void RoadNetwork::recomputeAllAutoSigns()
{
	for (auto& e : m_edges)
	{
		if (e.id < 0) continue;
		recomputeAutoSignsForEdge(e.id);
	}
}

void RoadNetwork::recomputeAutoGuideSignsForEdge(int edgeId)
{
	const RoadEdge* edge = getEdge(edgeId);
	if (!edge) return;
	recomputeAutoGuideSignsForNode(edge->nodeA);
	recomputeAutoGuideSignsForNode(edge->nodeB);
}

void RoadNetwork::recomputeAutoGuideSignsForNode(int nodeId)
{
	// このノード起点の自動案内標識だけを張り直し、手動配置や他ノード由来の標識は残す。
	// sourceNodeId == nodeId の自動標識を一括削除
	Array<int> removeIds;
	for (const auto& g : m_guideSigns)
	{
		if (g.id >= 0 && g.autoGenerated && g.sourceNodeId == nodeId)
		{
			removeIds << g.id;
		}
	}
	for (int id : removeIds)
	{
		removeGuideSign(id);
	}

	// nodeId に接続する全エッジで InferAutoForEdge を呼び、sourceNodeId==nodeId のものだけ追加
	const RoadNode* node = getNode(nodeId);
	if (!node)
	{
		return;
	}
	for (const auto& att : node->attachments)
	{
		const RoadEdge* edge = getEdge(att.edgeId);
		if (!edge)
		{
			continue;
		}
		auto autos = GuideSign::InferAutoForEdge(*edge, *this);
		for (auto& g : autos)
		{
			if (g.sourceNodeId != nodeId)
			{
				continue;
			}
			if (g.parentEdgeId < 0)
			{
				g.parentEdgeId = att.edgeId;
			}
			addGuideSign(std::move(g));
		}
	}
}

void RoadNetwork::recomputeAllAutoGuideSigns()
{
	for (auto& n : m_nodes)
	{
		if (n.id < 0) continue;
		recomputeAutoGuideSignsForNode(n.id);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// GuideSign CRUD
// ─────────────────────────────────────────────────────────────────────────────

int RoadNetwork::addGuideSign(GuideSignPlacement sign)
{
	sign.id = m_nextGuideSignId++;
	int slot;
	if (!m_freeGuideSignSlots.isEmpty())
	{
		slot = m_freeGuideSignSlots.back();
		m_freeGuideSignSlots.pop_back();
		m_guideSigns[slot] = std::move(sign);
	}
	else
	{
		slot = static_cast<int>(m_guideSigns.size());
		m_guideSigns << std::move(sign);
	}
	m_guideSignIdToIdx[m_guideSigns[slot].id] = slot;
	return m_guideSigns[slot].id;
}

void RoadNetwork::addGuideSignRaw(const GuideSignPlacement& sign)
{
	if (sign.id < 0) return;
	if (sign.id >= m_nextGuideSignId) m_nextGuideSignId = sign.id + 1;
	int slot;
	if (!m_freeGuideSignSlots.isEmpty())
	{
		slot = m_freeGuideSignSlots.back();
		m_freeGuideSignSlots.pop_back();
		m_guideSigns[slot] = sign;
	}
	else
	{
		slot = static_cast<int>(m_guideSigns.size());
		m_guideSigns << sign;
	}
	m_guideSignIdToIdx[sign.id] = slot;
}

void RoadNetwork::removeGuideSign(int signId)
{
	const int idx = guideSignIndex(signId);
	if (idx < 0) return;
	m_guideSigns[idx].id = -1;
	m_guideSignIdToIdx.erase(signId);
	m_freeGuideSignSlots << idx;
}

void RoadNetwork::removeGuideSignsByEdge(int edgeId)
{
	Array<int> ids;
	for (const auto& g : m_guideSigns)
		if (g.id >= 0 && g.parentEdgeId == edgeId) ids << g.id;
	for (int id : ids) removeGuideSign(id);
}

GuideSignPlacement* RoadNetwork::getGuideSign(int id)
{
	const int idx = guideSignIndex(id);
	return (idx >= 0) ? &m_guideSigns[idx] : nullptr;
}

const GuideSignPlacement* RoadNetwork::getGuideSign(int id) const
{
	const int idx = guideSignIndex(id);
	return (idx >= 0) ? &m_guideSigns[idx] : nullptr;
}

void RoadNetwork::clearGuideSigns()
{
	m_guideSigns.clear();
	m_guideSignIdToIdx.clear();
	m_freeGuideSignSlots.clear();
	m_nextGuideSignId = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// NamedDestination
// ─────────────────────────────────────────────────────────────────────────────

void RoadNetwork::addNamedDestination(int nodeId, const String& name, const String& reading, uint8 tier)
{
	if (nodeId < 0 || name.isEmpty()) return;
	// 既登録なら tier を昇格（数値が小さい = 優先度高い）
	if (auto it = m_destByNode.find(nodeId); it != m_destByNode.end())
	{
		NamedDestination& d = m_namedDestinations[it->second];
		d.name    = name;
		d.reading = reading;
		if (tier < d.tier) d.tier = tier;
		return;
	}
	m_destByNode[nodeId] = static_cast<int>(m_namedDestinations.size());
	m_namedDestinations << NamedDestination{ nodeId, name, reading, tier };
}

const String* RoadNetwork::getDestinationReading(int nodeId) const
{
	const auto it = m_destByNode.find(nodeId);
	if (it == m_destByNode.end()) return nullptr;
	const String& r = m_namedDestinations[it->second].reading;
	return r.isEmpty() ? nullptr : &r;
}

void RoadNetwork::clearNamedDestinations()
{
	m_namedDestinations.clear();
	m_destByNode.clear();
}

const String* RoadNetwork::getDestinationName(int nodeId) const
{
	const auto it = m_destByNode.find(nodeId);
	if (it == m_destByNode.end()) return nullptr;
	return &m_namedDestinations[it->second].name;
}

uint8 RoadNetwork::getDestinationTier(int nodeId) const
{
	const auto it = m_destByNode.find(nodeId);
	if (it == m_destByNode.end()) return 255;
	return m_namedDestinations[it->second].tier;
}

// ─────────────────────────────────────────────────────────────────────────────
// RoadRoute API
// ─────────────────────────────────────────────────────────────────────────────

ColorF RoadNetwork::defaultRouteColor(RoadRouteKind kind)
{
	switch (kind)
	{
	case RoadRouteKind::Expressway:      return ColorF{ 0.0, 0.6, 0.0 };
	case RoadRouteKind::NationalRoute:   return ColorF{ 0.8, 0.1, 0.1 };
	case RoadRouteKind::PrefectureRoute: return ColorF{ 0.1, 0.35, 0.75 };
	case RoadRouteKind::CityRoute:       return ColorF{ 0.95, 0.7, 0.2 };
	case RoadRouteKind::Named:           return ColorF{ 0.55 };
	}
	return ColorF{ 0.55 };
}

String RoadNetwork::generateAutoRouteName(RoadRouteKind kind, int* outNumber) const
{
	// 番号系 kind: kind 内でユニークな 1〜400 の番号
	const bool isNumbered =
		(kind == RoadRouteKind::NationalRoute) ||
		(kind == RoadRouteKind::PrefectureRoute) ||
		(kind == RoadRouteKind::CityRoute);

	int number = 0;
	if (isNumbered)
	{
		HashSet<int> used;
		for (const auto& r : m_routes)
			if (r.id >= 0 && r.kind == kind && r.number > 0)
				used.insert(r.number);

		// 1〜400 からランダムに 50 回試行
		for (int i = 0; i < 50; ++i)
		{
			const int n = Random(1, 400);
			if (!used.contains(n)) { number = n; break; }
		}
		// フォールバック: 線形走査
		if (number == 0)
		{
			for (int n = 1; n <= 400; ++n)
			{
				if (!used.contains(n)) { number = n; break; }
			}
		}
	}

	if (outNumber) *outNumber = number;

	switch (kind)
	{
	case RoadRouteKind::NationalRoute:   return U"国道{}号"_fmt(number);
	case RoadRouteKind::PrefectureRoute: return U"県道{}号"_fmt(number);
	case RoadRouteKind::CityRoute:       return U"市道{}号"_fmt(number);
	case RoadRouteKind::Expressway:      return U"○○自動車道";  // v2 で起点-終点合成
	case RoadRouteKind::Named:           return U"通り";         // v2 で地名連動
	}
	return U"名称未設定";
}

int RoadNetwork::addRoute(RoadRouteKind kind, String name, Array<int> edgeIds, int number)
{
	// 路線本体の登録と、各エッジから見た routeIds 逆引きの更新を同時に行う。
	// 空名なら自動命名
	if (name.isEmpty())
	{
		int autoN = 0;
		name = generateAutoRouteName(kind, &autoN);
		if (number == 0) number = autoN;
	}

	RoadRoute route;
	route.id      = m_nextRouteId++;
	route.kind    = kind;
	route.name    = std::move(name);
	route.number  = number;
	route.edgeIds = std::move(edgeIds);
	route.color   = defaultRouteColor(kind);

	// スロット割当
	int idx;
	if (!m_freeRouteSlots.isEmpty())
	{
		idx = m_freeRouteSlots.back();
		m_freeRouteSlots.pop_back();
		m_routes[idx] = route;
	}
	else
	{
		idx = static_cast<int>(m_routes.size());
		m_routes << route;
	}
	m_routeIdToIdx[route.id] = idx;

	// 各 edge に逆引き登録
	for (const int eid : m_routes[idx].edgeIds)
	{
		if (RoadEdge* e = getEdge(eid))
		{
			if (!e->routeIds.contains(route.id))
				e->routeIds << route.id;
		}
	}

	return route.id;
}

void RoadNetwork::addRouteRaw(const RoadRoute& route)
{
	if (route.id < 0 || m_routeIdToIdx.contains(route.id)) return;

	int idx;
	if (!m_freeRouteSlots.isEmpty())
	{
		idx = m_freeRouteSlots.back();
		m_freeRouteSlots.pop_back();
		m_routes[idx] = route;
	}
	else
	{
		idx = static_cast<int>(m_routes.size());
		m_routes << route;
	}
	m_routeIdToIdx[route.id] = idx;
	m_nextRouteId = Max(m_nextRouteId, route.id + 1);
}

void RoadNetwork::removeRoute(int routeId)
{
	const int idx = routeIndex(routeId);
	if (idx < 0) return;
	// 各 edge の routeIds からも除去
	for (const int eid : m_routes[idx].edgeIds)
	{
		if (RoadEdge* e = getEdge(eid))
		{
			e->routeIds.remove(routeId);
		}
	}
	m_routes[idx].id = -1;
	m_routes[idx].edgeIds.clear();
	m_routeIdToIdx.erase(routeId);
	m_freeRouteSlots << idx;

	for (auto& plan : m_plans)
	{
		if (plan.id >= 0 && plan.routeId == routeId)
			plan.routeId = -1;
	}
}

RoadRoute* RoadNetwork::getRoute(int id)
{
	const int idx = routeIndex(id);
	return (idx >= 0) ? &m_routes[idx] : nullptr;
}

const RoadRoute* RoadNetwork::getRoute(int id) const
{
	const int idx = routeIndex(id);
	return (idx >= 0) ? &m_routes[idx] : nullptr;
}

void RoadNetwork::rebuildEdgeRouteIndex()
{
	// セーブ復元や一括編集後に、路線→エッジ情報からエッジ側の逆引きを再構築する。
	// 全 edge の routeIds をクリア
	for (auto& e : m_edges)
	{
		if (e.id >= 0) e.routeIds.clear();
	}
	// 各 route の edgeIds から逆引きを構築
	for (const auto& r : m_routes)
	{
		if (r.id < 0) continue;
		for (const int eid : r.edgeIds)
		{
			if (RoadEdge* e = getEdge(eid))
			{
				if (!e->routeIds.contains(r.id))
					e->routeIds << r.id;
			}
		}
	}
}

Array<std::pair<int, float>> RoadNetwork::routeSignAnchors(
	const RoadRoute& route,
	float distFromJunction_m,
	float minEdgeLen_m) const
{
	Array<std::pair<int, float>> result;

	for (const int edgeId : route.edgeIds)
	{
		const RoadEdge* edge = getEdge(edgeId);
		if (!edge || edge->id < 0) continue;

		// Planned / UnderConstruction は対象外
		if (edge->edgeState == EdgeState::Planned ||
		    edge->edgeState == EdgeState::UnderConstruction)
			continue;

		const auto bezier = getBezier(edgeId);
		if (!bezier) continue;

		const float totalLen = bezier->totalLength;
		if (totalLen < minEdgeLen_m) continue;

		// 信号のある交差点の先 50m のみに配置する
		// nodeA 側: nodeA に信号があるとき
		{
			const RoadNode* nodeA = getNode(edge->nodeA);
			if (nodeA && nodeA->signalPlacement.has_value())
			{
				const float arc = Min(distFromJunction_m, totalLen * 0.5f);
				result.emplace_back(edgeId, arc);
			}
		}

		// nodeB 側: nodeB に信号があるとき
		{
			const RoadNode* nodeB = getNode(edge->nodeB);
			if (nodeB && nodeB->signalPlacement.has_value())
			{
				const float arc = totalLen - Min(distFromJunction_m, totalLen * 0.5f);
				result.emplace_back(edgeId, arc);
			}
		}
	}

	return result;
}

void RoadNetwork::onEdgeRemovedFromRoutes(int edgeId)
{
	// 路線の途中エッジが消えたときは、末端短縮か中間分割で経路の連続性を保ち直す。
	// edge が所属する route の順序を保持したまま処理するため、
	// m_routes のインデックスを走査（新 route 追加時に m_routes が拡張される点に注意）
	// ここでは id ベースの snapshot を取ってから処理する
	Array<int> affectedRouteIds;
	for (const auto& r : m_routes)
	{
		if (r.id >= 0 && r.edgeIds.contains(edgeId))
			affectedRouteIds << r.id;
	}

	for (const int rid : affectedRouteIds)
	{
		RoadRoute* r = getRoute(rid);
		if (!r) continue;

		// 該当 edgeId の位置を探す（複数出現は本来禁止だが、安全に全削除処理）
		// 先に最初の出現のみを扱う
		auto it = std::find(r->edgeIds.begin(), r->edgeIds.end(), edgeId);
		if (it == r->edgeIds.end()) continue;
		const int pos = static_cast<int>(it - r->edgeIds.begin());
		const int lastIdx = static_cast<int>(r->edgeIds.size()) - 1;

		if (pos == 0 || pos == lastIdx)
		{
			// 端: 短縮のみ
			r->edgeIds.remove_at(pos);
			if (r->edgeIds.isEmpty())
			{
				removeRoute(rid);  // 空になったら削除
			}
		}
		else
		{
			// 中間: 2 つに分割
			// 元 route は前半 [0..pos-1] に短縮
			Array<int> backHalf(r->edgeIds.begin() + pos + 1, r->edgeIds.end());
			r->edgeIds.resize(pos);

			// 新 route を作成（同じ kind/name/number/color、新 id）
			if (!backHalf.isEmpty())
			{
				const int newId = m_nextRouteId++;
				RoadRoute newR;
				newR.id      = newId;
				newR.kind    = r->kind;
				newR.name    = r->name;
				newR.number  = r->number;
				newR.edgeIds = backHalf;
				newR.color   = r->color;

				int newIdx;
				if (!m_freeRouteSlots.isEmpty())
				{
					newIdx = m_freeRouteSlots.back();
					m_freeRouteSlots.pop_back();
					m_routes[newIdx] = newR;
				}
				else
				{
					newIdx = static_cast<int>(m_routes.size());
					m_routes << newR;
				}
				m_routeIdToIdx[newId] = newIdx;

				// 後半 edge 群の routeIds を更新（元 id を除去 + 新 id を追加）
				// 注: 元 id は edgeId の除去処理の一部として後半 edge の routeIds からも消す必要あり。
				for (const int eid : backHalf)
				{
					if (RoadEdge* e = getEdge(eid))
					{
						e->routeIds.remove(rid);
						if (!e->routeIds.contains(newId))
							e->routeIds << newId;
					}
				}
			}
		}
	}
}

int RoadNetwork::addPlan(RoadPlan plan)
{
	plan.id = m_nextPlanId++;
	if (plan.routeId >= 0)
	{
		if (const RoadRoute* route = getRoute(plan.routeId))
		{
			plan.routeName = route->name;
		}
	}

	int idx;
	if (!m_freePlanSlots.isEmpty())
	{
		idx = m_freePlanSlots.back();
		m_freePlanSlots.pop_back();
		m_plans[idx] = std::move(plan);
	}
	else
	{
		idx = static_cast<int>(m_plans.size());
		m_plans << std::move(plan);
	}
	m_planIdToIdx[m_plans[idx].id] = idx;
	rebuildPlanStats(m_plans[idx].id);
	rebuildPlanEdgeLinks();
	return m_plans[idx].id;
}

void RoadNetwork::addPlanRaw(const RoadPlan& plan)
{
	if (plan.id < 0 || m_planIdToIdx.contains(plan.id)) return;

	int idx;
	if (!m_freePlanSlots.isEmpty())
	{
		idx = m_freePlanSlots.back();
		m_freePlanSlots.pop_back();
		m_plans[idx] = plan;
	}
	else
	{
		idx = static_cast<int>(m_plans.size());
		m_plans << plan;
	}
	m_planIdToIdx[plan.id] = idx;
	m_nextPlanId = Max(m_nextPlanId, plan.id + 1);
}

void RoadNetwork::removePlan(int planId)
{
	const int idx = planIndex(planId);
	if (idx < 0) return;
	for (const int eid : m_plans[idx].edgeIds)
	{
		if (RoadEdge* edge = getEdge(eid))
		{
			if (edge->planId == planId)
				edge->planId = -1;
		}
	}
	m_plans[idx].id = -1;
	m_plans[idx].edgeIds.clear();
	m_plans[idx].viaPoints.clear();
	m_planIdToIdx.erase(planId);
	m_freePlanSlots << idx;
}

RoadPlan* RoadNetwork::getPlan(int id)
{
	const int idx = planIndex(id);
	return (idx >= 0) ? &m_plans[idx] : nullptr;
}

const RoadPlan* RoadNetwork::getPlan(int id) const
{
	const int idx = planIndex(id);
	return (idx >= 0) ? &m_plans[idx] : nullptr;
}

void RoadNetwork::rebuildPlanEdgeLinks()
{
	for (auto& edge : m_edges)
	{
		if (edge.id >= 0 && edge.planId >= 0 && !getPlan(edge.planId))
			edge.planId = -1;
	}

	for (auto& plan : m_plans)
	{
		if (plan.id < 0) continue;
		Array<int> valid;
		for (const int eid : plan.edgeIds)
		{
			if (RoadEdge* edge = getEdge(eid))
			{
				edge->planId = plan.id;
				valid << eid;
			}
		}
		plan.edgeIds = std::move(valid);
	}
}

void RoadNetwork::rebuildPlanStats(int planId)
{
	RoadPlan* plan = getPlan(planId);
	if (!plan) return;

	double totalMeters = 0.0;
	for (const int eid : plan->edgeIds)
	{
		if (const RoadEdge* edge = getEdge(eid))
			totalMeters += edge->length;
	}
	plan->totalLength = static_cast<float>(totalMeters);
	plan->totalCost = static_cast<float>(estimatePlanCost(plan->roadType, totalMeters));
	plan->constructionDuration = estimatePlanConstructionDuration(plan->roadType, totalMeters);
	if (plan->routeId >= 0)
	{
		if (const RoadRoute* route = getRoute(plan->routeId))
			plan->routeName = route->name;
	}
}

void RoadNetwork::rebuildAllPlanStats()
{
	for (const auto& plan : m_plans)
	{
		if (plan.id >= 0)
			rebuildPlanStats(plan.id);
	}
}

bool RoadNetwork::startPlanConstruction(int planId, GameTime startTime)
{
	RoadPlan* plan = getPlan(planId);
	if (!plan || plan->edgeIds.isEmpty()) return false;

	bool changed = false;
	for (const int eid : plan->edgeIds)
	{
		RoadEdge* edge = getEdge(eid);
		if (!edge) continue;
		edge->planId = planId;
		edge->edgeState = EdgeState::UnderConstruction;
		edge->constructionStartTime = startTime;
		changed = true;
	}
	if (!changed) return false;

	plan->state = PlanState::UnderConstruction;
	plan->constructionStart = startTime;
	plan->completionDate = startTime + plan->constructionDuration;
	return true;
}

bool RoadNetwork::completePlanConstruction(int planId)
{
	RoadPlan* plan = getPlan(planId);
	if (!plan) return false;

	bool changed = false;
	for (const int eid : plan->edgeIds)
	{
		if (RoadEdge* edge = getEdge(eid))
		{
			edge->edgeState = EdgeState::Open;
			changed = true;
		}
	}
	if (!changed) return false;

	plan->state = PlanState::Complete;
	if (plan->constructionStart)
		plan->completionDate = *plan->constructionStart + plan->constructionDuration;
	return true;
}

double RoadNetwork::estimatePlanConstructionDuration(RoadType roadType, double totalLengthMeters) const
{
	const double lengthKm = totalLengthMeters / 1000.0;
	double secondsPerKm = 90.0;
	switch (roadType)
	{
	case RoadType::LocalRoad:  secondsPerKm = 90.0; break;
	case RoadType::Arterial:   secondsPerKm = 120.0; break;
	case RoadType::Expressway: secondsPerKm = 180.0; break;
	case RoadType::Highway:    secondsPerKm = 210.0; break;
	}
	return Max(30.0, lengthKm * secondsPerKm);
}

double RoadNetwork::estimatePlanCost(RoadType roadType, double totalLengthMeters) const
{
	double unitCost = 25000.0;
	switch (roadType)
	{
	case RoadType::LocalRoad:  unitCost = 25000.0; break;
	case RoadType::Arterial:   unitCost = 45000.0; break;
	case RoadType::Expressway: unitCost = 80000.0; break;
	case RoadType::Highway:    unitCost = 95000.0; break;
	}
	return totalLengthMeters * unitCost;
}

void RoadNetwork::onEdgeRemovedFromPlans(int edgeId)
{
	Array<int> emptyPlans;
	for (auto& plan : m_plans)
	{
		if (plan.id < 0) continue;
		const bool removed = plan.edgeIds.contains(edgeId);
		if (removed)
			plan.edgeIds.remove(edgeId);
		if (!removed) continue;
		rebuildPlanStats(plan.id);
		if (plan.edgeIds.isEmpty())
			emptyPlans << plan.id;
	}

	for (const int planId : emptyPlans)
		removePlan(planId);
}

// =============================================================================
// エッジ方向・直進ペア構築（rebuildLaneConnections / buildDefaultSignalPhases 共通）
// =============================================================================

HashTable<int, Vec2> RoadNetwork::buildEdgeDirs(int nodeId) const
{
	const RoadNode* node = getNode(nodeId);
	if (!node) return {};

	HashTable<int, Vec2> edgeDirs;
	for (const auto& att : node->attachments)
	{
		const auto bez = getBezier(att.edgeId);
		const RoadEdge* e = getEdge(att.edgeId);
		if (!bez || !e) continue;
		Vec3 tan;
		if (e->nodeA == nodeId)
			tan = -bez->tangent(0.0f);
		else
			tan = bez->tangent(1.0f);
		edgeDirs[att.edgeId] = Vec2{ tan.x, tan.z }.normalized();
	}
	return edgeDirs;
}

Array<Array<int>> RoadNetwork::buildStraightPairs(int nodeId,
                                                   const HashTable<int, Vec2>& edgeDirs) const
{
	const RoadNode* node = getNode(nodeId);
	if (!node) return {};

	constexpr float kOppositeAngleThreshold = static_cast<float>(Math::QuarterPi);
	HashSet<int> selected;
	Array<Array<int>> pairs;

	for (const auto& att : node->attachments)
	{
		if (selected.contains(att.edgeId)) continue;
		selected.insert(att.edgeId);

		const auto dirIt = edgeDirs.find(att.edgeId);
		if (dirIt == edgeDirs.end()) continue;
		const Vec2 opposite = -dirIt->second;

		int bestEdge = -1;
		float bestAngle = kOppositeAngleThreshold;
		for (const auto& other : node->attachments)
		{
			if (other.edgeId == att.edgeId || selected.contains(other.edgeId)) continue;
			const auto otherIt = edgeDirs.find(other.edgeId);
			if (otherIt == edgeDirs.end()) continue;
			const float angle = static_cast<float>(std::acos(std::clamp(
				opposite.dot(otherIt->second), -1.0, 1.0)));
			if (angle < bestAngle)
			{
				bestAngle = angle;
				bestEdge = other.edgeId;
			}
		}

		Array<int> pair;
		pair << att.edgeId;
		if (bestEdge >= 0)
		{
			pair << bestEdge;
			selected.insert(bestEdge);
		}
		pairs << std::move(pair);
	}
	return pairs;
}

void RoadNetwork::rebuildLaneConnections(int nodeId)
{
	RoadNode* node = getNode(nodeId);
	if (!node) return;

	// 旧接続の論理キー → ID マッピングを保持（信号フェーズの greenConnectionIds を維持するため）
	auto packKey = [](int fe, int fl, int te, int tl) -> int64 {
		return (static_cast<int64>(fe) << 48) | (static_cast<int64>(fl & 0xFFFF) << 32)
			 | (static_cast<int64>(te & 0xFFFF) << 16) | static_cast<int64>(tl & 0xFFFF);
	};
	HashTable<int64, int> oldKeyToId;
	for (const auto& conn : node->laneConnections)
		oldKeyToId[packKey(conn.fromEdgeId, conn.fromLaneIndex, conn.toEdgeId, conn.toLaneIndex)] = conn.id;

	node->laneConnections.clear();

	const auto allEdgeIds = node->edgeIds();
	if (allEdgeIds.size() < 2) return;

	// ── エッジ方向マップ・直進ペア構築 ──
	const auto edgeDirs = buildEdgeDirs(nodeId);
	const auto pairs    = buildStraightPairs(nodeId, edgeDirs);

	// ── 車線参照 ──
	struct LaneRef
	{
		int   edgeId;
		int   laneIndex;
		float driverOffset; // 運転者視点の左右（負=左、正=右）
	};

	// Entry 車線（交差点に進入）を収集、運転者視点で左→右ソート
	auto collectEntryLanes = [&](int edgeId) -> Array<LaneRef>
	{
		const RoadEdge* edge = getEdge(edgeId);
		if (!edge || !edge->isRoadbedBuilt()) return {};
		Array<LaneRef> result;
		for (int i = 0; i < static_cast<int>(edge->lanes.size()); ++i)
		{
			const Lane& lane = edge->lanes[i];
			if (lane.op != OpState::Open && lane.op != OpState::Provisional) continue;
			const bool exits =
				(lane.dir == LaneDir::Forward  && edge->nodeB == nodeId) ||
				(lane.dir == LaneDir::Backward && edge->nodeA == nodeId);
			if (!exits) continue;
			const bool isAtA = (edge->nodeA == nodeId);
			const float center = lane.centerAt(isAtA ? 0.0f : 1.0f);
			const float drvOff = (lane.dir == LaneDir::Forward) ? center : -center;
			result << LaneRef{ edgeId, i, drvOff };
		}
		result.sort_by([](const LaneRef& a, const LaneRef& b) { return a.driverOffset < b.driverOffset; });
		return result;
	};

	// Exit 車線（交差点から退出）を収集、オフセット昇順ソート
	auto collectExitLanes = [&](int edgeId) -> Array<LaneRef>
	{
		const RoadEdge* edge = getEdge(edgeId);
		if (!edge || !edge->isRoadbedBuilt()) return {};
		Array<LaneRef> result;
		for (int i = 0; i < static_cast<int>(edge->lanes.size()); ++i)
		{
			const Lane& lane = edge->lanes[i];
			if (lane.op != OpState::Open && lane.op != OpState::Provisional) continue;
			const bool enters =
				(lane.dir == LaneDir::Forward  && edge->nodeA == nodeId) ||
				(lane.dir == LaneDir::Backward && edge->nodeB == nodeId);
			if (!enters) continue;
			const bool isAtA = (edge->nodeA == nodeId);
			const float center = lane.centerAt(isAtA ? 0.0f : 1.0f);
			const float drvOff = (lane.dir == LaneDir::Forward) ? center : -center;
			result << LaneRef{ edgeId, i, drvOff };
		}
		result.sort_by([](const LaneRef& a, const LaneRef& b) { return a.driverOffset < b.driverOffset; });
		return result;
	};

	// LaneConnection を生成して追加
	auto addConn = [&](const LaneRef& from, const LaneRef& to)
	{
		const RoadEdge* fromEdge = getEdge(from.edgeId);
		const RoadEdge* toEdge   = getEdge(to.edgeId);
		if (!fromEdge || !toEdge) return;
		const auto fromBez = getBezier(from.edgeId);
		const auto toBez   = getBezier(to.edgeId);
		if (!fromBez || !toBez) return;

		const auto exitPt  = calcLaneEndpoint(*fromBez, *fromEdge, fromEdge->lanes[from.laneIndex], nodeId);
		const auto entryPt = calcLaneEndpoint(*toBez,   *toEdge,   toEdge->lanes[to.laneIndex],     nodeId);

		const double dist   = (exitPt.worldPos - entryPt.worldPos).length();
		const double handle = Max(dist * 0.33, 5.0);
		const Vec3 p1 = exitPt.worldPos  + (exitPt.tangent  * exitPt.dirSign).normalized()  * handle;
		const Vec3 p2 = entryPt.worldPos - (entryPt.tangent * entryPt.dirSign).normalized() * handle;

		LaneConnection conn;
		const int64 key = packKey(from.edgeId, from.laneIndex, to.edgeId, to.laneIndex);
		if (const auto it = oldKeyToId.find(key); it != oldKeyToId.end())
			conn.id = it->second;
		else
			conn.id = node->nextConnectionId++;
		conn.fromEdgeId    = from.edgeId;
		conn.fromLaneIndex = from.laneIndex;
		conn.toEdgeId      = to.edgeId;
		conn.toLaneIndex   = to.laneIndex;
		conn.path          = CubicBezier{ exitPt.worldPos, p1, p2, entryPt.worldPos };
		node->laneConnections << std::move(conn);
	};

	// N本のEntry車線をM本のExit車線に振り分けて接続を生成
	auto distribute = [&](const Array<LaneRef>& entries, const Array<LaneRef>& exits)
	{
		const int N = static_cast<int>(entries.size());
		const int M = static_cast<int>(exits.size());
		if (N == 0 || M == 0) return;

		if (M >= N)
		{
			// Exit が多い: 各 Entry に floor(M/N) 本、一番右に残り全て
			const int per = M / N;
			int ei = 0;
			for (int i = 0; i < N; ++i)
			{
				const int count = (i == N - 1) ? (M - ei) : per;
				for (int j = 0; j < count; ++j)
					addConn(entries[i], exits[ei + j]);
				ei += count;
			}
		}
		else
		{
			// Entry が多い: 各 Exit に floor(N/M) 本、一番右 Exit に残り全て
			const int per = N / M;
			int ni = 0;
			for (int j = 0; j < M; ++j)
			{
				const int count = (j == M - 1) ? (N - ni) : per;
				for (int i = 0; i < count; ++i)
					addConn(entries[ni + i], exits[j]);
				ni += count;
			}
		}
	};

	// ── 方向種別 ──
	enum class TurnDir { AllDirections, Left, StraightLeft, Straight, StraightRight, Right };

	// ── 各ペアのエッジについて方向別接続を生成 ──
	for (const auto& pair : pairs)
	{
		for (const int entryEdgeId : pair)
		{
			const auto entryDirIt = edgeDirs.find(entryEdgeId);
			if (entryDirIt == edgeDirs.end()) continue;

			// ペア相手
			int pairedEdgeId = -1;
			if (pair.size() == 2)
				pairedEdgeId = (pair[0] == entryEdgeId) ? pair[1] : pair[0];

			// Entry 車線を収集
			auto entryLanes = collectEntryLanes(entryEdgeId);
			if (entryLanes.isEmpty()) continue;
			const int A = static_cast<int>(entryLanes.size());

			// ペア相手の Exit 車線
			auto pairedExits = (pairedEdgeId >= 0) ? collectExitLanes(pairedEdgeId) : Array<LaneRef>{};
			const int B = static_cast<int>(pairedExits.size());

			// 左右エッジを判定し角度でソート（ペア方向に近い順）
			const Vec2 entryDir = entryDirIt->second;
			const Vec2 straightDir = (pairedEdgeId >= 0 && edgeDirs.count(pairedEdgeId))
				? edgeDirs.at(pairedEdgeId) : -entryDir;

			struct EdgeAngle { int edgeId; float angle; };
			Array<EdgeAngle> leftEdges, rightEdges;

			for (const auto& att : node->attachments)
			{
				if (att.edgeId == entryEdgeId || att.edgeId == pairedEdgeId) continue;
				const auto otherIt = edgeDirs.find(att.edgeId);
				if (otherIt == edgeDirs.end()) continue;

				const Vec2 otherDir = otherIt->second;
				// 外積: 正→右、負→左（運転者視点）
				const float cross = static_cast<float>(entryDir.x * otherDir.y - entryDir.y * otherDir.x);
				const float angleDot = static_cast<float>(std::clamp(
					straightDir.dot(otherDir), -1.0, 1.0));
				const float angle = static_cast<float>(std::acos(angleDot));

				if (cross < 0.0f)
					leftEdges << EdgeAngle{ att.edgeId, angle };
				else if (cross > 0.0f)
					rightEdges << EdgeAngle{ att.edgeId, angle };
			}

			leftEdges.sort_by([](const EdgeAngle& a, const EdgeAngle& b) { return a.angle < b.angle; });
			rightEdges.sort_by([](const EdgeAngle& a, const EdgeAngle& b) { return a.angle < b.angle; });

			// 左右 Exit 車線プールを構築（角度順に連結）
			Array<LaneRef> leftExitPool, rightExitPool;
			for (const auto& le : leftEdges)
				leftExitPool.append(collectExitLanes(le.edgeId));
			for (const auto& re : rightEdges)
				rightExitPool.append(collectExitLanes(re.edgeId));

			const int L = static_cast<int>(leftExitPool.size());
			const int R = static_cast<int>(rightExitPool.size());

			// ── Phase 1: 方向割り当て ──
			Array<TurnDir> laneDirs(A, TurnDir::Straight);

			if (A == 1)
			{
				laneDirs[0] = TurnDir::AllDirections;
			}
			else if (A <= B)
			{
				laneDirs[0]     = TurnDir::StraightLeft;
				laneDirs[A - 1] = TurnDir::StraightRight;
			}
			else // A > B
			{
				int numLeft  = Min((A - B) / 2, L);
				int numRight = Min((A - B + 1) / 2, R);

				for (int i = 0; i < numLeft; ++i)
					laneDirs[i] = TurnDir::Left;
				for (int i = 0; i < numRight; ++i)
					laneDirs[A - 1 - i] = TurnDir::Right;

				// 補正: L>0 かつ左折なしなら一番左を左折直進に
				if (L > 0 && numLeft == 0)
					laneDirs[0] = TurnDir::StraightLeft;

				// B=0: 直進先なし → 残りの Straight を Left/Right に再分配
				if (B == 0)
				{
					// 左側から Left に割り当て
					for (int i = 0; i < A; ++i)
					{
						if (laneDirs[i] != TurnDir::Straight) continue;
						int assignedLeft = 0;
						for (int j = 0; j < A; ++j)
							if (laneDirs[j] == TurnDir::Left || laneDirs[j] == TurnDir::StraightLeft) assignedLeft++;
						if (assignedLeft < L)
							laneDirs[i] = TurnDir::Left;
						else
							break;
					}
					// 右側から Right に割り当て
					for (int i = A - 1; i >= 0; --i)
					{
						if (laneDirs[i] != TurnDir::Straight) continue;
						int assignedRight = 0;
						for (int j = 0; j < A; ++j)
							if (laneDirs[j] == TurnDir::Right || laneDirs[j] == TurnDir::StraightRight) assignedRight++;
						if (assignedRight < R)
							laneDirs[i] = TurnDir::Right;
						else
							break;
					}
				}
			}

			// ── Phase 2: 方向別に接続を生成 ──
			Array<LaneRef> straightEntries, leftEntries, rightEntries;

			for (int i = 0; i < A; ++i)
			{
				switch (laneDirs[i])
				{
				case TurnDir::AllDirections:
					for (const auto& ex : pairedExits)    addConn(entryLanes[i], ex);
					for (const auto& ex : leftExitPool)   addConn(entryLanes[i], ex);
					for (const auto& ex : rightExitPool)  addConn(entryLanes[i], ex);
					break;
				case TurnDir::Straight:
					straightEntries << entryLanes[i];
					break;
				case TurnDir::StraightLeft:
					straightEntries << entryLanes[i];
					leftEntries     << entryLanes[i];
					break;
				case TurnDir::StraightRight:
					straightEntries << entryLanes[i];
					rightEntries    << entryLanes[i];
					break;
				case TurnDir::Left:
					leftEntries << entryLanes[i];
					break;
				case TurnDir::Right:
					rightEntries << entryLanes[i];
					break;
				}
			}

			distribute(straightEntries, pairedExits);
			distribute(leftEntries, leftExitPool);

			// 右折: 入力を右端から順にするため反転
			if (!rightEntries.isEmpty() && !rightExitPool.isEmpty())
			{
				rightEntries.reverse();
				distribute(rightEntries, rightExitPool);
			}
		}
	}

	// ── 自動信号設置 ──
	constexpr int kAutoSignalThreshold = 8;
	if (static_cast<int>(node->laneConnections.size()) > kAutoSignalThreshold)
	{
		if (!node->signalPlacement)
		{
			SignalPlacement sp;
			sp.signalDefId = U"signal_3lamp";
			sp.phases = buildDefaultSignalPhases(nodeId);
			node->signalPlacement = sp;
			for (auto& att : node->attachments)
				att.control = TrafficControl::Signal;
		}
	}
}

void RoadNetwork::updateLaneConnectionPaths(int nodeId)
{
	RoadNode* node = getNode(nodeId);
	if (!node) return;

	for (auto& conn : node->laneConnections)
	{
		const RoadEdge* fromEdge = getEdge(conn.fromEdgeId);
		const RoadEdge* toEdge   = getEdge(conn.toEdgeId);
		if (!fromEdge || !toEdge) continue;

		const auto fromBez = getBezier(conn.fromEdgeId);
		const auto toBez   = getBezier(conn.toEdgeId);
		if (!fromBez || !toBez) continue;

		if (conn.fromLaneIndex >= static_cast<int>(fromEdge->lanes.size())) continue;
		if (conn.toLaneIndex   >= static_cast<int>(toEdge->lanes.size()))   continue;

		const auto exitPt  = calcLaneEndpoint(*fromBez, *fromEdge, fromEdge->lanes[conn.fromLaneIndex], nodeId);
		const auto entryPt = calcLaneEndpoint(*toBez,   *toEdge,   toEdge->lanes[conn.toLaneIndex],     nodeId);

		const double dist   = (exitPt.worldPos - entryPt.worldPos).length();
		const double handle = Max(dist * 0.33, 5.0);
		const Vec3 p1 = exitPt.worldPos  + (exitPt.tangent  * exitPt.dirSign).normalized()  * handle;
		const Vec3 p2 = entryPt.worldPos - (entryPt.tangent * entryPt.dirSign).normalized() * handle;

		conn.path = CubicBezier{ exitPt.worldPos, p1, p2, entryPt.worldPos };
	}
}

Array<SignalPhaseDef> RoadNetwork::buildDefaultSignalPhases(int nodeId) const
{
	const RoadNode* node = getNode(nodeId);
	if (!node) return {};

	constexpr float kMinPhaseDuration = 5.0f;       // 実時間秒
	constexpr float kDurationPerConnection = 2.0f;  // 実時間秒

	// エッジ方向マップ・直進ペア構築
	const auto edgeDirs = buildEdgeDirs(nodeId);
	const auto pairs    = buildStraightPairs(nodeId, edgeDirs);

	// フェーズの生成
	Array<SignalPhaseDef> phases;
	for (const auto& pair : pairs)
	{
		SignalPhaseDef phase;
		for (const auto& conn : node->laneConnections)
		{
			for (const int eid : pair)
			{
				if (conn.fromEdgeId == eid)
				{
					phase.greenConnectionIds << conn.id;
					break;
				}
			}
		}

		if (phase.greenConnectionIds.isEmpty()) continue;

		phase.duration = Max(
			static_cast<float>(phase.greenConnectionIds.size()) * kDurationPerConnection,
			kMinPhaseDuration);
		phases << std::move(phase);
	}

	return phases;
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
