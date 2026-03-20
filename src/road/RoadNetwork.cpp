#include "RoadNetwork.hpp"

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
	smoothCurveAt(e.id, nodeA);
	smoothCurveAt(e.id, nodeB);

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

bool RoadNetwork::spreadIntersectionTangents()
{
	constexpr float kMinAngleDeg = 45.0;
	int adjustedNodes = 0;

	for (RoadNode& node : m_nodes)
	{
		if (node.id < 0 || node.edgeIds.size() < 3) continue;

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
		for (int eid : node.edgeIds)
		{
			const RoadEdge* e = getEdge(eid);
			if (!e) continue;

			const Vec3& ctrl = (e->nodeA == node.id) ? e->ctrlA : e->ctrlB;
			const float cdx = ctrl.x - nodePos.x;
			const float cdz = ctrl.z - nodePos.z;
			const float clen = std::sqrt(cdx * cdx + cdz * cdz);
			if (clen < 1e-6f) continue;

			EdgeEntry entry;
			entry.edgeId  = eid;
			entry.ctrlLen = clen;
			entry.ctrlY   = ctrl.y;
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

		// ---- 調整前の角度リストを出力 ----
		{
			String s = U"[spreadIntersectionTangents] node={} before:"_fmt(node.id);
			for (int i = 0; i < n; ++i)
				s += U" edge{}={:.1f}deg"_fmt(rolled[i].edgeId, angles[i]);
			Console << s;
		}

		// Step 5: n 個目（n>=2）の要素について、前との差が kMinAngleDeg 未満なら補正
		bool changed = false;
		for (int i = 1; i < n; ++i)
		{
			if (angles[i] - angles[i - 1] < kMinAngleDeg)
			{
				Console << U"  step5: edge{} {:.1f}->{:.1f} (prev={:.1f})"_fmt(
					rolled[i].edgeId, angles[i], angles[i - 1] + kMinAngleDeg, angles[i - 1]);
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
				Console << U"  step6: circular gap={:.1f}deg -> backward pass"_fmt(
					360.0f - (angles[n - 1] - angles[0]));

				changed = true;
				angles[n - 1] = maxLast;

				// 前の要素との差が不足する間、逆順に押し戻す
				bool needFallback = false;
				for (int i = n - 2; i >= 1; --i)
				{
					if (angles[i + 1] - angles[i] >= kMinAngleDeg)
						break; // 十分な間隔が確保できたので終了

					angles[i] = angles[i + 1] - kMinAngleDeg - 1.0f;
					Console << U"  step6: edge{} pushed back to {:.1f}"_fmt(
						rolled[i].edgeId, angles[i]);

					// angles[0]（最太道路）との間隔も不足するなら等分配置へ
					if (angles[i] < angles[0] + kMinAngleDeg)
					{
						needFallback = true;
						break;
					}
				}

				if (needFallback)
				{
					Console << U"  step6: fallback -> even distribution"_fmt();
					for (int i = 1; i < n; ++i)
						angles[i] = angles[0] + static_cast<float>(i) * 360.0f / n;
				}
			}
		}

		if (!changed)
		{
			Console << U"  -> no change"_fmt();
			continue;
		}

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

		// ---- 調整後の角度リストを出力 ----
		{
			String s = U"  after:"_fmt();
			for (int i = 0; i < n; ++i)
				s += U" edge{}={:.1f}deg"_fmt(rolled[i].edgeId, angles[i]);
			Console << s;
		}

		Console << U"[spreadIntersectionTangents] node={} ({} edges) adjusted"_fmt(node.id, n);
		++adjustedNodes;
	}

	Console << U"[spreadIntersectionTangents] done: {} node(s) adjusted"_fmt(adjustedNodes);
	return adjustedNodes > 0;
}

void RoadNetwork::smoothCurveAt(int newEdgeId, int midNodeId)
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

bool RoadNetwork::removeDuplicateEdges(uint64 seed)
{
	bool removed = false;
	Array<int> edgeIds;
	for (const RoadEdge& e : m_edges)
		if (e.id >= 0) edgeIds << e.id;

	for (int ii = 0; ii < static_cast<int>(edgeIds.size()); ++ii)
	{
		const RoadEdge* e1 = getEdge(edgeIds[ii]);
		if (!e1) continue;

		for (int jj = ii + 1; jj < static_cast<int>(edgeIds.size()); ++jj)
		{
			const RoadEdge* e2 = getEdge(edgeIds[jj]);
			if (!e2) continue;

			// 同一ノードペア（順方向・逆方向どちらも）を検出
			const bool dup =
				(e1->nodeA == e2->nodeA && e1->nodeB == e2->nodeB) ||
				(e1->nodeA == e2->nodeB && e1->nodeB == e2->nodeA);
			if (!dup) continue;

			// 道幅が小さい方を削除。同幅は seed + エッジ ID のハッシュで決定的に選択
			int toRemove;
			const float w1 = e1->totalWidth(), w2 = e2->totalWidth();
			if (w1 < w2)
				toRemove = edgeIds[ii];
			else if (w2 < w1)
				toRemove = edgeIds[jj];
			else
			{
				const uint64 lo = static_cast<uint64>(Min(edgeIds[ii], edgeIds[jj]));
				const uint64 hi = static_cast<uint64>(Max(edgeIds[ii], edgeIds[jj]));
				const uint64 h  = (lo * 2654435761ULL ^ hi * 2246822519ULL) ^ seed;
				toRemove = (h & 1) ? edgeIds[ii] : edgeIds[jj];
			}

			removeEdge(toRemove);
			removed = true;

			// e1 が削除されたなら内側ループを抜ける
			e1 = getEdge(edgeIds[ii]);
			if (!e1) break;
		}
	}
	return removed;
}

bool RoadNetwork::resolveIntersections()
{
	// Case 1: NodeA→NodeB の直線同士が交わる → 交点で両エッジを分割
	// Case 2: Case 1 が外れ、かつ端点↔制御点の直線が交わる
	//         → 4 端点の平均位置にノードを生成し、両エッジを t=0.5 で分割
	// ノードマージ: 生成ノードが既存ノードと 40 m 以内ならマージ

	constexpr float MERGE_DIST = 40.0f;
	constexpr float SKIP_EPS   = 0.02f;

	bool foundAny = true;
	while (foundAny)
	{
		foundAny = false;

		Array<int> edgeIds;
		for (const RoadEdge& e : m_edges)
			if (e.id >= 0) edgeIds << e.id;

		for (int ii = 0; ii < static_cast<int>(edgeIds.size()) && !foundAny; ++ii)
		for (int jj = ii + 1; jj < static_cast<int>(edgeIds.size()) && !foundAny; ++jj)
		{
			const RoadEdge* e1 = getEdge(edgeIds[ii]);
			const RoadEdge* e2 = getEdge(edgeIds[jj]);
			if (!e1 || !e2) continue;

			// 隣接エッジ（共有ノードあり）はスキップ
			if (e1->nodeA == e2->nodeA || e1->nodeA == e2->nodeB ||
			    e1->nodeB == e2->nodeA || e1->nodeB == e2->nodeB) continue;

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
				// Case 1: 端点直線が交わる → s,t をそのまま分割 t に使用
				bt1 = s; bt2 = t;
				if (bt1 < SKIP_EPS || bt1 > 1.0f - SKIP_EPS) continue;
				if (bt2 < SKIP_EPS || bt2 > 1.0f - SKIP_EPS) continue;
				intPos = posA1 + (posB1 - posA1) * bt1;
			}
			else
			{
				// Case 2: 端点↔制御点の直線（4 通り）が交わるか確認
				float ds, dt;
				const bool cpHit =
					segIntersect2D({ posA1.x, posA1.z }, { cA1.x, cA1.z },
					               { posA2.x, posA2.z }, { cA2.x, cA2.z }, ds, dt) ||
					segIntersect2D({ posA1.x, posA1.z }, { cA1.x, cA1.z },
					               { posB2.x, posB2.z }, { cB2.x, cB2.z }, ds, dt) ||
					segIntersect2D({ posB1.x, posB1.z }, { cB1.x, cB1.z },
					               { posA2.x, posA2.z }, { cA2.x, cA2.z }, ds, dt) ||
					segIntersect2D({ posB1.x, posB1.z }, { cB1.x, cB1.z },
					               { posB2.x, posB2.z }, { cB2.x, cB2.z }, ds, dt);
				if (!cpHit) continue;

				// 4 端点の平均を接続ノード位置とする（高さも平均）
				bt1 = bt2 = 0.5f;
				intPos = Vec3{
					(posA1.x + posB1.x + posA2.x + posB2.x) * 0.25f,
					(posA1.y + posB1.y + posA2.y + posB2.y) * 0.25f,
					(posA1.z + posB1.z + posA2.z + posB2.z) * 0.25f
				};
			}

			// 既存ノードへのマージ判定（分割対象エッジの端点は除外）
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

			// 新規ノードを生成（m_nodes が realloc される可能性あり）
			if (splitNodeId < 0)
				splitNodeId = addNode(intPos, NodeType::Intersection);

			// 分割ノードの位置（addNode 後でも ID 検索で有効）
			const Vec3 splitPos = getNode(splitNodeId)->position;

			// E1 を分割（制御点は 1/3・2/3 線形補間、smoothJunction が後で整える）
			removeEdge(edgeIds[ii]);
			addEdge(nA1, splitNodeId,
			        posA1 + (splitPos - posA1) * (1.0 / 3.0),
			        posA1 + (splitPos - posA1) * (2.0 / 3.0), rt1, lanes1);
			addEdge(splitNodeId, nB1,
			        splitPos + (posB1 - splitPos) * (1.0 / 3.0),
			        splitPos + (posB1 - splitPos) * (2.0 / 3.0), rt1, lanes1);

			// E2 を分割
			removeEdge(edgeIds[jj]);
			addEdge(nA2, splitNodeId,
			        posA2 + (splitPos - posA2) * (1.0 / 3.0),
			        posA2 + (splitPos - posA2) * (2.0 / 3.0), rt2, lanes2);
			addEdge(splitNodeId, nB2,
			        splitPos + (posB2 - splitPos) * (1.0 / 3.0),
			        splitPos + (posB2 - splitPos) * (2.0 / 3.0), rt2, lanes2);

			foundAny = true;
		}
	}
	return foundAny;
}

bool RoadNetwork::fixSharpAngles(float minAngleDeg)
{
	const float cosThresh = static_cast<float>(Math::Cos(Math::ToRadians(minAngleDeg)));
	bool anyFixed = false;

	// イテレーション中に m_nodes の要素変更が起きるため、先に ID を収集する
	Array<int> nodeIds;
	for (const RoadNode& n : m_nodes)
		if (n.id >= 0) nodeIds << n.id;

	for (const int nodeId : nodeIds)
	{
		const RoadNode* node = getNode(nodeId);
		if (!node || node->edgeIds.size() < 2) continue;

		// edgeIds をコピーしておく（removeEdge / addEdge で変化するため）
		const Array<int> edgesCopy = node->edgeIds;
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
	return anyFixed;
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
