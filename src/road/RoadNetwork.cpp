#include "../gen/GenerationSettings.hpp"
#include "RoadNetwork.hpp"
#include "../world/World.hpp"

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
	e->farmAccess = tmpl.farmAccess;
	e->electrified = tmpl.electrified;
	e->depotTrack = tmpl.depotTrack;
	e->parts        = tmpl.parts;
	e->lanes        = tmpl.lanes;
	// 断面は複製しても運行中の予約は引き継がない。旧エッジの列車が解放できなくなるため。
	for (auto& lane : e->lanes) { lane.reservedBy = -1; }
	e->occupiedBy = -1;
	e->laneVehicles = Array<Array<int>>(e->lanes.size());
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
	const auto* node = getNode(nodeId);
	if (!node) { return false; }
	for (const auto& attachment : node->attachments)
	{
		const auto* edge = getEdge(attachment.edgeId);
		if (edge && edge->useElevation) { return true; }
	}
	return false;
}

bool RoadNetwork::nodeUsesDesignHeight(int nodeId) const
{
	const RoadNode* node = getNode(nodeId);
	if (!node) return false;
	for (const auto& att : node->attachments)
	{
		const RoadEdge* e = getEdge(att.edgeId);
		if (e && e->usesDesignHeight()) return true;
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
		std::abs(nA->position.y - gyA) > GenerationSettings::get().roads_endpointElevationThreshold ||
		std::abs(nB->position.y - gyB) > GenerationSettings::get().roads_endpointElevationThreshold;
	edge->tunnel=false;
	if (const auto curve=getBezier(edgeId)) for (float arc=0;arc<=curve->totalLength;arc+=8)
	{
		const Vec3 p=curve->positionAt(arc); const double ground=world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z));
		edge->tunnel|=ground-p.y>GenerationSettings::get().roads_maximumCut;
		edge->useElevation|=Abs(ground-p.y)>GenerationSettings::get().roads_interiorElevationThreshold;
	}
	edge->useElevation|=edge->tunnel;
}

void RoadNetwork::generatePiersForEdge(int edgeId, const World& world)
{
	const RoadEdge* edge = getEdge(edgeId);
	if (!edge || !edge->useElevation) return;

	// 橋脚だけ再生成し、同じ道路にある他の設備を保持する。
	Array<int> previous;
	for (const auto& object : objects()) { if (object.id >= 0 && object.parentEdgeId == edgeId && object.type == RoadObjectType::Pier) { previous << object.id; } }
	for (const int id : previous) { removeObject(id); }

	const auto bez = getBezier(edgeId);
	if (!bez) return;

	const auto& settings = GenerationSettings::get();
	const float totalLen = bez->totalLength;
	if (totalLen < settings.roads_pierMinimumSpan || edge->tunnel) { return; }
	const int steps = Max(1, static_cast<int>(Ceil(totalLen / settings.roads_pierInterval)));

	for (int i = 0; i < steps; ++i)
	{
		const float s = totalLen * (i + .5f) / steps;

		const Vec3 pos = bez->positionAt(s);
		const float terrainY = world.sampleHeight(
			static_cast<float>(pos.x), static_cast<float>(pos.z));
		const float gap = static_cast<float>(pos.y) - terrainY;

		if (gap >= settings.roads_pierMinimumClearance)
		{
			RoadObject pier;
			pier.parentEdgeId = edgeId;
			pier.arcPos = s;
			pier.type = RoadObjectType::Pier;
			addObject(pier);
		}
	}
}
