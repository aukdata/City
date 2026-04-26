#include "RoadArrow.hpp"
#include "RoadNetwork.hpp"
#include "../traffic/TrafficCommon.hpp"
#include "../traffic/TrafficGraph.hpp"


RoadArrowType RoadArrow::InferType(const RoadNetwork& network,
                                   int edgeId, int laneIndex, int towardNodeId)
{
	// レーン接続グラフから進行可能な旋回方向を読み取り、路面矢印種別へ要約する。
	// 対象レーンから実際に到達できる出口方向集合を集め、その組み合わせから標示矢印種別を決める。
	const RoadEdge* edge = network.getEdge(edgeId);
	const RoadNode* node = network.getNode(towardNodeId);
	if (!edge || !node)
	{
		return RoadArrowType::None;
	}
	if (laneIndex < 0 || laneIndex >= static_cast<int>(edge->lanes.size()))
	{
		return RoadArrowType::None;
	}

	const Lane& lane = edge->lanes[laneIndex];

	// このレーンが当該ノードへの entry かチェック
	const bool isAtA = (edge->nodeA == towardNodeId);
	const bool isAtB = (edge->nodeB == towardNodeId);
	if (!isAtA && !isAtB)
	{
		return RoadArrowType::None;
	}
	const bool entersHere =
		(lane.dir == LaneDir::Forward  && isAtB) ||
		(lane.dir == LaneDir::Backward && isAtA);
	if (!entersHere)
	{
		return RoadArrowType::None;
	}

	// このレーンから出る LaneConnection を抽出
	Array<const LaneConnection*> outConns;
	for (const auto& c : node->laneConnections)
	{
		if (c.fromEdgeId == edgeId && c.fromLaneIndex == laneIndex)
		{
			outConns << &c;
		}
	}
	if (outConns.isEmpty())
	{
		return RoadArrowType::None;
	}

	// entry の進行方向角度（ノードに向かって入る方向）
	const auto bezIn = network.getBezier(edgeId);
	if (!bezIn)
	{
		return RoadArrowType::None;
	}
	// Forward + nodeB end: tangent at end (B→outside だが、ここでは A→B 方向 = 進行方向)
	// Backward + nodeA end: -tangent at start (進行方向は B→A)
	const Vec3 tInRaw = isAtB
		? bezIn->tangentAt(bezIn->totalLength)
		: -bezIn->tangentAt(0.0f);
	const float inAngle = static_cast<float>(Math::Atan2(tInRaw.x, tInRaw.z));

	// 各 connection を分類
	bool hasStraight = false, hasLeft = false, hasRight = false, hasUTurn = false;
	for (const auto* c : outConns)
	{
		// UTurn トポロジ判定（角度より優先）
		if (c->toEdgeId == c->fromEdgeId)
		{
			hasUTurn = true;
			continue;
		}

		const RoadEdge* outEdge = network.getEdge(c->toEdgeId);
		if (!outEdge) { continue; }
		if (c->toLaneIndex < 0 || c->toLaneIndex >= static_cast<int>(outEdge->lanes.size()))
		{
			continue;
		}
		const auto bezOut = network.getBezier(c->toEdgeId);
		if (!bezOut) { continue; }

		const LaneDir outDir = outEdge->lanes[c->toLaneIndex].dir;
		const bool outAtA = (outEdge->nodeA == towardNodeId);
		// 退出方向: Forward + nodeA → tangent at start, Backward + nodeB → -tangent at end
		const Vec3 tOutRaw = (outDir == LaneDir::Forward)
			? (outAtA ? bezOut->tangentAt(0.0f) : -bezOut->tangentAt(bezOut->totalLength))
			: (outAtA ? -bezOut->tangentAt(0.0f) : -bezOut->tangentAt(bezOut->totalLength));
		const float outAngle = static_cast<float>(Math::Atan2(tOutRaw.x, tOutRaw.z));

		const TurnType turn = TrafficCommon::classifyTurnByAngles(inAngle, outAngle);
		switch (turn)
		{
		case TurnType::Straight: hasStraight = true; break;
		case TurnType::Left:     hasLeft     = true; break;
		case TurnType::Right:    hasRight    = true; break;
		case TurnType::UTurn:    hasUTurn    = true; break;
		}
	}

	const int s = hasStraight ? 1 : 0;
	const int l = hasLeft     ? 1 : 0;
	const int r = hasRight    ? 1 : 0;
	const int n = s + l + r;

	if (n == 0)
	{
		return hasUTurn ? RoadArrowType::UTurn : RoadArrowType::None;
	}
	else if (n == 1)
	{
		if (s) { return RoadArrowType::Straight; }
		if (l) { return RoadArrowType::Left; }
		return RoadArrowType::Right;
	}
	else if (n == 2)
	{
		if (s && l) { return RoadArrowType::StraightLeft; }
		if (s && r) { return RoadArrowType::StraightRight; }
		return RoadArrowType::LeftRight;
	}
	// n == 3
	return RoadArrowType::All;
}
