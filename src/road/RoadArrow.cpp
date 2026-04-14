#include "RoadArrow.hpp"
#include "RoadNetwork.hpp"
#include "../traffic/TrafficCommon.hpp"
#include "../traffic/TrafficGraph.hpp"

namespace
{
	// ===== reference/204.ht*.gif から OpenCV で抽出した輪郭（ピクセル単位） =====
	// 1px ≈ 1cm 横方向、Y は中心=0、巻き順 CW（Siv3D Polygon 互換）
	// 参照: chore/extract_arrow_contour.py 相当の処理（過去対話で実施）

	// ht2 直進矢印: 7頂点。X∈[0,501] tip=0 / Y∈[-26.5, 26.5]
	const Array<Vec2> kPx_Straight = {
		Vec2{ 253.0, -26.5 }, Vec2{ 254.0, -10.5 }, Vec2{ 501.0, -10.5 },
		Vec2{ 501.0,   9.5 }, Vec2{ 254.0,   9.5 }, Vec2{ 253.0,  26.5 },
		Vec2{   0.0,  -5.5 },
	};

	// ht1 左折矢印: 11頂点。X∈[0,492] tip=0 / Y∈[-38, 38] 矢じり先端は +Y 側
	const Array<Vec2> kPx_Left = {
		Vec2{  75.0,   0.0 }, Vec2{  89.0, -24.0 }, Vec2{ 113.0, -38.0 },
		Vec2{ 492.0, -38.0 }, Vec2{ 492.0, -19.0 }, Vec2{ 147.0, -19.0 },
		Vec2{ 135.0, -10.0 }, Vec2{ 134.0,   0.0 }, Vec2{ 208.0,   0.0 },
		Vec2{ 103.0,  38.0 }, Vec2{   0.0,   0.0 },
	};

	// ht3 直進+左折矢印: 16頂点。X∈[0,501] tip=0 / Y∈[-46.5, 46.5] 左折部は +Y 側
	const Array<Vec2> kPx_StraightLeft = {
		Vec2{ 222.0, -46.5 }, Vec2{ 223.0, -28.5 }, Vec2{ 501.0, -28.5 },
		Vec2{ 501.0, -10.5 }, Vec2{ 428.0, -10.5 }, Vec2{ 416.0,  -6.5 },
		Vec2{ 409.0,   9.5 }, Vec2{ 483.0,   9.5 }, Vec2{ 373.0,  46.5 },
		Vec2{ 260.0,   9.5 }, Vec2{ 334.0,   9.5 }, Vec2{ 339.0,  -2.5 },
		Vec2{ 352.0, -10.5 }, Vec2{ 223.0, -10.5 }, Vec2{ 219.0,   8.5 },
		Vec2{   0.0, -18.5 },
	};

	/// @brief ピクセル輪郭を実寸ローカル座標に変換する
	/// @details
	///   - X 反転（リファレンス画像は tip=左、ローカル規約は tip=+X）
	///   - X スケール: 全 px 範囲 → kArrowLength_m
	///   - Y スケール: 全 px 範囲 → 指定 widthMeters
	///   - flipY=true で Y 反転（左→右変換用）
	///   - 巻き順は X 反転で逆転するので、配列を reverse する
	Array<Vec2> normalizeContour(const Array<Vec2>& px, double pxXMax,
	                             double widthMeters, double pxYRange, bool flipY)
	{
		const double scaleX = RoadArrow::kArrowLength_m / pxXMax;
		const double scaleY = widthMeters / pxYRange;

		Array<Vec2> out;
		out.reserve(px.size());
		for (const auto& v : px)
		{
			const double localX = (pxXMax - v.x) * scaleX;          // 反転 + スケール
			const double localY = v.y * scaleY * (flipY ? -1.0 : 1.0);
			out << Vec2{ localX, localY };
		}
		// X 反転で巻き順が逆になる + flipY でも逆になる
		// 元 CW → X 反転で CCW → flipY で CW（つまり flipY なしなら CCW、flipY ありで CW）
		// Siv3D は CW を要求するので、flipY=false の場合のみ reverse する
		if (!flipY)
		{
			out.reverse();
		}
		return out;
	}

	// ピクセル X 範囲・Y 範囲（normalizeContour に渡す定数）
	constexpr double kPxXMax_Straight     = 501.0;
	constexpr double kPxYRange_Straight   = 53.0;   // -26.5 〜 26.5
	constexpr double kPxXMax_Left         = 492.0;
	constexpr double kPxYRange_Left       = 76.0;   // -38 〜 38
	constexpr double kPxXMax_StraightLeft = 501.0;
	constexpr double kPxYRange_StraightLeft = 93.0; // -46.5 〜 46.5
}

Polygon RoadArrow::CreateContour(RoadArrowType type)
{
	switch (type)
	{
	case RoadArrowType::Straight:
		return Polygon{ normalizeContour(kPx_Straight, kPxXMax_Straight,
		                                  kArrowWidthStraight_m, kPxYRange_Straight, false) };
	case RoadArrowType::Left:
		return Polygon{ normalizeContour(kPx_Left, kPxXMax_Left,
		                                  kArrowWidthTurn_m, kPxYRange_Left, false) };
	case RoadArrowType::Right:
		return Polygon{ normalizeContour(kPx_Left, kPxXMax_Left,
		                                  kArrowWidthTurn_m, kPxYRange_Left, true) };
	case RoadArrowType::StraightLeft:
		return Polygon{ normalizeContour(kPx_StraightLeft, kPxXMax_StraightLeft,
		                                  kArrowWidthCombined_m, kPxYRange_StraightLeft, false) };
	case RoadArrowType::StraightRight:
		return Polygon{ normalizeContour(kPx_StraightLeft, kPxXMax_StraightLeft,
		                                  kArrowWidthCombined_m, kPxYRange_StraightLeft, true) };
	default:
		return Polygon{};
	}
}

MeshData RoadArrow::CreateMesh(RoadArrowType type)
{
	const Polygon poly = CreateContour(type);
	if (poly.isEmpty())
	{
		return MeshData{};
	}

	const auto& outerVerts = poly.outer();
	const auto& triIndices = poly.indices();

	MeshData md;
	md.vertices.reserve(outerVerts.size());
	for (const auto& v : outerVerts)
	{
		Vertex3D vert;
		// ローカル座標: X=進行方向, Z=lateral（XZ 平面に展開）, Y=0
		vert.pos    = Float3{ static_cast<float>(v.x), 0.0f, static_cast<float>(v.y) };
		vert.normal = Float3{ 0.0f, 1.0f, 0.0f };
		vert.tex    = Float2{ static_cast<float>(v.x), static_cast<float>(v.y) };
		md.vertices << vert;
	}

	md.indices.reserve(triIndices.size());
	for (const auto& tri : triIndices)
	{
		md.indices << TriangleIndex32{ tri.i0, tri.i1, tri.i2 };
	}

	return md;
}

RoadArrowType RoadArrow::InferType(const RoadNetwork& network,
                                   int edgeId, int laneIndex, int towardNodeId)
{
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
