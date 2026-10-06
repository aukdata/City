#include "RoadRenderer.hpp"
#include "RoadRenderDetail.hpp"
#include "../road/RoadArrow.hpp"
#include "../road/JunctionGeometry.hpp"
#include "../road/RoadMarkingGenerator.hpp"

using namespace RoadRenderDetail;

/// @file
/// @brief 車線・横断歩道・停止線・矢印の構築。

namespace
{
	struct LineStyle { ColorF color; float lineWidth; float dashLen; float gapLen; };
	struct EdgeLineInfo { Vec3 pos; Vec3 tangent; };
	/// @brief ベジェに沿った破線（または実線）ポリゴン帯を追記する
	/// @param sStart  描画開始弧長 [m]
	/// @param sEnd    描画終了弧長 [m]
	void appendDashedStrip(
		Array<Vertex3D>&        vertices,
		Array<TriangleIndex32>& indices,
		const CubicBezier&      bez,
		const World&            world,
		float offsetA, float offsetB, float halfLW,
		float dashLength, float gapLength,
		float sStart, float sEnd,
		float terrainLift = static_cast<float>(kRoadLineLift),
		bool useElevation = false)
	{
		const float spanLen  = sEnd - sStart;
		if (spanLen <= 0.0f) return;

		const bool  solid    = (dashLength <= 0.0f || gapLength <= 0.0f);
		const float cycleLen = dashLength + gapLength;
		const int   N        = Clamp(static_cast<int>(spanLen / 2.0f) + 1, 5, 200);
		const float totalLen = bez.totalLength;

		for (int i = 0; i < N; ++i)
		{
			const float s0   = sStart + (i       / static_cast<float>(N)) * spanLen;
			const float s1   = sStart + ((i + 1) / static_cast<float>(N)) * spanLen;
			const float sMid = (s0 + s1) * 0.5f;

			if (!solid && (Math::Fmod(sMid - sStart, cycleLen) >= dashLength))
				continue;

			const uint32 base = static_cast<uint32>(vertices.size());

			for (int j = 0; j <= 1; ++j)
			{
				const float   s  = (j == 0) ? s0 : s1;
				const auto    sl = makeSlice(bez, world, s, terrainLift, useElevation);
				// A端→B端のオフセット線形補間でテーパーを表現
				const float   t  = (totalLen > 0.0f) ? (s / totalLen) : 0.0f;
				const float   offset = offsetA * (1.0f - t) + offsetB * t;
				const Vec3    lc = sl.center + sl.right * offset;
				vertices << makeVert(lc - sl.right * halfLW, 0.0f, s / totalLen);
				vertices << makeVert(lc + sl.right * halfLW, 1.0f, s / totalLen);
			}

			appendQuad(indices, base, base + 1, base + 2, base + 3);
		}
	}

	/// @brief 2 点間をつなぐ平たい帯（路面塗り用の短冊）を追記する
	void appendBar(Array<Vertex3D>& vertices, Array<TriangleIndex32>& indices,
	               const Vec3& p0, const Vec3& p1, float halfLW)
	{
		const Vec3 dir = p1 - p0;
		const double lenXZ = Math::Sqrt(dir.x * dir.x + dir.z * dir.z);
		if (lenXZ < 1e-6) return;
		const Vec3 perp{ dir.z / lenXZ, 0.0, -dir.x / lenXZ };
		const uint32 base = static_cast<uint32>(vertices.size());
		vertices << makeVert(p0 - perp * halfLW, 0.0f, 0.0f);
		vertices << makeVert(p0 + perp * halfLW, 1.0f, 0.0f);
		vertices << makeVert(p1 - perp * halfLW, 0.0f, 1.0f);
		vertices << makeVert(p1 + perp * halfLW, 1.0f, 1.0f);
		appendQuad(indices, base, base + 1, base + 2, base + 3);
	}

	/// @brief レーン領域内に縞塗り（ゼブラ/導流帯）を敷き詰める
	/// @param chevron true: 「く」の字（導流帯） / false: 斜線（立入り禁止部分）
	void appendLaneStripes(
		Array<Vertex3D>&        vertices,
		Array<TriangleIndex32>& indices,
		const CubicBezier&      bez,
		const World&            world,
		float offA_L, float offA_R, float offB_L, float offB_R,
		float sStart, float sEnd,
		bool  chevron,
		float terrainLift = static_cast<float>(kRoadLineLift),
		bool  useElevation = false)
	{
		const float totalLen = bez.totalLength;
		if (totalLen <= 0.0f || sEnd <= sStart) return;

		constexpr float kSpacing = 2.0f;
		constexpr float kHalfLW  = 0.075f;

		auto offsetsAt = [&](float s, float& oL, float& oR)
		{
			const float t = Clamp(s / totalLen, 0.0f, 1.0f);
			oL = offA_L * (1.0f - t) + offB_L * t;
			oR = offA_R * (1.0f - t) + offB_R * t;
		};

		for (float s = sStart; s < sEnd; s += kSpacing)
		{
			float oL0, oR0;
			offsetsAt(s, oL0, oR0);
			const float width = Abs(oR0 - oL0);
			if (width < 0.2f) continue;

			const auto sl0 = makeSlice(bez, world, s, terrainLift, useElevation);
			const Vec3 pL  = sl0.center + sl0.right * oL0;
			const Vec3 pR  = sl0.center + sl0.right * oR0;

			if (chevron)
			{
				const float sApex = Min(s + width * 0.5f, totalLen);
				float oLa, oRa;
				offsetsAt(sApex, oLa, oRa);
				const auto slA = makeSlice(bez, world, sApex, terrainLift, useElevation);
				const Vec3 pC = slA.center + slA.right * ((oLa + oRa) * 0.5f);
				appendBar(vertices, indices, pL, pC, kHalfLW);
				appendBar(vertices, indices, pC, pR, kHalfLW);
			}
			else
			{
				const float sR = Min(s + width * 0.5f, totalLen);
				float oLb, oRb;
				offsetsAt(sR, oLb, oRb);
				const auto slR = makeSlice(bez, world, sR, terrainLift, useElevation);
				const Vec3 pRr = slR.center + slR.right * oRb;
				appendBar(vertices, indices, pL, pRr, kHalfLW);
			}
		}
	}

	LineStyle lineStyleFor(LineType lt)
	{
		switch (lt)
		{
		case LineType::SolidWhite:   return { ColorF{1,1,1}, 0.15f, 0, 0 };
		case LineType::DashedWhite:  return { ColorF{1,1,1}, 0.15f, 8, 12 };
		case LineType::SolidYellow:  return { ColorF{ 0.86, 0.74, 0.30 }, 0.16f, 0, 0 };
		case LineType::DoubleYellow: return { ColorF{ 0.86, 0.74, 0.30 }, 0.16f, 0, 0 };
		default: return {};
		}
	}

	EdgeLineInfo calcEdgeLineAt(const RoadNetwork& network, int nodeId, const World& world,
	                            const RoadEdge& edge, float offset)
	{
		const RoadNode* node = network.getNode(nodeId);
		const auto bez = network.getBezier(edge.id);
		if (!node || !bez) return { node ? node->position : Vec3{}, Vec3{0, 0, 1} };
		const bool isNodeA = (edge.nodeA == nodeId);
		const float capRad = isNodeA ? edge.cutoffA : edge.cutoffB;
		const float s = isNodeA
			? Clamp(capRad - 0.1f, 0.0f, bez->totalLength * 0.45f)
			: Clamp(bez->totalLength - capRad + 0.1f, bez->totalLength * 0.55f, bez->totalLength);
		const auto range = RoadGeometry::roadbedRangeAt(edge,s / Max(0.01f,bez->totalLength));
		if (range.valid && range.width() > 1.05f) { offset = Clamp(offset,range.left+0.525f,range.right-0.525f); }
		const Vec3 pos = bez->positionAt(s);
		const Vec3 rawTan = bez->tangentAt(s);
		const Vec3 right = tangentToRight(rawTan);
		const Vec3 tan = isNodeA ? -rawTan : rawTan;
		const double lineY = edge.usesDesignHeight()
			? pos.y + kRoadLineLift
			: world.sampleHeight(static_cast<float>(pos.x), static_cast<float>(pos.z)) + kRoadLineLift;
		return { Vec3{ pos.x, lineY, pos.z } + right * static_cast<double>(offset), tan };
	}

	/// @brief 2点間をベジェ曲線で結ぶ車線ライン
	/// @param tanFrom from 地点の進行方向（外向き）
	/// @param tanTo   to 地点の進行方向（外向き）
	void appendBezierLine(Array<RoadRenderer::LaneLineBatch>& out,
	                      const Vec3& from, const Vec3& to,
	                      const Vec3& tanFrom, const Vec3& tanTo,
	                      float lineWidth, const ColorF& color, [[maybe_unused]] const World& world, const MeshData* surface = nullptr)
	{
		const Vec3 diff = to - from;
		if (diff.lengthSq() < 0.01) return;
		const double dist = diff.length();
		const double ctrlLen = dist * 0.4;

		// XZ 平面でベジェ曲線を構築（Y は線形補間）
		// tanFrom = from の出発方向（ノード内向き）
		// tanTo   = to の出発方向（ノード内向き）
		const Vec3 cp1{ from.x + tanFrom.x * ctrlLen, 0.0, from.z + tanFrom.z * ctrlLen };
		const Vec3 cp2{ to.x   + tanTo.x   * ctrlLen, 0.0, to.z   + tanTo.z   * ctrlLen };

		const int divisions = Clamp(static_cast<int>(Ceil(dist / 1.5)),12,64);
		const double hw = static_cast<double>(lineWidth * 0.5f);
		MeshData md;

		for (int k = 0; k <= divisions; ++k)
		{
			const double t = k / static_cast<double>(divisions);
			const double u = 1.0 - t;
			// cubic bezier: B(t) = (1-t)^3*P0 + 3(1-t)^2*t*P1 + 3(1-t)*t^2*P2 + t^3*P3
			const Vec3 pos = from * (u * u * u) + cp1 * (3 * u * u * t)
			               + cp2 * (3 * u * t * t) + to * (t * t * t);
			// tangent: B'(t)
			const Vec3 tan = (cp1 - from) * (3 * u * u) + (cp2 - cp1) * (6 * u * t)
			               + (to - cp2) * (3 * t * t);
			const Vec3 right = tangentToRight(tan.lengthSq() > 0.001 ? tan.normalized() : diff.normalized());
			// Y は from → to の線形補間（路面高さを維持）
			const Vec3 p{ pos.x, from.y + (to.y - from.y) * t, pos.z };
			const uint32 base = static_cast<uint32>(md.vertices.size());
			md.vertices << makeVert(p - right * hw, 0, static_cast<float>(t));
			md.vertices << makeVert(p + right * hw, 1, static_cast<float>(t));

			if (k > 0)
				appendQuad(md.indices, base - 2, base - 1, base, base + 1);
		}
		if (surface) { md = RoadGeometry::projectMarking(md,*surface); }
		if (!md.vertices.isEmpty())
			out << RoadRenderer::LaneLineBatch{ color.removeSRGBCurve(), Mesh{ md } };
	}

	/// @brief 後方互換: 直線版（tangent を自動計算）
	void appendStraightLine(Array<RoadRenderer::LaneLineBatch>& out,
	                        const Vec3& from, const Vec3& to,
	                        float lineWidth, const ColorF& color)
	{
		const Vec3 dir = to - from;
		if (dir.lengthSq() < 0.01) return;
		const Vec3 normDir = dir.normalized();
		const Vec3 r  = tangentToRight(normDir);
		const double hw = static_cast<double>(lineWidth * 0.5f);

		constexpr int kDiv = 4;
		MeshData md;
		for (int k = 0; k < kDiv; ++k)
		{
			const float t0 = k / static_cast<float>(kDiv);
			const float t1 = (k + 1) / static_cast<float>(kDiv);
			const Vec3 p0 = from + dir * static_cast<double>(t0);
			const Vec3 p1 = from + dir * static_cast<double>(t1);
			const uint32 base = static_cast<uint32>(md.vertices.size());
			md.vertices << makeVert(p0 - r * hw, 0, 0);
			md.vertices << makeVert(p0 + r * hw, 1, 0);
			md.vertices << makeVert(p1 - r * hw, 0, 1);
			md.vertices << makeVert(p1 + r * hw, 1, 1);
			appendQuad(md.indices, base, base + 1, base + 2, base + 3);
		}
		if (!md.vertices.isEmpty())
			out << RoadRenderer::LaneLineBatch{ color.removeSRGBCurve(), Mesh{ md } };
	}

}

Array<RoadRenderer::LaneLineBatch> RoadRenderer::buildLaneLineBatches(
	const RoadEdge& edge, const CubicBezier& bezier,
	const World& world,
	float marginA, float marginB) const
{
	if (edge.lanes.empty()) return {};
	StripRange range;
	if (!calcStripRange(bezier.totalLength, marginA, marginB, range)) return {};
	const float sStart = range.sStart;
	const float sEnd   = range.sEnd;

	Array<LaneLineBatch> batches;

	for (int i = 0; i < static_cast<int>(edge.lanes.size()); ++i)
	{
		const auto& lane = edge.lanes[i];

		// 右境界の線（lineRight が None でなければ描画）
		if (lane.lineRight != LineType::None)
		{
			const auto lineStyle = lineStyleFor(lane.lineRight);

			MeshData md;
			appendDashedStrip(md.vertices, md.indices,
			                  bezier, world,
			                  lane.offsetA_R, lane.offsetB_R, lineStyle.lineWidth * 0.5f,
			                  lineStyle.dashLen, lineStyle.gapLen,
			                  sStart, sEnd, static_cast<float>(kRoadLineLift), edge.usesDesignHeight());
			if (!md.vertices.isEmpty())
			{
				batches << LaneLineBatch{ lineStyle.color.removeSRGBCurve(), Mesh{ md } };
			}
		}

		// 左境界の線（lineLeft が None でなければ描画）
		if (lane.lineLeft != LineType::None)
		{
			const auto lineStyle = lineStyleFor(lane.lineLeft);

			MeshData md;
			appendDashedStrip(md.vertices, md.indices,
			                  bezier, world,
			                  lane.offsetA_L, lane.offsetB_L, lineStyle.lineWidth * 0.5f,
			                  lineStyle.dashLen, lineStyle.gapLen,
			                  sStart, sEnd, static_cast<float>(kRoadLineLift), edge.usesDesignHeight());
			if (!md.vertices.isEmpty())
			{
				batches << LaneLineBatch{ lineStyle.color.removeSRGBCurve(), Mesh{ md } };
			}
		}

		// 路面縞塗り（立入り禁止部分=斜線 / 導流帯=くの字）
		if (lane.type == LaneType::KeepOut || lane.type == LaneType::TrafficIsland)
		{
			MeshData md;
			appendLaneStripes(md.vertices, md.indices,
			                  bezier, world,
			                  lane.offsetA_L, lane.offsetA_R,
			                  lane.offsetB_L, lane.offsetB_R,
			                  sStart, sEnd,
			                  lane.type == LaneType::TrafficIsland,
			                  static_cast<float>(kRoadLineLift), edge.usesDesignHeight());
			if (!md.vertices.isEmpty())
			{
				batches << LaneLineBatch{ ColorF{ 1, 1, 1 }.removeSRGBCurve(), Mesh{ md } };
			}
		}
	}

	return batches;
}


namespace
{
	struct RoadMarkingRenderPlan
	{
		Array<RoadMarkingPlacement> laneLines;
		Array<RoadMarkingPlacement> stopLines;
		Array<RoadMarkingPlacement> crosswalks;
		Array<RoadMarkingPlacement> directionArrows;
		Array<RoadMarkingPlacement> keepOuts;
		Array<RoadMarkingPlacement> zebraZones;
	};

	RoadMarkingRenderPlan makeRoadMarkingRenderPlan(const Array<RoadMarkingPlacement>& placements)
	{
		RoadMarkingRenderPlan plan;
		for (const RoadMarkingPlacement& marking : placements)
		{
			if (marking.overrideMode == RoadMarkingOverrideMode::Suppress) continue;
			switch (marking.kind)
			{
			case RoadMarkingKind::LaneLine:
				plan.laneLines << marking;
				break;
			case RoadMarkingKind::StopLine:
				plan.stopLines << marking;
				break;
			case RoadMarkingKind::Crosswalk:
				plan.crosswalks << marking;
				break;
			case RoadMarkingKind::DirectionArrow:
				plan.directionArrows << marking;
				break;
			case RoadMarkingKind::KeepOut:
				plan.keepOuts << marking;
				break;
			case RoadMarkingKind::ZebraZone:
				plan.zebraZones << marking;
				break;
			}
		}
		return plan;
	}
}

static Array<RoadRenderer::LaneLineBatch> buildLaneLineBatchesFromPlacements(
	const RoadNetwork& network, const RoadEdge& edge, const CubicBezier& bezier, const World& world,
	float marginA, float marginB, const Array<RoadMarkingPlacement>& placements);
static Array<RoadRenderer::LaneLineBatch> buildCrosswalkBatchesFromPlacements(
	const RoadNetwork& network, const Array<RoadMarkingPlacement>& placements, const World& world);

static Array<RoadRenderer::LaneLineBatch> buildLaneLineBatchesFromPlacements(
	const RoadNetwork& network, const RoadEdge& edge, const CubicBezier& bezier, const World& world,
	float marginA, float marginB, const Array<RoadMarkingPlacement>& placements)
{
	StripRange edgeRange;
	if (!calcStripRange(bezier.totalLength, marginA, marginB, edgeRange)) return {};

	Array<RoadRenderer::LaneLineBatch> batches;
	const MeshData surface = RoadGeometry::roadbedSurface(edge, bezier, world);
	const MeshData overlap = adjacentPavement(network,edge,world);
	for (const RoadMarkingPlacement& marking : placements)
	{
		if (marking.kind != RoadMarkingKind::LaneLine || marking.scope != RoadMarkingScope::Edge) continue;
		const float sStart = Max(edgeRange.sStart, marking.arcOffset);
		const float sEnd = Min(edgeRange.sEnd, marking.arcOffset + marking.length);
		if (sEnd - sStart <= 0.1f) continue;

		const float tStart = Clamp(sStart / Max(1.0f, bezier.totalLength), 0.0f, 1.0f);
		const float tEnd = Clamp(sEnd / Max(1.0f, bezier.totalLength), 0.0f, 1.0f);
		const RoadGeometry::LateralRange startRange = RoadGeometry::roadbedRangeAt(edge, tStart);
		const RoadGeometry::LateralRange endRange = RoadGeometry::roadbedRangeAt(edge, tEnd);
		if (!startRange.valid || !endRange.valid) continue;

		const float lineWidth = Clamp(marking.width, 0.12f, 0.32f);
		const float halfLineWidth = lineWidth * 0.5f;
		const float startMin = startRange.left + halfLineWidth + 0.45f;
		const float startMax = startRange.right - halfLineWidth - 0.45f;
		const float endMin = endRange.left + halfLineWidth + 0.45f;
		const float endMax = endRange.right - halfLineWidth - 0.45f;
		if (startMin > startMax || endMin > endMax) continue;

		const float offsetA = Clamp(marking.lateralCenter, startMin, startMax);
		const float offsetB = Clamp(marking.lateralCenter, endMin, endMax);
		MeshData md;
		LineType type = LineType::SolidWhite;
		float nearest = Math::Inf;
		for (const auto& lane : edge.lanes)
		{
			for (const auto boundary : { std::pair<float,LineType>{lane.offsetA_L,lane.lineLeft}, {lane.offsetA_R,lane.lineRight} })
			{
				const float distance = Abs(marking.lateralCenter-boundary.first);
				if (distance < nearest) { nearest = distance; type = boundary.second; }
			}
		}
		const auto style = lineStyleFor(type);
		appendDashedStrip(md.vertices, md.indices, bezier, world, offsetA, offsetB, halfLineWidth,
			style.dashLen, style.gapLen, sStart, sEnd, static_cast<float>(kRoadLineLift), edge.usesDesignHeight());
		md = RoadGeometry::projectMarking(md, surface);
		if (!overlap.indices.isEmpty()) { md = RoadGeometry::excludeSurface(md,overlap); }
		if (!md.vertices.isEmpty())
		{
			const ColorF lineColor = style.color.removeSRGBCurve();
			batches << RoadRenderer::LaneLineBatch{ lineColor, Mesh{ md } };
		}
	}
	return batches;
}
Array<RoadMarkingPlacement> RoadRenderer::collectEdgeRoadMarkings(const RoadNetwork& network, const RoadEdge& edge) const
{
	return RoadMarkingGenerator::collectEdge(network, edge);
}

Array<RoadMarkingPlacement> RoadRenderer::collectNodeRoadMarkings(const RoadNetwork& network, int nodeId, bool isClose) const
{
	return RoadMarkingGenerator::collectNode(network, nodeId, isClose);
}

Array<RoadRenderer::LaneLineBatch> RoadRenderer::buildEdgeRoadMarkingsFromPlacements(
	const RoadNetwork& network, const RoadEdge& edge, const CubicBezier& bezier,
	const World& world, float marginA, float marginB,
	const Array<RoadMarkingPlacement>& placements) const
{
	(void)network;
	const RoadMarkingRenderPlan plan = makeRoadMarkingRenderPlan(placements);
	Array<LaneLineBatch> batches;
	if (!plan.laneLines.isEmpty())
	{
		batches.append(buildLaneLineBatchesFromPlacements(network, edge, bezier, world, marginA, marginB, plan.laneLines));
	}
	if (!plan.keepOuts.isEmpty() || !plan.zebraZones.isEmpty())
	{
		batches.append(buildLaneLineBatches(edge, bezier, world, marginA, marginB));
	}
	if (!plan.crosswalks.isEmpty())
	{
		batches.append(buildCrosswalkBatchesFromPlacements(network, plan.crosswalks, world));
	}
	return batches;
}

Array<RoadRenderer::LaneLineBatch> RoadRenderer::buildNodeRoadMarkingsFromPlacements(
	const RoadNetwork& network, int nodeId, const World& world,
	const Array<RoadMarkingPlacement>& placements) const
{
	const RoadMarkingRenderPlan plan = makeRoadMarkingRenderPlan(placements);
	Array<LaneLineBatch> batches;
	if (!plan.laneLines.isEmpty())
	{
		batches.append(buildNodeCapLaneLines(network, nodeId, world));
	}
	if (!plan.stopLines.isEmpty())
	{
		batches.append(buildStopLineBatches(network, nodeId, world));
	}
	if (!plan.crosswalks.isEmpty())
	{
		batches.append(buildCrosswalkBatchesFromPlacements(network, plan.crosswalks, world));
	}
	if (!plan.directionArrows.isEmpty())
	{
		batches.append(buildLaneArrowMeshes(network, nodeId, world));
	}
	return batches;
}

Array<RoadRenderer::LaneLineBatch> RoadRenderer::buildEdgeRoadMarkingBatches(
	const RoadNetwork& network, const RoadEdge& edge, const CubicBezier& bezier,
	const World& world, float marginA, float marginB) const
{
	const Array<RoadMarkingPlacement> placements = collectEdgeRoadMarkings(network, edge);
	return buildEdgeRoadMarkingsFromPlacements(network, edge, bezier, world, marginA, marginB, placements);
}

Array<RoadRenderer::LaneLineBatch> RoadRenderer::buildNodeRoadMarkingBatches(
	const RoadNetwork& network, int nodeId, const World& world, bool isClose) const
{
	const Array<RoadMarkingPlacement> placements = collectNodeRoadMarkings(network, nodeId, isClose);
	return buildNodeRoadMarkingsFromPlacements(network, nodeId, world, placements);
}

Array<RoadRenderer::LaneLineBatch> RoadRenderer::buildNodeCapLaneLines(
	const RoadNetwork& network, int nodeId, const World& world) const
{
	const RoadNode* node = network.getNode(nodeId);
	if (!node || node->attachments.size() < 2) return {};

	// Intersection では車線区画線を描画しない（Joint / Diverge では描画）
	if (node->type == NodeType::Intersection && node->attachments.size() > 2)
	{
		return {};
	}

	Array<LaneLineBatch> batches;

	// === Joint (Blend) — 別メソッドで処理 ===
	if (node->attachments.size() == 2 && node->transition == NodeTransition::Blend)
	{
		return buildJointBlendLaneLines(network, nodeId, *node, world);
	}

	// calcEdgeLine ヘルパー（Diverge パスで使用）
	auto calcEdgeLine = [&](const RoadEdge& edge, float offset) -> EdgeLineInfo
	{
		return calcEdgeLineAt(network, nodeId, world, edge, offset);
	};

	// === Diverge — 各エッジの車線境界からノード中心へベジェ曲線 ===
	const bool anyElev = network.nodeUsesDesignHeight(nodeId);
	const double ctrY = anyElev
		? node->position.y + kRoadLineLift
		: world.sampleHeight(static_cast<float>(node->position.x),
		                      static_cast<float>(node->position.z)) + kRoadLineLift;
	const Vec3 centerPos{ node->position.x, ctrY, node->position.z };

	for (const auto& att : node->attachments)
	{
		const RoadEdge* edge = network.getEdge(att.edgeId);
		if (!edge) continue;

		for (size_t i = 0; i < edge->lanes.size(); ++i)
		{
			const auto& lane = edge->lanes[i];

			// lineRight
			if (lane.lineRight != LineType::None)
			{
				const auto lineStyle = lineStyleFor(lane.lineRight);
				const auto info = calcEdgeLine(*edge, lane.offsetA_R);
				appendBezierLine(batches, info.pos, centerPos,
				                 info.tangent, info.tangent, lineStyle.lineWidth, lineStyle.color, world);
			}

			// lineLeft
			if (lane.lineLeft != LineType::None)
			{
				const auto lineStyle = lineStyleFor(lane.lineLeft);
				const auto info = calcEdgeLine(*edge, lane.offsetA_L);
				appendBezierLine(batches, info.pos, centerPos,
				                 info.tangent, info.tangent, lineStyle.lineWidth, lineStyle.color, world);
			}
		}
	}

	return batches;
}

// ─────────────────────────────────────────────────────────────────────────────
// ノード境界の停止線生成
// ─────────────────────────────────────────────────────────────────────────────

Array<RoadRenderer::LaneLineBatch> RoadRenderer::buildStopLineBatches(
	const RoadNetwork& network, int nodeId, const World& world) const
{
	const RoadNode* node = network.getNode(nodeId);
	if (!node) return {};

	Array<LaneLineBatch> batches;
	for (const auto& att : node->attachments)
	{
		if (att.control != TrafficControl::Stop && att.control != TrafficControl::Signal)
			continue;

		const RoadEdge* edge = network.getEdge(att.edgeId);
		if (!edge || !edge->isRoadbedBuilt()) continue;
		const auto bez = network.getBezier(att.edgeId);
		if (!bez) continue;

		const bool isNodeA = (edge->nodeA == nodeId);
		const float cutoff = isNodeA ? edge->cutoffA : edge->cutoffB;
		const float cutoffArc = isNodeA ? cutoff : (bez->totalLength - cutoff);
		const Vec3 pos = bez->positionAt(cutoffArc);
		const Vec3 tan = bez->tangentAt(cutoffArc);
		const Vec3 right = tangentToRight(tan);

		// Entry 車線（ノードに進入する方向）のオフセット範囲を求める
		float entryMin = 1e9f, entryMax = -1e9f;
		bool hasEntry = false;
		for (const auto& lane : edge->lanes)
		{
			if (lane.op != OpState::Open && lane.op != OpState::Provisional) continue;
			const bool enters =
				(lane.dir == LaneDir::Forward  && edge->nodeB == nodeId) ||
				(lane.dir == LaneDir::Backward && edge->nodeA == nodeId);
			if (!enters) continue;

			const float oL = isNodeA ? lane.offsetA_L : lane.offsetB_L;
			const float oR = isNodeA ? lane.offsetA_R : lane.offsetB_R;
			entryMin = Min(entryMin, Min(oL, oR));
			entryMax = Max(entryMax, Max(oL, oR));
			hasEntry = true;
		}
		if (!hasEntry) continue;

		const double lineY = edge->usesDesignHeight()
			? pos.y + kRoadLineLift
			: world.sampleHeight(static_cast<float>(pos.x), static_cast<float>(pos.z)) + kRoadLineLift;

		const Vec3 p0{ pos.x + right.x * entryMin, lineY, pos.z + right.z * entryMin };
		const Vec3 p1{ pos.x + right.x * entryMax, lineY, pos.z + right.z * entryMax };
		appendStraightLine(batches, p0, p1, 0.3f, ColorF{ 1.0, 1.0, 1.0 });
	}
	return batches;
}

static Array<RoadRenderer::LaneLineBatch> buildCrosswalkBatchesFromPlacements(
	const RoadNetwork& network, const Array<RoadMarkingPlacement>& placements, const World& world)
{
	Array<RoadRenderer::LaneLineBatch> batches;
	for (const RoadMarkingPlacement& marking : placements)
	{
		if (marking.kind != RoadMarkingKind::Crosswalk || marking.edgeId < 0) continue;
		const RoadEdge* edge = network.getEdge(marking.edgeId);
		if (!edge || !edge->isRoadbedBuilt()) continue;
		const auto bezier = network.getBezier(marking.edgeId);
		if (!bezier || bezier->totalLength <= 1.0f) continue;
		const float arc = Clamp(marking.arcOffset, 0.0f, bezier->totalLength);
		const Vec3 pos = bezier->positionAt(arc);
		Vec3 tan = bezier->tangentAt(arc);
		Vec2 tangent{ static_cast<float>(tan.x), static_cast<float>(tan.z) };
		if (marking.angleOffset != 0.0f)
		{
			const float cosA = Math::Cos(marking.angleOffset);
			const float sinA = Math::Sin(marking.angleOffset);
			tangent = Vec2{ tangent.x * cosA - tangent.y * sinA, tangent.x * sinA + tangent.y * cosA };
		}
		if (tangent.lengthSq() <= 1e-8f) continue;
		tangent.normalize();
		const Vec2 right{ tangent.y, -tangent.x };
		const RoadGeometry::LateralRange roadbedRange = RoadGeometry::roadbedRangeAt(*edge, arc / Max(1.0f, bezier->totalLength));
		if (!roadbedRange.valid) continue;
		const float halfWidth = Max(0.3f, Min(marking.width, roadbedRange.width() - 0.70f) * 0.5f);
		const float minCenter = roadbedRange.left + halfWidth + 0.35f;
		const float maxCenter = roadbedRange.right - halfWidth - 0.35f;
		const float fallbackCenter = (roadbedRange.left + roadbedRange.right) * 0.5f;
		const float centerOffset = (minCenter <= maxCenter) ? Clamp(marking.lateralCenter, minCenter, maxCenter) : fallbackCenter;
		const float zebraDepth = Clamp(marking.length, 2.4f, 4.6f);
		// Zebra bars run along traffic, repeating across the road.
		constexpr float stripeWidth = 0.45f;
		constexpr float stripePitch = 0.90f;
		const int stripeCount = Max(1, static_cast<int>(Floor((halfWidth * 2.0f + stripePitch - stripeWidth) / stripePitch)));
		MeshData md;
		for (int stripe = 0; stripe < stripeCount; ++stripe)
		{
			const double lateral = centerOffset + (stripe - (stripeCount - 1) * 0.5) * stripePitch;
			const Vec3 center{ pos.x + right.x * lateral, pos.y, pos.z + right.y * lateral };
			const Vec3 p0{ center.x - tangent.x * zebraDepth * 0.5, center.y, center.z - tangent.y * zebraDepth * 0.5 };
			const Vec3 p1{ center.x + tangent.x * zebraDepth * 0.5, center.y, center.z + tangent.y * zebraDepth * 0.5 };
			appendBar(md.vertices, md.indices, p0, p1, stripeWidth * 0.5f);
		}
		md = RoadGeometry::projectMarking(md, RoadGeometry::roadbedSurface(*edge, *bezier, world));
		if (!md.vertices.isEmpty()) batches << RoadRenderer::LaneLineBatch{ ColorF{ 1.0, 1.0, 1.0 }, Mesh{ md } };
	}
	return batches;
}
// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// Joint (Blend) ノードの車線区画線
// ─────────────────────────────────────────────────────────────────────────────

Array<RoadRenderer::LaneLineBatch> RoadRenderer::buildJointBlendLaneLines(
	const RoadNetwork& network, int nodeId, const RoadNode& node, const World& world) const
{
	auto calcEdgeLine = [&](const RoadEdge& edge, float offset) -> EdgeLineInfo
	{
		return calcEdgeLineAt(network, nodeId, world, edge, offset);
	};

	const RoadEdge* edgeA = network.getEdge(node.attachments[0].edgeId);
	const RoadEdge* edgeB = network.getEdge(node.attachments[1].edgeId);
	if (!edgeA || !edgeB) return {};

	const auto layout = JunctionGeometry::build(network,nodeId,true,&world);
	MeshData surface = layout.asphalt;
	for (auto& vertex : surface.vertices)
	{
		if (!layout.elevated) { vertex.pos.y = world.sampleHeight(vertex.pos.x,vertex.pos.z) + static_cast<float>(kRoadSurfaceLift); }
	}
	Array<LaneLineBatch> batches;

	// 共通フレームに正規化された車線情報
	struct LaneAtNode
	{
		Vec3     leftPos,  leftTan;
		Vec3     rightPos, rightTan;
		LineType lineLeft;
		LineType lineRight;
		float    centerCommon;
		int      rawFlow;
		int      groupId;
	};

	// Transport the cross section through the bend using edge orientation.
	// Projecting onto one world-space right vector collapses the other arm at 90 degrees.
	const bool referenceStartsAtNode = edgeA->nodeA == nodeId;

	auto buildInfos = [&](const RoadEdge& edge, bool isNodeAEdge) -> Array<LaneAtNode>
	{
		Array<LaneAtNode> out;
		out.reserve(edge.lanes.size());
		for (const auto& L : edge.lanes)
		{
			const float oL = isNodeAEdge ? L.offsetA_L : L.offsetB_L;
			const float oR = isNodeAEdge ? L.offsetA_R : L.offsetB_R;
			const auto  infoL = calcEdgeLine(edge, oL);
			const auto  infoR = calcEdgeLine(edge, oR);
			const bool swap = edge.id != edgeA->id && isNodeAEdge == referenceStartsAtNode;

			LaneAtNode info;
			if (swap)
			{
				info.leftPos  = infoR.pos;  info.leftTan  = infoR.tangent;
				info.rightPos = infoL.pos;  info.rightTan = infoL.tangent;
				info.lineLeft = L.lineRight;
				info.lineRight = L.lineLeft;
			}
			else
			{
				info.leftPos  = infoL.pos;  info.leftTan  = infoL.tangent;
				info.rightPos = infoR.pos;  info.rightTan = infoR.tangent;
				info.lineLeft = L.lineLeft;
				info.lineRight = L.lineRight;
			}
			info.centerCommon = (oL+oR)*0.5f*(swap ? -1.0f : 1.0f);

			const bool outgoing = (L.dir == LaneDir::Forward) == isNodeAEdge;
			info.rawFlow = (edge.id == edgeA->id ? outgoing : !outgoing) ? 0 : 1;
			info.groupId = -1;

			out << info;
		}
		return out;
	};

	const bool isNodeA_A = (edgeA->nodeA == nodeId);
	const bool isNodeA_B = (edgeB->nodeA == nodeId);

	auto infosA = buildInfos(*edgeA, isNodeA_A);
	auto infosB = buildInfos(*edgeB, isNodeA_B);

	// groupId の付与: centerCommon 昇順に並べ、rawFlow が変わるたびに新グループ
	auto assignGroups = [](Array<LaneAtNode>& infos) -> int
	{
		if (infos.isEmpty()) return 0;
		Array<int> ids(infos.size());
		for (int i = 0; i < static_cast<int>(infos.size()); ++i) ids[i] = i;
		ids.sort_by([&](int a, int b)
		{
			return infos[a].centerCommon < infos[b].centerCommon;
		});
		int gid = 0;
		int prevFlow = infos[ids[0]].rawFlow;
		infos[ids[0]].groupId = gid;
		for (int k = 1; k < static_cast<int>(ids.size()); ++k)
		{
			const int i = ids[k];
			if (infos[i].rawFlow != prevFlow) { ++gid; prevFlow = infos[i].rawFlow; }
			infos[i].groupId = gid;
		}
		return gid + 1;
	};

	const int numGroupsA = assignGroups(infosA);
	const int numGroupsB = assignGroups(infosB);
	const int numGroups  = Max(numGroupsA, numGroupsB);

	// 未ペア車線テーパー: 左右両方を targetPos に収束
	auto drawTaper = [&](const LaneAtNode& l, const Vec3& targetPos, const Vec3& targetTan)
	{
		if (l.lineLeft != LineType::None)
		{
			const auto lineStyle = lineStyleFor(l.lineLeft);
			appendBezierLine(batches, l.leftPos, targetPos, l.leftTan, targetTan,
			                 lineStyle.lineWidth, lineStyle.color, world, &surface);
		}
		if (l.lineRight != LineType::None)
		{
			const auto lineStyle = lineStyleFor(l.lineRight);
			appendBezierLine(batches, l.rightPos, targetPos, l.rightTan, targetTan,
			                 lineStyle.lineWidth, lineStyle.color, world, &surface);
		}
	};

	// グループ境界を越えた参照のために、ペアリングと未ペアをグローバル追跡
	Array<int> globalPairA(infosA.size(), -1);
	Array<int> globalPairB(infosB.size(), -1);
	Array<int> unpairedA, unpairedB;

	for (int group = 0; group < numGroups; ++group)
	{
		Array<int> idsA, idsB;
		for (int i = 0; i < static_cast<int>(infosA.size()); ++i)
			if (infosA[i].groupId == group) idsA << i;
		for (int j = 0; j < static_cast<int>(infosB.size()); ++j)
			if (infosB[j].groupId == group) idsB << j;

		const auto sortInsideOut = [&](const Array<LaneAtNode>& infos)
		{
			return [&infos](int a, int b)
			{
				return Math::Abs(infos[a].centerCommon) < Math::Abs(infos[b].centerCommon);
			};
		};
		idsA.sort_by(sortInsideOut(infosA));
		idsB.sort_by(sortInsideOut(infosB));

		Array<int>  pairIdx(idsA.size(), -1);
		Array<bool> usedB(idsB.size(), false);
		for (int k = 0; k < static_cast<int>(idsA.size()); ++k)
		{
			const float ca = infosA[idsA[k]].centerCommon;
			float best = 1e9f; int bestM = -1;
			for (int m = 0; m < static_cast<int>(idsB.size()); ++m)
			{
				if (usedB[m]) continue;
				const float d = Math::Abs(ca - infosB[idsB[m]].centerCommon);
				if (d < best) { best = d; bestM = m; }
			}
			if (bestM >= 0) { pairIdx[k] = bestM; usedB[bestM] = true; }
		}

		for (int k = 0; k < static_cast<int>(idsA.size()); ++k)
		{
			if (pairIdx[k] < 0) continue;
			const auto& lA = infosA[idsA[k]];
			const auto& lB = infosB[idsB[pairIdx[k]]];
			const LineType lineL = (lA.lineLeft != LineType::None && lB.lineLeft != LineType::None) ? lA.lineLeft : LineType::None;
			const LineType lineR = (lA.lineRight != LineType::None && lB.lineRight != LineType::None) ? lA.lineRight : LineType::None;
			if (lineL != LineType::None)
			{
				const auto lineStyle = lineStyleFor(lineL);
				appendBezierLine(batches, lA.leftPos, lB.leftPos, lA.leftTan, lB.leftTan,
				                 lineStyle.lineWidth, lineStyle.color, world, &surface);
			}
			if (lineR != LineType::None)
			{
				const auto lineStyle = lineStyleFor(lineR);
				appendBezierLine(batches, lA.rightPos, lB.rightPos, lA.rightTan, lB.rightTan,
				                 lineStyle.lineWidth, lineStyle.color, world, &surface);
			}
		}

		for (int k = 0; k < static_cast<int>(idsA.size()); ++k)
			if (pairIdx[k] >= 0) globalPairA[idsA[k]] = idsB[pairIdx[k]];
		for (int m = 0; m < static_cast<int>(idsB.size()); ++m)
			if (usedB[m])
			{
				for (int k = 0; k < static_cast<int>(idsA.size()); ++k)
					if (pairIdx[k] == m) { globalPairB[idsB[m]] = idsA[k]; break; }
			}

		for (int k = 0; k < static_cast<int>(idsA.size()); ++k)
			if (pairIdx[k] < 0) unpairedA << idsA[k];
		for (int m = 0; m < static_cast<int>(idsB.size()); ++m)
			if (!usedB[m]) unpairedB << idsB[m];
	}

	auto findInnerNeighbor = [](const Array<LaneAtNode>& infos, int xIdx) -> int
	{
		const float xc = infos[xIdx].centerCommon;
		const float xMag = Math::Abs(xc);
		const bool xPositive = (xc >= 0.0f);
		int bestIdx = -1;
		float bestMag = -1.0f;
		for (int i = 0; i < static_cast<int>(infos.size()); ++i)
		{
			if (i == xIdx) continue;
			const float c = infos[i].centerCommon;
			if ((c >= 0.0f) != xPositive) continue;
			const float mag = Math::Abs(c);
			if (mag >= xMag) continue;
			if (mag > bestMag) { bestMag = mag; bestIdx = i; }
		}
		return bestIdx;
	};

	auto computeTaperTarget = [&](int xIdx, bool fromIsA)
		-> Optional<std::pair<Vec3, Vec3>>
	{
		const auto& infosFrom = fromIsA ? infosA : infosB;
		const auto& infosTo   = fromIsA ? infosB : infosA;
		const auto& globalPairFrom = fromIsA ? globalPairA : globalPairB;

		const int innerIdx = findInnerNeighbor(infosFrom, xIdx);
		if (innerIdx < 0) return none;
		const int pairedIdx = globalPairFrom[innerIdx];
		if (pairedIdx < 0) return none;
		const LaneAtNode& Y  = infosFrom[innerIdx];
		const LaneAtNode& pY = infosTo[pairedIdx];
		const bool outerIsRight = (Y.centerCommon >= 0.0f);
		if ((outerIsRight ? pY.lineRight : pY.lineLeft)==LineType::None) { return none; }
		return std::make_pair(
			outerIsRight ? pY.rightPos : pY.leftPos,
			outerIsRight ? pY.rightTan : pY.leftTan);
	};

	for (int xIdx : unpairedA)
	{
		if (const auto target=computeTaperTarget(xIdx,true)) { drawTaper(infosA[xIdx],target->first,target->second); }
	}
	for (int xIdx : unpairedB)
	{
		if (const auto target=computeTaperTarget(xIdx,false)) { drawTaper(infosB[xIdx],target->first,target->second); }
	}

	return batches;
}

Array<RoadRenderer::LaneLineBatch> RoadRenderer::buildLaneArrowMeshes(
	const RoadNetwork& network, int nodeId, const World& world) const
{
	const RoadNode* node = network.getNode(nodeId);
	if (!node) return {};
	// 交差点（3本以上のエッジが集まるノード）のみ矢印を表示
	if (node->attachments.size() < 3) return {};

	Array<LaneLineBatch> batches;

	for (const auto& att : node->attachments)
	{
		const RoadEdge* edge = network.getEdge(att.edgeId);
		if (!edge || !edge->isRoadbedBuilt()) { continue; }
		const auto bez = network.getBezier(att.edgeId);
		if (!bez) { continue; }

		const bool isAtA = (edge->nodeA == nodeId);
		const float cutoff = isAtA ? edge->cutoffA : edge->cutoffB;
		// 矢印中心の弧長位置: ノード境界から内側へ kArrowOffset
		const float arcCenter = isAtA
			? (cutoff + static_cast<float>(RoadArrow::kArrowOffsetFromNode_m))
			: (bez->totalLength - cutoff - static_cast<float>(RoadArrow::kArrowOffsetFromNode_m));
		// 弧長範囲外なら矢印を出さない（短いエッジ）
		if (arcCenter < cutoff || arcCenter > bez->totalLength - cutoff)
		{
			continue;
		}
		// kArrowLength_m 分のスペースが取れるかチェック
		const float halfLen = static_cast<float>(RoadArrow::kArrowLength_m) * 0.5f;
		if (arcCenter - halfLen < cutoff || arcCenter + halfLen > bez->totalLength - cutoff)
		{
			continue;
		}

		for (int li = 0; li < static_cast<int>(edge->lanes.size()); ++li)
		{
			const Lane& lane = edge->lanes[li];
			if (lane.op != OpState::Open && lane.op != OpState::Provisional) { continue; }

			// このノードへの entry レーンか
			const bool entersHere =
				(lane.dir == LaneDir::Forward  && edge->nodeB == nodeId) ||
				(lane.dir == LaneDir::Backward && edge->nodeA == nodeId);
			if (!entersHere) { continue; }

			// 矢印種別を推論
			const RoadArrowType atype = RoadArrow::InferType(network, edge->id, li, nodeId);
			if (atype == RoadArrowType::None) { continue; }

			// 配置位置・向きを計算（レーン中心線）
			const Vec3 centerPos = bez->positionAt(arcCenter);
			const Vec3 rawTan = bez->tangentAt(arcCenter);
			const Vec3 right = tangentToRight(rawTan);
			// 進行方向単位ベクトル: Forward なら +tangent, Backward なら -tangent
			const Vec3 forward = (lane.dir == LaneDir::Forward) ? rawTan : -rawTan;
			const double fLen = Math::Sqrt(forward.x * forward.x + forward.z * forward.z);
			const Vec3 fwdN = (fLen > 1e-6)
				? Vec3{ forward.x / fLen, 0.0, forward.z / fLen }
				: Vec3{ 1.0, 0.0, 0.0 };
			// 進行方向に対する右ベクトル（forward が反転すれば right も反転）
			const Vec3 rightForLane = (lane.dir == LaneDir::Forward) ? right : -right;

			// レーン中心の横方向オフセット（A端/B端でテーパー補間）
			const float oL = isAtA ? lane.offsetA_L : lane.offsetB_L;
			const float oR = isAtA ? lane.offsetA_R : lane.offsetB_R;
			const float laneCenterOffset = (oL + oR) * 0.5f;

			const Vec3 anchor{
				centerPos.x + right.x * static_cast<double>(laneCenterOffset),
				0.0,
				centerPos.z + right.z * static_cast<double>(laneCenterOffset)
			};

			// ローカル座標 → ワールド座標変換してバッチ追加
			// Y: 各頂点の弧長位置での中心線 XZ で地形をサンプリング。
			//    頂点の実 XZ で地形をサンプルすると傾斜地で外側車線が路面下に潜るため。
			//    高架橋は弧長ごとの bezier Y を使用。
			const auto addArrowBatch = [&](RoadArrowType t)
			{
				const MeshData* src = m_arrowMarkingRegistry.getMesh(t);
				if (!src || src->vertices.isEmpty()) return;
				MeshData md = *src;
				for (auto& v : md.vertices)
				{
					const double lx = v.pos.x;
					const double lz = v.pos.z;
					const Vec3 wp = anchor + fwdN * lx + rightForLane * lz;
					const float sForVertex = isAtA
						? static_cast<float>(arcCenter - lx)
						: static_cast<float>(arcCenter + lx);
					double vy;
					if (edge->usesDesignHeight())
					{
						vy = bez->positionAt(sForVertex).y + kRoadLineLift;
					}
					else
					{
						const Vec3 clPos = bez->positionAt(sForVertex);
						vy = world.sampleHeight(static_cast<float>(clPos.x), static_cast<float>(clPos.z)) + kRoadLineLift;
					}
					v.pos = Float3{ static_cast<float>(wp.x), static_cast<float>(vy), static_cast<float>(wp.z) };
				}
				batches << LaneLineBatch{ ColorF{ 1.0, 1.0, 1.0 }, Mesh{ md } };
			};

			addArrowBatch(atype);
		}
	}

	return batches;
}
