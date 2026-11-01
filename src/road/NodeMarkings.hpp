#pragma once
#include "RoadNetwork.hpp"

/// @brief ノード内（分合流部・交差点）の区画線と導流帯。
/// @details 自動では、自動車専用道だけが 3 本以上集まる節点（高速道路の分岐・合流）で、車線接続に沿って
///   車線の左右の境界線を切り口から切り口へ引き、同じ点から別の道路へ分かれる（または別の道路から同じ点へ集まる）
///   2 本の線の間を導流帯にする。利用者が編集したノード（RoadNode::customMarkings）は保存された線・導流帯をそのまま使う。
namespace NodeMarkings
{
	struct Set
	{
		Array<NodeMarkingLine> lines;
		Array<NodeMarkingZone> zones;
	};

	/// @brief 線種の強さ。2 本の道路で線種が違う境界は強いほうを引く（実線 > 太い破線 > 破線）。
	inline int strength(LineType type)
	{
		switch (type)
		{
		case LineType::None: return 0;
		case LineType::DashedWhite: return 1;
		case LineType::DashedWhiteThick: return 2;
		case LineType::SolidWhite: return 3;
		case LineType::SolidYellow: return 4;
		case LineType::DoubleYellow: return 5;
		}
		return 0;
	}

	inline LineType stronger(LineType a, LineType b) { return strength(a) >= strength(b) ? a : b; }

	/// @brief 自動で引くノードか（自動車専用道だけが 3 本以上集まる分合流部）。
	inline bool automaticApplies(const RoadNetwork& network, const RoadNode& node)
	{
		if (node.attachments.size() < 3) { return false; }
		for (const auto& attachment : node.attachments)
		{
			const auto* edge = network.getEdge(attachment.edgeId);
			if (!edge || edge->roadType != RoadType::Expressway) { return false; }
		}
		return true;
	}

	/// @brief 区画線を引くノードか（手で設定した、または自動の対象）。
	inline bool applies(const RoadNetwork& network, const RoadNode& node)
	{
		return node.customMarkings || automaticApplies(network, node);
	}

	/// @brief 道路のノード側の切り口での車線の境界（横位置と線種）。横位置の小さい順。
	inline Array<std::pair<float, LineType>> boundaries(const RoadEdge& edge, int nodeId)
	{
		const bool atA = edge.nodeA == nodeId;
		Array<std::pair<float, LineType>> result;
		const auto add = [&](float lateral, LineType type)
		{
			for (auto& [existing, existingType] : result)
			{
				if (Abs(existing - lateral) < .05f) { existingType = stronger(existingType, type); return; }
			}
			result << std::pair{ lateral, type };
		};
		for (const auto& lane : edge.lanes)
		{
			add(atA ? lane.offsetA_L : lane.offsetB_L, lane.lineLeft);
			add(atA ? lane.offsetA_R : lane.offsetB_R, lane.lineRight);
		}
		result.sort_by([](const auto& a, const auto& b) { return a.first < b.first; });
		return result;
	}

	inline bool sameEnd(const NodeLineEnd& a, const NodeLineEnd& b) { return a.edgeId == b.edgeId && Abs(a.lateral - b.lateral) < .05f; }

	inline Set automatic(const RoadNetwork& network, int nodeId)
	{
		Set set;
		const RoadNode* node = network.getNode(nodeId);
		if (!node || !automaticApplies(network, *node)) { return set; }
		const auto addLine = [&](NodeLineEnd from, NodeLineEnd to, LineType type)
		{
			if (type == LineType::None) { return; }
			for (auto& line : set.lines)
			{
				if (sameEnd(line.from, from) && sameEnd(line.to, to)) { line.type = stronger(line.type, type); return; }
			}
			set.lines << NodeMarkingLine{ from, to, type };
		};
		for (const auto& connection : node->laneConnections)
		{
			const auto* from = network.getEdge(connection.fromEdgeId);
			const auto* to = network.getEdge(connection.toEdgeId);
			if (!from || !to || connection.fromLaneIndex >= static_cast<int>(from->lanes.size()) || connection.toLaneIndex >= static_cast<int>(to->lanes.size())) { continue; }
			const Lane& a = from->lanes[connection.fromLaneIndex];
			const Lane& b = to->lanes[connection.toLaneIndex];
			// 進行方向の左右。流入側はノードが B 端なら A→B に走る（左 = offset_L）。流出側はノードが A 端なら A→B。
			const bool fromForward = from->nodeB == nodeId, toForward = to->nodeA == nodeId;
			const float fromAtNodeL = from->nodeA == nodeId ? a.offsetA_L : a.offsetB_L, fromAtNodeR = from->nodeA == nodeId ? a.offsetA_R : a.offsetB_R;
			const float toAtNodeL = to->nodeA == nodeId ? b.offsetA_L : b.offsetB_L, toAtNodeR = to->nodeA == nodeId ? b.offsetA_R : b.offsetB_R;
			const NodeLineEnd fromLeft{ from->id, fromForward ? fromAtNodeL : fromAtNodeR }, fromRight{ from->id, fromForward ? fromAtNodeR : fromAtNodeL };
			const NodeLineEnd toLeft{ to->id, toForward ? toAtNodeL : toAtNodeR }, toRight{ to->id, toForward ? toAtNodeR : toAtNodeL };
			const LineType fromLeftType = fromForward ? a.lineLeft : a.lineRight, fromRightType = fromForward ? a.lineRight : a.lineLeft;
			const LineType toLeftType = toForward ? b.lineLeft : b.lineRight, toRightType = toForward ? b.lineRight : b.lineLeft;
			// 線のない側（料金所前後など）は引かない。
			if (fromLeftType != LineType::None && toLeftType != LineType::None) { addLine(fromLeft, toLeft, stronger(fromLeftType, toLeftType)); }
			if (fromRightType != LineType::None && toRightType != LineType::None) { addLine(fromRight, toRight, stronger(fromRightType, toRightType)); }
		}
		// 導流帯: 同じ点から別の道路へ分かれる 2 本（分流）、別の道路から同じ点へ集まる 2 本（合流）の間。
		// 1 つの分合流につき、開いた側の端どうしが最も近い（向かい合う内側の）2 本だけを使う。
		const auto endPosition = [&](const NodeLineEnd& end) -> Vec2
		{
			const RoadEdge* edge = network.getEdge(end.edgeId);
			const auto bez = network.getBezier(end.edgeId);
			if (!edge || !bez) { return node->position.xz(); }
			const float s = edge->nodeA == nodeId ? 0.0f : bez->totalLength;
			const Vec3 p = bez->positionAt(s) + tangentToRight(bez->tangentAt(s)) * static_cast<double>(end.lateral);
			return p.xz();
		};
		struct Candidate { int i, j; int edgeLow, edgeHigh; double gap; };
		Array<Candidate> candidates;
		for (size_t i = 0; i < set.lines.size(); ++i)
		{
			for (size_t j = i + 1; j < set.lines.size(); ++j)
			{
				const auto& p = set.lines[i];
				const auto& q = set.lines[j];
				const bool split = sameEnd(p.from, q.from) && p.to.edgeId != q.to.edgeId;
				const bool join = sameEnd(p.to, q.to) && p.from.edgeId != q.from.edgeId;
				if (!split && !join) { continue; }
				const NodeLineEnd& openP = split ? p.to : p.from;
				const NodeLineEnd& openQ = split ? q.to : q.from;
				candidates << Candidate{ static_cast<int>(i), static_cast<int>(j),
					Min(openP.edgeId, openQ.edgeId), Max(openP.edgeId, openQ.edgeId), endPosition(openP).distanceFrom(endPosition(openQ)) };
			}
		}
		for (size_t k = 0; k < candidates.size(); ++k)
		{
			const auto& c = candidates[k];
			bool best = true;
			for (size_t m = 0; m < candidates.size() && best; ++m)
			{
				const auto& o = candidates[m];
				if (m == k || o.edgeLow != c.edgeLow || o.edgeHigh != c.edgeHigh) { continue; }
				// 同じ 2 本の道路の間の分合流（共有する端が違っても同じ導流帯）は、最も近い組だけ残す。
				if (o.gap < c.gap - 1e-6 || (Abs(o.gap - c.gap) <= 1e-6 && m < k)) { best = false; }
			}
			if (best) { set.zones << NodeMarkingZone{ c.i, c.j, NodeZonePattern::Chevron }; }
		}
		return set;
	}

	/// @brief 描画に使う線と導流帯。手で設定したノードは保存した内容、そうでなければ自動。
	inline Set effective(const RoadNetwork& network, int nodeId)
	{
		const RoadNode* node = network.getNode(nodeId);
		if (!node) { return {}; }
		if (node->customMarkings) { return Set{ node->markingLines, node->markingZones }; }
		return automatic(network, nodeId);
	}
}
