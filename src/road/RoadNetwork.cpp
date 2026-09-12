#include "RoadNetwork.hpp"
#include "RoadSign.hpp"
#include "RoadGeometry.hpp"
#include "../debug/DebugLog.hpp"
#include "GuideSign.hpp"
#include "../world/World.hpp"

namespace
{
	constexpr double kConstructionUnitsPerDay = GameClock::kSecondsPerGameDay;
	constexpr double kConstructionUnitsPerMonth = GameClock::kSecondsPerGameMonth;

	/// @brief 道路種別ごとの概算建設費 [億円/km]
	double constructionCostPerKm(RoadType roadType)
	{
		switch (roadType)
		{
		case RoadType::LocalRoad:  return 0.5;   // 生活道路
		case RoadType::Arterial:   return 1.5;   // 県道級の幹線
		case RoadType::Expressway: return 8.0;   // バイパス・自動車専用道級
		case RoadType::Highway:    return 20.0;  // 高速道路級
		}
		return 0.5;
	}

	/// @brief 道路種別ごとの建設期間 [ゲーム月/km]
	double constructionMonthsPerKm(RoadType roadType)
	{
		switch (roadType)
		{
		case RoadType::LocalRoad:  return 1.0;
		case RoadType::Arterial:   return 2.0;
		case RoadType::Expressway: return 6.0;
		case RoadType::Highway:    return 12.0;
		}
		return 1.0;
	}

	/// @brief XZ 平面での 2D 線分交差判定
	/// @param a1,a2  線分1の端点 (x = world-X, y = world-Z)
	/// @param b1,b2  線分2の端点
	/// @param s      出力: 線分1上の交差パラメータ [0,1]
	/// @param t      出力: 線分2上の交差パラメータ [0,1]
	/// @return Sample chords intersect, including shared sample endpoints.
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
		return s >= -EPS && s <= 1.0f + EPS && t >= -EPS && t <= 1.0f + EPS;
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
	double bestDistance = static_cast<double>(maxDist) * maxDist;
	for (const auto& edge : m_edges)
	{
		if (edge.id < 0) { continue; }
		const auto* a = getNode(edge.nodeA);
		const auto* b = getNode(edge.nodeB);
		if (!a || !b) { continue; }
		double minX=a->position.x, maxX=minX, minZ=a->position.z, maxZ=minZ;
		for (const Vec3 p : {b->position,edge.ctrlA,edge.ctrlB})
		{
			minX=Min(minX,p.x); maxX=Max(maxX,p.x);
			minZ=Min(minZ,p.z); maxZ=Max(maxZ,p.z);
		}
		if (pos.x < minX-maxDist || pos.x > maxX+maxDist || pos.z < minZ-maxDist || pos.z > maxZ+maxDist) { continue; }
		const auto curve = getBezier(edge.id);
		if (!curve) { continue; }
		const auto distanceAt = [&](float arc)
		{
			const Vec3 p = curve->positionAt(arc);
			return Vec2{p.x-pos.x,p.z-pos.z}.lengthSq();
		};
		constexpr int kSamples = 32;
		int bestIndex = 0;
		double nearest = distanceAt(0);
		for (int i=1;i<=kSamples;++i)
		{
			const double distance = distanceAt(curve->totalLength*i/kSamples);
			if (distance < nearest) { nearest=distance; bestIndex=i; }
		}
		float low=curve->totalLength*Max(bestIndex-1,0)/kSamples;
		float high=curve->totalLength*Min(bestIndex+1,kSamples)/kSamples;
		for (int i=0;i<18;++i)
		{
			const float first=low+(high-low)/3, second=high-(high-low)/3;
			if (distanceAt(first) < distanceAt(second)) { high=second; } else { low=first; }
		}
		const float arc=(low+high)*0.5f;
		if (const double distance=distanceAt(arc); distance < bestDistance)
		{
			bestDistance=distance;
			best=std::pair<int,float>{edge.id,arc};
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
	const auto bezier = getBezier(edgeId);
	return bezier ? splitEdgeAtParameter(edgeId,bezier->tFromArcLength(arcLength)) : -1;
}

int RoadNetwork::splitEdgeAtParameter(int edgeId, float t, int existingNodeId)
{
	const RoadEdge* edge = getEdge(edgeId);
	const auto curve = getBezier(edgeId);
	if (!edge || !curve || t <= 0.00001f || t >= 0.99999f) { return -1; }
	const CubicBezier bez = *curve;
	int reuseA=-1,reuseB=-1;
	if (existingNodeId>=0)
	{
		const auto* existing=getNode(existingNodeId);
		if (!existing || existingNodeId==edge->nodeA || existingNodeId==edge->nodeB) return -1;
		for (const int id : existing->edgeIds())
		{
			const auto* candidate=getEdge(id);if (!candidate) continue;
			if (candidate->nodeA==edge->nodeA || candidate->nodeB==edge->nodeA) reuseA=id;
			if (candidate->nodeA==edge->nodeB || candidate->nodeB==edge->nodeB) reuseB=id;
		}
		if (existing->attachments.size()+(reuseA<0?1:0)+(reuseB<0?1:0)>6) return -1;
	}

	const auto [bezA, bezB] = bez.split(t);
	const Vec3 splitPos = bez.evaluate(t);

	// 元エッジの属性を保存（removeEdge で無効化される前にコピー）
	const int origNodeA  = edge->nodeA;
	const int origNodeB  = edge->nodeB;
	const RoadType rt    = edge->roadType;
	const int numLanes   = static_cast<int>(edge->lanes.size());
	const RoadEdge tmpl  = *edge;  // テンプレートとして属性を丸ごとコピー

	// 接続点を挿入する操作は道路撤去ではない。撤去コールバックに路線・計画を分断させない。
	struct Membership { int id; Array<int> edgeIds; bool reverse = false; };
	Array<Membership> routeMemberships, planMemberships;
	for (auto& route : m_routes)
	{
		if (route.id < 0 || !route.edgeIds.contains(edgeId)) { continue; }
		bool reverse = false;
		const auto position = std::find(route.edgeIds.begin(),route.edgeIds.end(),edgeId);
		if (position != route.edgeIds.begin())
		{
			const auto* previous = getEdge(*(position-1));
			reverse = previous && (previous->nodeA == origNodeB || previous->nodeB == origNodeB);
		}
		else if (position+1 != route.edgeIds.end())
		{
			const auto* next = getEdge(*(position+1));
			reverse = next && (next->nodeA == origNodeA || next->nodeB == origNodeA);
		}
		routeMemberships << Membership{route.id,route.edgeIds,reverse};
		route.edgeIds.remove(edgeId);
	}
	for (auto& plan : m_plans)
	{
		if (plan.id < 0 || !plan.edgeIds.contains(edgeId)) { continue; }
		planMemberships << Membership{plan.id,plan.edgeIds};
		plan.edgeIds.remove(edgeId);
	}

	// Inserting a node must not demolish attached piers or manually placed signs.
	Array<int> retainedObjects,retainedGuides;
	for (auto& object : m_objects) if (object.id>=0 && object.parentEdgeId==edgeId)
	{
		retainedObjects << object.id;object.parentEdgeId=-1;
	}
	for (auto& sign : m_guideSigns) if (sign.id>=0 && sign.parentEdgeId==edgeId)
	{
		retainedGuides << sign.id;sign.parentEdgeId=-1;
	}
	removeEdge(edgeId);

	const int midNodeId = existingNodeId >= 0 ? existingNodeId : addNode(splitPos, NodeType::Joint);
	const Vec3 shift = getNode(midNodeId)->position - splitPos;

	int newEidA = -1, newEidB = -1;
	if (auto eidA = reuseA>=0 ? Optional<int>{reuseA} : addEdge(origNodeA, midNodeId, bezA.p1, bezA.p2 + shift, rt, numLanes))
	{
		if (reuseA<0 || getEdge(*eidA)->totalWidth()<tmpl.totalWidth()) applyEdgeTemplate(*eidA, tmpl);
		if (RoadEdge* ea = getEdge(*eidA))
		{
			ea->edgeState             = tmpl.edgeState;
			ea->constructionStartTime = tmpl.constructionStartTime;
			ea->useElevation          = tmpl.useElevation;
		}
		newEidA = *eidA;
	}

	if (auto eidB = reuseB>=0 ? Optional<int>{reuseB} : addEdge(midNodeId, origNodeB, bezB.p1 + shift, bezB.p2, rt, numLanes))
	{
		if (reuseB<0 || getEdge(*eidB)->totalWidth()<tmpl.totalWidth()) applyEdgeTemplate(*eidB, tmpl);
		if (RoadEdge* eb = getEdge(*eidB))
		{
			eb->edgeState             = tmpl.edgeState;
			eb->constructionStartTime = tmpl.constructionStartTime;
			eb->useElevation          = tmpl.useElevation;
		}
		newEidB = *eidB;
	}

	const float scaled=t*CubicBezier::SAMPLES;
	const int sample=Min(static_cast<int>(scaled),CubicBezier::SAMPLES-1);
	const float splitArc=Math::Lerp(bez.arcTable[sample],bez.arcTable[sample+1],scaled-sample);
	const float fraction=splitArc/Max(.001f,bez.totalLength);
	const auto splitOffsets=[&](auto& child,const auto& original,bool before)
	{
		const float left=Math::Lerp(original.offsetA_L,original.offsetB_L,fraction);
		const float right=Math::Lerp(original.offsetA_R,original.offsetB_R,fraction);
		if (before) { child.offsetB_L=left;child.offsetB_R=right; }
		else { child.offsetA_L=left;child.offsetA_R=right; }
	};
	for (const bool before : {true,false})
	{
		if ((before?reuseA:reuseB)>=0) continue;
		auto* child=getEdge(before?newEidA:newEidB);if (!child) continue;
		for (size_t i=0;i<child->parts.size();++i) splitOffsets(child->parts[i],tmpl.parts[i],before);
		for (size_t i=0;i<child->lanes.size();++i) splitOffsets(child->lanes[i],tmpl.lanes[i],before);
		rebuildNodeConnectivity(child->nodeA,child->nodeB);
	}

	for (const int id : retainedObjects)
	{
		auto* object=getObject(id);if (!object) continue;
		const bool before=object->arcPos<=splitArc;
		const auto* child=getEdge(before?newEidA:newEidB);if (!child) continue;
		object->parentEdgeId=child->id;
		object->arcPos-=before?0.0f:splitArc;
		if (child->nodeA!=(before?origNodeA:midNodeId))
		{
			object->arcPos=child->length-object->arcPos;
			object->lateralOffset=-object->lateralOffset;
			object->yawOffset+=static_cast<float>(Math::Pi);
		}
	}
	for (const int id : retainedGuides)
	{
		auto* sign=getGuideSign(id);if (!sign) continue;
		sign->parentEdgeId=sign->nodeEndId==origNodeB?newEidB:newEidA;
	}
	for (const auto& sign : tmpl.signs)
	{
		if (sign.autoGenerated) continue;
		if (auto* child=getEdge(sign.nodeEndId==origNodeB?newEidB:newEidA)) child->signs << sign;
	}
	for (auto& marking : m_manualMarkings)
	{
		if (marking.edgeId!=edgeId) continue;
		const bool before=marking.arcOffset<=splitArc;
		marking.edgeId=before?newEidA:newEidB;
		if (!before) marking.arcOffset-=splitArc;
	}

	const auto replaceMember = [&](const Membership& membership)
	{
		Array<int> result;
		for (const int id : membership.edgeIds)
		{
			if (id != edgeId) { result << id; }
			else if (membership.reverse) { result << newEidB << newEidA; }
			else { result << newEidA << newEidB; }
		}
		return result;
	};
	if (newEidA >= 0 && newEidB >= 0)
	{
		for (const auto& membership : routeMemberships)
		{
			if (auto* route = getRoute(membership.id))
			{
				route->edgeIds = replaceMember(membership);
				getEdge(newEidA)->routeIds << route->id;
				getEdge(newEidB)->routeIds << route->id;
			}
		}
		for (const auto& membership : planMemberships)
		{
			if (auto* plan = getPlan(membership.id))
			{
				plan->edgeIds = replaceMember(membership);
				getEdge(newEidA)->planId = plan->id;
				getEdge(newEidB)->planId = plan->id;
				rebuildPlanStats(plan->id);
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
			const auto* a = getEdge(node->attachments[0].edgeId);
			const auto* b = getEdge(node->attachments[1].edgeId);
			if (a && b)
			{
				Vec3 da = (a->nodeA==nodeId ? a->ctrlA : a->ctrlB)-node->position;
				Vec3 db = (b->nodeA==nodeId ? b->ctrlA : b->ctrlB)-node->position;
				da.y=0; db.y=0;
				if (da.lengthSq()>1e-8 && db.lengthSq()>1e-8)
				{
					const double cosine = Clamp(da.normalized().dot(db.normalized()),-1.0,0.98);
					constexpr double kInnerTurnRadius = 6.0;
					const double radius = Max(RoadGeometry::structuralWidth(*a),RoadGeometry::structuralWidth(*b))*0.5+kInnerTurnRadius;
					cutoff = Max(cutoff,static_cast<float>(radius*Sqrt((1.0+cosine)/(1.0-cosine))));
				}
			}
		}
		else
		{
			// Abrupt: 最小限のキャップ
			cutoff = 0.1f;
		}
		break;
	case NodeType::Intersection:
	case NodeType::Diverge:
		cutoff = Max(6.0f, maxWidth * 0.5f + 3.0f);
		break;
	}

	// At an acute fork, a width-only cutoff leaves two full road strips on top of
	// each other outside the junction. Start the strips where their envelopes separate.
	if (validCount>=3)
	{
		for (size_t i=0;i<node->attachments.size();++i) for (size_t j=i+1;j<node->attachments.size();++j)
		{
			const auto* a=getEdge(node->attachments[i].edgeId);
			const auto* b=getEdge(node->attachments[j].edgeId);
			if (!a || !b) continue;
			Vec3 da=(a->nodeA==nodeId?a->ctrlA:a->ctrlB)-node->position;
			Vec3 db=(b->nodeA==nodeId?b->ctrlA:b->ctrlB)-node->position;
			da.y=0;db.y=0;
			if (da.lengthSq()<1e-8 || db.lengthSq()<1e-8) continue;
			const double cosine=Clamp(da.normalized().dot(db.normalized()),-1.0,1.0);
			if (cosine<=0) continue;
			const double sineHalf=Sqrt(Max(.0025,(1-cosine)*.5));
			const double envelope=(RoadGeometry::structuralWidth(*a)+RoadGeometry::structuralWidth(*b))*.5;
			const auto first=getBezier(a->id),second=getBezier(b->id);
			if (!first || !second) continue;
			const float limit=Min(160.0f,Min(a->length,b->length)*.4f);
			float separation=Min(limit,static_cast<float>(envelope/(2*sineHalf)+2.0));
			// Curved arms can keep nearly the same tangent well past the node.
			// Verify their actual mouth positions instead of trusting that tangent alone.
			for (;separation<limit;separation+=2.0f)
			{
				const Vec3 pa=first->positionAt(a->nodeA==nodeId?separation:first->totalLength-separation);
				const Vec3 pb=second->positionAt(b->nodeA==nodeId?separation:second->totalLength-separation);
				if (Vec2{pa.x-pb.x,pa.z-pb.z}.length()>=envelope+1.0) break;
			}
			cutoff=Max(cutoff,Min(separation,limit));
		}
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

		const int survivor = toRemove == e1->id ? e.id : e1->id;
		if (getEdge(toRemove)->planId>=0 || getEdge(survivor)->planId>=0) continue;
		const auto firstCurve = getBezier(toRemove), secondCurve = getBezier(survivor);
		bool sameCorridor = firstCurve && secondCurve;
		if (sameCorridor)
		{
			// Curves with different control-handle lengths do not reach the same
			// physical place at the same parameter. Compare nearest corridor points.
			const double tolerance=Min(8.0,Min(w1,w2)*0.5-0.15);
			const auto followsCorridor=[&](const CubicBezier& source,const CubicBezier& target)
			{
				for (int sample=0;sample<=12;++sample)
				{
					const Vec3 point=source.evaluate(sample/12.0f);
					double nearest=Math::Inf,heightDifference=Math::Inf;
					Vec3 a=target.evaluate(0);
					for (int segment=1;segment<=32;++segment)
					{
						const Vec3 b=target.evaluate(segment/32.0f),delta=b-a;
						const Vec2 horizontal{delta.x,delta.z},relative{point.x-a.x,point.z-a.z};
						const double along=Clamp(relative.dot(horizontal)/Max(1e-9,horizontal.lengthSq()),0.0,1.0);
						const Vec3 projected=a+delta*along;
						const double distance=Vec2{point.x-projected.x,point.z-projected.z}.length();
						if (distance<nearest) { nearest=distance; heightDifference=Abs(point.y-projected.y); }
						a=b;
					}
					if (nearest>tolerance || heightDifference>1.0) { return false; }
				}
				return true;
			};
			sameCorridor=followsCorridor(*firstCurve,*secondCurve) && followsCorridor(*secondCurve,*firstCurve);
		}
		if (!sameCorridor) continue;
		for (auto& route : m_routes)
		{
			if (route.id<0 || !route.edgeIds.contains(toRemove)) continue;
			Array<int> replaced;
			for (const int id : route.edgeIds)
			{
				const int replacement=id==toRemove?survivor:id;
				if (replaced.isEmpty() || replaced.back()!=replacement) replaced << replacement;
			}
			route.edgeIds=std::move(replaced);
			if (!getEdge(survivor)->routeIds.contains(route.id)) getEdge(survivor)->routeIds << route.id;
		}
		// Planned roads are not consolidated with existing corridors.
		if (getEdge(toRemove)->planId>=0 || getEdge(survivor)->planId>=0) continue;
		const auto* removedEdge=getEdge(toRemove);const auto* keptEdge=getEdge(survivor);
		const bool reverse=removedEdge->nodeA!=keptEdge->nodeA;
		for (auto& object : m_objects) if (object.id>=0 && object.parentEdgeId==toRemove)
		{
			object.parentEdgeId=survivor;
			object.arcPos=object.arcPos/Max(.001f,removedEdge->length)*keptEdge->length;
			if (reverse) { object.arcPos=keptEdge->length-object.arcPos;object.lateralOffset=-object.lateralOffset;object.yawOffset+=static_cast<float>(Math::Pi); }
		}
		for (auto& sign : m_guideSigns) if (sign.id>=0 && sign.parentEdgeId==toRemove) sign.parentEdgeId=survivor;
		for (auto& marking : m_manualMarkings) if (marking.edgeId==toRemove) marking.edgeId=survivor;
		removeEdge(toRemove);
		removed = true;

		// 残った方のエッジ ID をマップに記録
		if (toRemove == it->second)
			it->second = e.id;
	}
	return removed;
}

int RoadNetwork::consolidateOverlappingRoads()
{
	const Stopwatch timer{StartImmediately::Yes};
	int joined = 0;
	const auto horizontal=[](Vec3 p) { return Vec2{p.x,p.z}; };
	const auto nearestParameter=[&](const CubicBezier& curve,Vec3 position)
	{
		const int samples=Clamp(static_cast<int>(Ceil(curve.totalLength/8)),8,512);
		float best=0;double distance=Math::Inf;
		for (int i=0;i<=samples;++i)
		{
			const float t=i/static_cast<float>(samples);
			const double current=horizontal(curve.evaluate(t)-position).lengthSq();
			if (current<distance) { distance=current;best=t; }
		}
		float low=Max(0.0f,best-1.0f/samples),high=Min(1.0f,best+1.0f/samples);
		for (int i=0;i<18;++i)
		{
			const float a=(2*low+high)/3,b=(low+2*high)/3;
			if (horizontal(curve.evaluate(a)-position).lengthSq()<horizontal(curve.evaluate(b)-position).lengthSq()) high=b;else low=a;
		}
		return (low+high)*.5f;
	};
	const auto eligible=[](const RoadEdge& edge) { return edge.id>=0 && edge.planId<0 && (edge.edgeState==EdgeState::Open || edge.edgeState==EdgeState::Existing); };
	const auto joinNodeInto=[&](int nodeId,int endpoint)
	{
		const Vec3 shift=getNode(endpoint)->position-getNode(nodeId)->position;
		const auto attachments=getNode(nodeId)->attachments;
		for (const auto& attachment : attachments)
		{
			auto* neighbor=getEdge(attachment.edgeId);if (!neighbor) continue;
			if (neighbor->nodeA==nodeId) { neighbor->nodeA=endpoint;neighbor->ctrlA+=shift; }
			if (neighbor->nodeB==nodeId) { neighbor->nodeB=endpoint;neighbor->ctrlB+=shift; }
			if (neighbor->nodeA==neighbor->nodeB) { removeEdge(neighbor->id);continue; }
			getNode(endpoint)->attachments << attachment;
			neighbor->length=getBezier(neighbor->id)->totalLength;
		}
		for (auto& sign : m_guideSigns)
		{
			if (sign.nodeEndId==nodeId) sign.nodeEndId=endpoint;
			if (sign.sourceNodeId==nodeId) sign.sourceNodeId=endpoint;
		}
		removeNode(nodeId);
		rebuildNodeConnectivity(endpoint,endpoint);
	};
	for (int pass=0;pass<32;++pass)
	{
		HashTable<Point,Array<int>> grid;
		for (const auto& edge : m_edges)
		{
			if (!eligible(edge)) continue;
			const auto curve=getBezier(edge.id);if (!curve) continue;
			const int x0=static_cast<int>(Floor((Min({curve->p0.x,curve->p1.x,curve->p2.x,curve->p3.x})-10)/128));
			const int x1=static_cast<int>(Floor((Max({curve->p0.x,curve->p1.x,curve->p2.x,curve->p3.x})+10)/128));
			const int z0=static_cast<int>(Floor((Min({curve->p0.z,curve->p1.z,curve->p2.z,curve->p3.z})-10)/128));
			const int z1=static_cast<int>(Floor((Max({curve->p0.z,curve->p1.z,curve->p2.z,curve->p3.z})+10)/128));
			for (int z=z0;z<=z1;++z) for (int x=x0;x<=x1;++x) grid[{x,z}] << edge.id;
		}
		Array<int> nodes;
		for (const auto& node : m_nodes) if (node.id>=0 && !node.attachments.isEmpty()) nodes << node.id;
		nodes.sort();
		int changed=0;
		HashSet<int> touched;
		for (const int nodeId : nodes)
		{
			const auto* node=getNode(nodeId);if (!node) continue;
			const Vec3 position=node->position;
			bool editable=true;
			for (const int id : node->edgeIds()) { const auto* attached=getEdge(id);if (attached && !eligible(*attached)) editable=false; }
			if (!editable) continue;
			const auto bucket=grid.find({static_cast<int>(Floor(position.x/128)),static_cast<int>(Floor(position.z/128))});
			if (bucket==grid.end()) continue;
			int bestEdge=-1;float bestT=0;double bestDistance=Math::Inf;
			for (const int edgeId : bucket->second)
			{
				const auto* edge=getEdge(edgeId);
				if (!edge || !eligible(*edge) || touched.contains(edgeId) || edge->nodeA==nodeId || edge->nodeB==nodeId) continue;
				const auto curve=getBezier(edgeId);if (!curve) continue;
				const float t=nearestParameter(*curve,position);
				const Vec3 target=curve->evaluate(t);
				const double distance=horizontal(target-position).length();
				if (distance>=bestDistance || distance>8) continue;
				bool parallelOverlap=(horizontal(position-curve->p0).length()<1.0 || horizontal(position-curve->p3).length()<1.0)
					&& Abs(target.y-position.y)<1.0;
				for (const int attachedId : node->edgeIds())
				{
					const auto* attached=getEdge(attachedId);
					if (!attached || !eligible(*attached) || touched.contains(attachedId)) continue;
					if ((edge->useElevation || attached->useElevation) && Abs(target.y-position.y)>1.0) continue;
					const auto arm=getBezier(attachedId);if (!arm || arm->totalLength<12) continue;
					const double width=Min(8.0,(RoadGeometry::structuralWidth(*attached)+RoadGeometry::structuralWidth(*edge))*.5-1.0);
					if (distance>=width) continue;
					const bool reversed=attached->nodeB==nodeId;
					const Vec2 direction=horizontal(arm->tangent(reversed?1.0f:0.0f)).normalized();
					if (Abs(direction.dot(horizontal(curve->tangent(t)).normalized()))<.90) continue;
					const double pavement=(RoadGeometry::roadbedRangeAt(*attached,.5f).width()+RoadGeometry::roadbedRangeAt(*edge,t).width())*.5;
					if (distance<Min(2.5,pavement-.5)) { parallelOverlap=true;break; }
					bool follows=true;
					for (const float fraction : {.2f,.5f,.8f})
					{
						const float length=Min(12.0f,arm->totalLength)*fraction;
						const Vec3 sample=arm->positionAt(reversed?arm->totalLength-length:length);
						const float nearest=nearestParameter(*curve,sample);
						if (horizontal(curve->evaluate(nearest)-sample).length()>=width) { follows=false;break; }
					}
					if (follows) { parallelOverlap=true;break; }
				}
				if (!parallelOverlap) continue;
				bestEdge=edgeId;bestT=t;bestDistance=distance;
			}
			if (bestEdge<0) continue;
			const auto curve=*getBezier(bestEdge);
			const auto edge=*getEdge(bestEdge);
			int endpoint=-1;
			if (horizontal(position-curve.p0).length()<8 && bestT*curve.totalLength<2) endpoint=edge.nodeA;
			if (horizontal(position-curve.p3).length()<8 && (1-bestT)*curve.totalLength<2) endpoint=edge.nodeB;
			if (endpoint>=0)
			{
				joinNodeInto(nodeId,endpoint);
			}
			else
			{
				if (bestT*curve.totalLength<.5f || (1-bestT)*curve.totalLength<.5f) continue;
				if (splitEdgeAtParameter(bestEdge,bestT,nodeId)<0) continue;
				for (const int attached : getNode(nodeId)->edgeIds()) touched.insert(attached);
			}
			++changed;
		}
		// Roads from one junction may share hundreds of metres before diverging.
		// Move the fork to the end of that common corridor instead of drawing two decks.
		for (const int nodeId : nodes)
		{
			const auto* node=getNode(nodeId);if (!node) continue;
			const auto arms=node->edgeIds();bool merged=false;
			for (size_t i=0;i<arms.size() && !merged;++i) for (size_t j=i+1;j<arms.size() && !merged;++j)
			{
				const auto* firstEdge=getEdge(arms[i]);const auto* secondEdge=getEdge(arms[j]);
				if (!firstEdge || !secondEdge || !eligible(*firstEdge) || !eligible(*secondEdge)) continue;
				if (firstEdge->useElevation!=secondEdge->useElevation) continue;
				const auto first=*getBezier(arms[i]),second=*getBezier(arms[j]);
				const bool firstReverse=firstEdge->nodeB==nodeId,secondReverse=secondEdge->nodeB==nodeId;
				const float firstSample=Min(12.0f,first.totalLength*.5f);
				const Vec2 firstDirection=horizontal(first.positionAt(firstReverse?first.totalLength-firstSample:firstSample)-node->position);
				const float secondSample=Min(12.0f,second.totalLength*.5f);
				const Vec2 secondDirection=horizontal(second.positionAt(secondReverse?second.totalLength-secondSample:secondSample)-node->position);
				if (firstDirection.normalized().dot(secondDirection.normalized())<.90) continue;
				const float span=Min(first.totalLength,second.totalLength);
				const double width=Min(16.0,(RoadGeometry::structuralWidth(*firstEdge)+RoadGeometry::structuralWidth(*secondEdge))*.5);
				float common=0;
				for (float distance=2;distance<=span+2;distance+=2)
				{
					const float arc=Min(distance,span);
					const Vec3 a=first.positionAt(firstReverse?first.totalLength-arc:arc);
					const Vec3 b=second.positionAt(secondReverse?second.totalLength-arc:arc);
					if (horizontal(a-b).length()>width || (firstEdge->useElevation && Abs(a.y-b.y)>1.0)) break;
					common=arc;
					if (arc==span) break;
				}
				if (common<Max(12.0,width*1.1)) continue;
				int joint=-1;
				const bool firstEnd=first.totalLength-common<2,secondEnd=second.totalLength-common<2;
				if (firstEnd) joint=firstReverse?firstEdge->nodeA:firstEdge->nodeB;
				else if (secondEnd) joint=secondReverse?secondEdge->nodeA:secondEdge->nodeB;
				if (firstEnd && secondEnd)
				{
					const int firstNode=firstReverse?firstEdge->nodeA:firstEdge->nodeB;
					const int secondNode=secondReverse?secondEdge->nodeA:secondEdge->nodeB;
					if (firstNode==secondNode) continue;
					bool editable=true;
					for (const int id : getNode(secondNode)->edgeIds()) if (!eligible(*getEdge(id))) editable=false;
					if (!editable) continue;
					joinNodeInto(secondNode,firstNode);
					++changed;merged=true;continue;
				}
				if (joint>=0 && getNode(joint)->attachments.size()>=6) continue;
				const float firstT=first.tFromArcLength(firstReverse?first.totalLength-common:common);
				const float secondT=second.tFromArcLength(secondReverse?second.totalLength-common:common);
				if (joint<0)
				{
					joint=splitEdgeAtParameter(arms[i],firstT);
					if (joint<0) continue;
					if (splitEdgeAtParameter(arms[j],secondT,joint)<0) continue;
				}
				else if (splitEdgeAtParameter(firstEnd?arms[j]:arms[i],firstEnd?secondT:firstT,joint)<0) continue;
				++changed;merged=true;
			}
		}
		removeDuplicateEdges(0);
		joined+=changed;
		if (!changed) break;
	}
	DBG_LOG(U"[RoadIntegrity] consolidatedNodes={} elapsedMs={:.2f}"_fmt(joined,timer.msF()));
	return joined;
}

bool RoadNetwork::resolveIntersections(int sinceEdgeId)
{
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
		dirtyBatch.sort();
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

			Array<int> sortedCandidates(candidates.begin(),candidates.end());
			sortedCandidates.sort();
			for (const int otherId : sortedCandidates)
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

				const CubicBezier first{na1->position,e1->ctrlA,e1->ctrlB,nb1->position};
				const CubicBezier second{na2->position,e2->ctrlA,e2->ctrlB,nb2->position};
				float firstT = 0, secondT = 0;
				bool hit = false;
				// Adaptive sampling bounds each chord to about 4m, followed by Newton refinement.
				const int firstSteps = Clamp(static_cast<int>(Ceil(first.totalLength/4)),8,512);
				const int secondSteps = Clamp(static_cast<int>(Ceil(second.totalLength/4)),8,512);
				for (int i=0;i<firstSteps && !hit;++i)
				{
					const Vec3 a=first.evaluate(i/static_cast<float>(firstSteps));
					const Vec3 b=first.evaluate((i+1)/static_cast<float>(firstSteps));
					for (int j=0;j<secondSteps && !hit;++j)
					{
						const Vec3 c=second.evaluate(j/static_cast<float>(secondSteps));
						const Vec3 d=second.evaluate((j+1)/static_cast<float>(secondSteps));
						if (Max(a.x,b.x)<Min(c.x,d.x) || Max(c.x,d.x)<Min(a.x,b.x)
							|| Max(a.z,b.z)<Min(c.z,d.z) || Max(c.z,d.z)<Min(a.z,b.z)) continue;
						float alongFirst,alongSecond;
						if (!segIntersect2D({a.x,a.z},{b.x,b.z},{c.x,c.z},{d.x,d.z},alongFirst,alongSecond)) continue;
						firstT=(i+alongFirst)/firstSteps; secondT=(j+alongSecond)/secondSteps;
						const auto derivative=[](const CubicBezier& curve,float t)
						{
							return (curve.p1-curve.p0)*(3*(1-t)*(1-t))+(curve.p2-curve.p1)*(6*t*(1-t))+(curve.p3-curve.p2)*(3*t*t);
						};
						for (int iteration=0;iteration<8;++iteration)
						{
							const Vec3 delta=second.evaluate(secondT)-first.evaluate(firstT);
							const Vec3 u=derivative(first,firstT),v=derivative(second,secondT);
							const double determinant=u.z*v.x-u.x*v.z;
							if (Abs(determinant)<1e-8) break;
							firstT=Clamp(firstT+static_cast<float>((v.x*delta.z-v.z*delta.x)/determinant),0.0f,1.0f);
							secondT=Clamp(secondT+static_cast<float>((u.x*delta.z-u.z*delta.x)/determinant),0.0f,1.0f);
						}
						const Vec3 p=first.evaluate(firstT),q=second.evaluate(secondT);
						hit=firstT*first.totalLength>0.5f && (1-firstT)*first.totalLength>0.5f
							&& secondT*second.totalLength>0.5f && (1-secondT)*second.totalLength>0.5f
							&& Vec2{p.x-q.x,p.z-q.z}.length()<.02
							&& (!(e1->useElevation || e2->useElevation) || Abs(p.y-q.y)<1.0);
					}
				}
				if (!hit) continue;
				const int splitNodeId=splitEdgeAtParameter(dirtyId,firstT);
				if (splitNodeId<0) continue;
				if (splitEdgeAtParameter(otherId,secondT,splitNodeId)<0) continue;
				getNode(splitNodeId)->type=NodeType::Intersection;
				for (const int id : getNode(splitNodeId)->edgeIds()) dirtyEdges.insert(id);

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
		? 3.75f
		: (edge.roadType == RoadType::Arterial ? 3.85f : 3.15f);
	const int nLanes = static_cast<int>(edge.lanes.size());
	const float roadbedWidth = nLanes * laneW;
	const float halfRoadbed = roadbedWidth * 0.5f;

	auto addPart = [&](RoadPartType type, float offset, float width, StringView defId = U"",
	                RoadPartPlacement placement = RoadPartPlacement::Strip,
	                RoadPartEnvelopeRole envelopeRole = RoadPartEnvelopeRole::Structural,
	                float repeatSpacing = 20.0f,
	                float repeatJitter = 0.0f)
	{
		RoadPart p;
		p.type      = type;
		p.defId     = String{ defId };
		p.offsetA_L = offset;
		p.offsetA_R = offset + width;
		p.offsetB_L = offset;
		p.offsetB_R = offset + width;
		p.build     = BuildState::Built;
		p.placement = placement;
		p.envelopeRole = envelopeRole;
		p.repeatSpacing = repeatSpacing;
		p.repeatJitter = repeatJitter;
		p.useDefinitionRepeatSpacing = false;
		p.useDefinitionRepeatJitter = false;
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
		const float sidewalkW = 3.8f;
		const float curbW = 0.22f;
		const float gutterW = 0.42f;

		float x = -halfRoadbed - curbW - gutterW - sidewalkW;
		addPart(RoadPartType::Sidewalk, x, sidewalkW, U"sidewalk_tile"); x += sidewalkW;
		addPart(RoadPartType::RoadsideGutter, x, gutterW, U"roadside_gutter_concrete"); x += gutterW;
		addPart(RoadPartType::Curb, x, curbW, U"curb_concrete"); x += curbW;
		addPart(RoadPartType::Roadbed, x, roadbedWidth, U"roadbed_asphalt"); x += roadbedWidth;
		addPart(RoadPartType::Curb, x, curbW, U"curb_concrete"); x += curbW;
		addPart(RoadPartType::RoadsideGutter, x, gutterW, U"roadside_gutter_concrete"); x += gutterW;
		addPart(RoadPartType::Sidewalk, x, sidewalkW, U"sidewalk_tile");
		addPart(RoadPartType::UtilityPole, halfRoadbed + curbW + gutterW + 0.72f, 0.0f, U"utility_pole_concrete",
		        RoadPartPlacement::RepeatAlongEdge, RoadPartEnvelopeRole::RoadOwnedObject, 36.0f, 4.0f);
		break;
	}
	default: // LocalRoad
	{
		const float gutterW = 0.42f;

		float x = -halfRoadbed - gutterW;
		addPart(RoadPartType::RoadsideGutter, x, gutterW, U"roadside_gutter_concrete"); x += gutterW;
		addPart(RoadPartType::Roadbed, x, roadbedWidth, U"roadbed_asphalt"); x += roadbedWidth;
		addPart(RoadPartType::RoadsideGutter, x, gutterW, U"roadside_gutter_concrete");
		addPart(RoadPartType::UtilityPole, halfRoadbed + gutterW + 0.48f, 0.0f, U"utility_pole_concrete",
		        RoadPartPlacement::RepeatAlongEdge, RoadPartEnvelopeRole::RoadOwnedObject, 31.0f, 3.0f);
		break;
	}
	}
}

Array<Lane> RoadNetwork::buildDefaultLanes(int numLanes, RoadType rt)
{
	float laneWidth = 3.15f;
	if (rt == RoadType::Arterial)
		laneWidth = 3.85f;
	if (rt == RoadType::Expressway || rt == RoadType::Highway)
		laneWidth = 3.85f;

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
		if (!hasMedian) { left += (forwardCount - numLanes * 0.5f) * laneWidth; }
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
			if (keepNode->attachments.size() >= 3 && rmNode->attachments.size() >= 3 && edge.length > 2.0f) { continue; }


			const Vec3 mergedPosition = (keepNode->position+rmNode->position)*0.5;
			const Vec3 keepShift = mergedPosition-keepNode->position;
			const Vec3 removedShift = mergedPosition-rmNode->position;
			for (const int attached : keepNode->edgeIds())
			{
				if (auto* neighbor = getEdge(attached))
				{
					if (neighbor->nodeA == keepId) { neighbor->ctrlA += keepShift; }
					if (neighbor->nodeB == keepId) { neighbor->ctrlB += keepShift; }
				}
			}
			// rmNode の接続エッジを keepNode に移し替える
			for (const int eid : rmNode->edgeIds())
			{
				if (eid == edge.id) continue;
				RoadEdge* e = getEdge(eid);
				if (!e) continue;

				if (e->nodeA == rmId) { e->nodeA = keepId; e->ctrlA += removedShift; }
				if (e->nodeB == rmId) { e->nodeB = keepId; e->ctrlB += removedShift; }

				if (e->nodeA == keepId && e->nodeB == keepId)
				{
					removeEdge(eid);
					continue;
				}

				if (!keepNode->getAttachment(eid))
					keepNode->addEdge(eid);
			}

			keepNode->position = mergedPosition;

			removeEdge(edge.id);
			removeNode(rmId);
			for (const int attached : keepNode->edgeIds())
			{
				if (auto* neighbor = getEdge(attached))
				{
					if (const auto curve = getBezier(attached)) { neighbor->length = curve->totalLength; }
					rebuildNodeConnectivity(neighbor->nodeA,neighbor->nodeB);
				}
			}

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

void RoadNetwork::recomputeAutoGuideSignsForNode(int nodeId, const GuideSign::AutoPlacementClearance* clearance)
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
		auto autos = GuideSign::InferAutoForEdge(*edge, *this, clearance);
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
	const GuideSign::AutoPlacementClearance clearance{ *this };
	for (auto& n : m_nodes)
	{
		if (n.id < 0) continue;
		recomputeAutoGuideSignsForNode(n.id, &clearance);
	}
	int generated=0, violations=0;
	for (const auto& sign : m_guideSigns)
	{
		if (sign.id<0 || !sign.autoGenerated) { continue; }
		const auto* edge=getEdge(sign.parentEdgeId);
		const auto curve=getBezier(sign.parentEdgeId);
		if (!edge || !curve) { continue; }
		++generated;
		const float arc=sign.nodeEndId==edge->nodeA?sign.arcOffset:curve->totalLength-sign.arcOffset;
		if (!clearance.allows(curve->positionAt(arc),edge->totalWidth()*0.5+4.0)) { ++violations; }
	}
	DBG_LOG(U"[GuideSignClearance] generated={} exclusionViolations={} clearanceM=30"_fmt(generated,violations));
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
	if (!plan || plan->state != PlanState::Planning || plan->edgeIds.isEmpty()) return false;

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
	const double duration = lengthKm * constructionMonthsPerKm(roadType) * kConstructionUnitsPerMonth;
	return Max(kConstructionUnitsPerDay, duration);
}

double RoadNetwork::estimatePlanCost(RoadType roadType, double totalLengthMeters) const
{
	const double lengthKm = totalLengthMeters / 1000.0;
	return lengthKm * constructionCostPerKm(roadType);
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
	const double gyA = world.sampleHeight(
		static_cast<float>(nA->position.x), static_cast<float>(nA->position.z));
	const double gyB = world.sampleHeight(
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
		const float terrainY = world.sampleHeight(
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
