#include "RoadNetwork.hpp"
#include "RoadGeometry.hpp"
#include "../debug/DebugLog.hpp"
#include "../world/World.hpp"

namespace
{
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
			ea->designGrade = tmpl.designGrade;
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
			eb->designGrade = tmpl.designGrade;
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
		const float c = e->hasRailLanes() ? 0.0f : Min(cutoff, maxCut);
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
		if (getEdge(eid0)->designGrade || getEdge(eid1)->designGrade) { continue; }
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

bool RoadNetwork::resolveIntersections(int sinceEdgeId, Array<int>* trackedEdges)
{
	// Keep construction ownership on the new road only, including after repeated splits.
	const auto splitTracked=[&](int edgeId,float parameter,int existingNode=-1)
	{
		const auto* original=getEdge(edgeId);if (!original) { return -1; }
		const int start=original->nodeA,end=original->nodeB;
		const int joint=splitEdgeAtParameter(edgeId,parameter,existingNode);
		if (joint<0 || !trackedEdges || !trackedEdges->contains(edgeId)) { return joint; }
		int first=-1,second=-1;
		for (const int child : getNode(joint)->edgeIds())
		{
			const auto* edge=getEdge(child);
			if (edge->nodeA==start || edge->nodeB==start) { first=child; }
			if (edge->nodeA==end || edge->nodeB==end) { second=child; }
		}
		Array<int> updated;
		for (const int id : *trackedEdges)
		{
			if (id!=edgeId) { updated<<id; }
			else { if (first>=0) { updated<<first; } if (second>=0) { updated<<second; } }
		}
		*trackedEdges=std::move(updated);return joint;
	};
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
							&& (!(e1->usesDesignHeight() || e2->usesDesignHeight()) || Abs(p.y-q.y)<1.0);
					}
				}
				if (!hit) continue;
				const int splitNodeId=splitTracked(dirtyId,firstT);
				if (splitNodeId<0) continue;
				if (splitTracked(otherId,secondT,splitNodeId)<0) continue;
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

