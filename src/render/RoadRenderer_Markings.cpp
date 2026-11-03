#include "RoadRenderer.hpp"
#include "RoadRenderDetail.hpp"
#include "MeshGeometry.hpp"
#include "../road/RoadArrow.hpp"
#include "../road/JunctionGeometry.hpp"
#include "../road/RoadMarkingGenerator.hpp"
#include "../road/NodeMarkings.hpp"

using namespace RoadRenderDetail;

static std::function<Vec3(const Vec3&, const Vec3&)> sharedEndLineAdjust(const RoadNetwork& network, const RoadEdge& edge, float lateral, bool atA);

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
		bool useElevation = false,
		const std::function<Vec3(const Vec3&, const Vec3&)>& adjust = {})
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
				Vec3          lc = sl.center + sl.right * offset;
				if (adjust) { lc = adjust(lc, sl.right); }
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
		// 本線と変速車線の境界は、通常の車線境界線より太く短い周期の破線（幅 45cm、線 3m・間隔 3m）。
		case LineType::DashedWhiteThick: return { ColorF{1,1,1}, 0.45f, 3, 3 };
		// 日本の黄色の区画線は橙に近い（写真の実測で sRGB 0.95, 0.65, 0.15 付近）。
		case LineType::SolidYellow:  return { ColorF{ 0.95, 0.64, 0.13 }, 0.16f, 0, 0 };
		case LineType::DoubleYellow: return { ColorF{ 0.95, 0.64, 0.13 }, 0.16f, 0, 0 };
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
	                      float lineWidth, const ColorF& color, [[maybe_unused]] const World& world, const MeshData* surface = nullptr, bool roundBend = false)
	{
		const Vec3 diff = to - from;
		if (diff.lengthSq() < 0.01) return;
		const double dist = diff.length();
		double ctrlLen = dist * 0.4;
		if (roundBend)
		{
			const Vec2 a{tanFrom.x,tanFrom.z}, b{tanTo.x,tanTo.z};
			if (a.lengthSq() > 1e-8 && b.lengthSq() > 1e-8)
			{
				const double cosine = Clamp(a.normalized().dot(b.normalized()),-1.0,1.0);
				// Circular-arc cubic handles match the two tangent-fitted road boundaries.
				ctrlLen = dist * (2.0 / 3.0) / (1.0 + Sqrt((1.0-cosine)*0.5));
			}
		}

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

	/// @brief ノード内の区画線の曲線。両端の切り口から、道路の向きに沿って出る 3 次ベジェ（高さは線形補間）。
	/// @brief 節点の端の横位置を、切り口（calcEdgeLineAt が測る位置）での同じ車線境界の横位置に直す。
	/// 端で車線を寄せる道路（ランプなど）は、切り口が長いと端の値のままでは舗装の外へずれる。
	float lateralAtCut(const RoadNetwork& network, int nodeId, const RoadEdge& edge, float lateral)
	{
		const auto bez = network.getBezier(edge.id);
		if (!bez || bez->totalLength <= 0) { return lateral; }
		const bool isNodeA = edge.nodeA == nodeId;
		const float capRad = isNodeA ? edge.cutoffA : edge.cutoffB;
		const float s = isNodeA
			? Clamp(capRad - 0.1f, 0.0f, bez->totalLength * 0.45f)
			: Clamp(bez->totalLength - capRad + 0.1f, bez->totalLength * 0.55f, bez->totalLength);
		const float ft = s / bez->totalLength;
		for (const auto& lane : edge.lanes)
		{
			const float endL = isNodeA ? lane.offsetA_L : lane.offsetB_L, endR = isNodeA ? lane.offsetA_R : lane.offsetB_R;
			if (Abs(endL - lateral) < .05f) { return Math::Lerp(lane.offsetA_L, lane.offsetB_L, ft); }
			if (Abs(endR - lateral) < .05f) { return Math::Lerp(lane.offsetA_R, lane.offsetB_R, ft); }
		}
		return lateral;
	}

	Optional<CubicBezier> nodeLineCurve(const RoadNetwork& network, int nodeId, const World& world, const NodeMarkingLine& line)
	{
		const RoadEdge* from = network.getEdge(line.from.edgeId);
		const RoadEdge* to = network.getEdge(line.to.edgeId);
		if (!from || !to) { return none; }
		const auto a = calcEdgeLineAt(network, nodeId, world, *from, lateralAtCut(network, nodeId, *from, line.from.lateral));
		const auto b = calcEdgeLineAt(network, nodeId, world, *to, lateralAtCut(network, nodeId, *to, line.to.lateral));
		const double distance = (b.pos - a.pos).length();
		if (distance < .1) { return none; }
		const double handle = distance * .4;
		const Vec3 c1 = a.pos + Vec3{ a.tangent.x, 0, a.tangent.z } * handle, c2 = b.pos + Vec3{ b.tangent.x, 0, b.tangent.z } * handle;
		return CubicBezier{ a.pos, Vec3{ c1.x, Math::Lerp(a.pos.y, b.pos.y, 1.0 / 3), c1.z }, Vec3{ c2.x, Math::Lerp(a.pos.y, b.pos.y, 2.0 / 3), c2.z }, b.pos };
	}

	/// @brief 節点の区画線を、つながる 2 本の道路に沿う点列で作る。切り口 → 節点 → 切り口の順。
	/// @details 各道路の上では、その車線境界の横位置（A 端と B 端の間を線形に移る）に沿う。2 本の道路が重なる区間で
	///   相手の車線へ食い込む線は、向かい合う線と中点の 1 本にそろえる（sharedEndLineAdjust）。
	Array<Vec3> nodeLinePath(const RoadNetwork& network, int nodeId, const World& world, const NodeMarkingLine& line)
	{
		Array<Vec3> points;
		for (const bool incoming : { true, false })
		{
			const NodeLineEnd& end = incoming ? line.from : line.to;
			const RoadEdge* edge = network.getEdge(end.edgeId);
			const auto bez = network.getBezier(end.edgeId);
			if (!edge || !bez || bez->totalLength <= 0) { return {}; }
			const bool atA = edge->nodeA == nodeId;
			const float length = bez->totalLength;
			const float cut = Clamp(atA ? edge->cutoffA : edge->cutoffB, 0.0f, length);
			// 境界の A 端・B 端の横位置。
			Optional<std::pair<float, float>> offsets;
			for (const auto& lane : edge->lanes)
			{
				if (Abs((atA ? lane.offsetA_L : lane.offsetB_L) - end.lateral) < .05f) { offsets = std::pair{ lane.offsetA_L, lane.offsetB_L }; break; }
				if (Abs((atA ? lane.offsetA_R : lane.offsetB_R) - end.lateral) < .05f) { offsets = std::pair{ lane.offsetA_R, lane.offsetB_R }; break; }
			}
			if (!offsets) { offsets = std::pair{ end.lateral, end.lateral }; }
			const auto adjust = sharedEndLineAdjust(network, *edge, end.lateral, atA);
			const int count = Max(2, static_cast<int>(Ceil(cut / 1.5f)));
			for (int i = 0; i <= count; ++i)
			{
				// 流入側は切り口から節点へ、流出側は節点から切り口へ。
				const float fromNode = cut * (incoming ? 1.0f - i / static_cast<float>(count) : i / static_cast<float>(count));
				if (!incoming && i == 0 && !points.isEmpty()) { continue; }
				const float s = atA ? fromNode : length - fromNode;
				const auto slice = makeSlice(*bez, world, s, kRoadLineLift, edge->usesDesignHeight());
				Vec3 p = slice.center + slice.right * static_cast<double>(Math::Lerp(offsets->first, offsets->second, s / length));
				if (adjust) { p = adjust(p, slice.right); }
				points << p;
			}
		}
		return points;
	}

	/// @brief 点列に沿う帯（実線・破線）。
	void appendPolylineStrip(MeshData& md, const Array<Vec3>& points, float halfWidth, float dashLength, float gapLength)
	{
		const bool solid = dashLength <= 0 || gapLength <= 0;
		double arc = 0;
		for (size_t i = 0; i + 1 < points.size(); ++i)
		{
			const Vec3 a = points[i], b = points[i + 1];
			const Vec3 d{ b.x - a.x, 0, b.z - a.z };
			const double length = d.length();
			if (length < 1e-4) { continue; }
			const double mid = arc + length * .5;
			arc += length;
			if (!solid && Math::Fmod(mid, static_cast<double>(dashLength + gapLength)) >= dashLength) { continue; }
			// 継ぎ目で隙間が出ないよう、両端の向きは前後の区間との平均にする。
			const auto rightAt = [&](size_t k) -> Vec3
			{
				const Vec3 before = points[k > 0 ? k - 1 : k], after = points[Min(k + 1, points.size() - 1)];
				Vec3 t{ after.x - before.x, 0, after.z - before.z };
				if (t.lengthSq() < 1e-8) { t = d; }
				return tangentToRight(t.normalized());
			};
			const Vec3 ra = rightAt(i) * static_cast<double>(halfWidth), rb = rightAt(i + 1) * static_cast<double>(halfWidth);
			const uint32 base = static_cast<uint32>(md.vertices.size());
			md.vertices << makeVert(a - ra, 0, 0) << makeVert(a + ra, 1, 0) << makeVert(b - rb, 0, 1) << makeVert(b + rb, 1, 1);
			appendQuad(md.indices, base, base + 1, base + 2, base + 3);
		}
	}

	/// @brief ノード内の区画線と導流帯（縞）を描く。
	void appendNodeMarkings(Array<RoadRenderer::LaneLineBatch>& out, const RoadNetwork& network, int nodeId, const World& world, const NodeMarkings::Set& set)
	{
		// 線と縞を、つながる道路の舗装（節点の中まで延ばした帯）の上へ載せる。切り口どうしを結ぶ曲線の高さは
		// 両端の線形補間なので、地形に沿う舗装の下へ潜って縞が消えることがある。
		struct Footprint { CubicBezier curve; float low, high; bool elevated; };
		Array<Footprint> footprints;
		if (const RoadNode* node = network.getNode(nodeId))
		{
			// 料金所側の鼻は上り・下りの 2 つの節点が 10m ほどで並び、両方の舗装が重なる。近くの分合流の節点の道路も含める。
			Array<int> edgeIds;
			for (const auto& attachment : node->attachments) { edgeIds << attachment.edgeId; }
			for (const auto& other : network.nodes())
			{
				if (other.id < 0 || other.id == nodeId || other.position.xz().distanceFromSq(node->position.xz()) > Square(40.0)
					|| !NodeMarkings::automaticApplies(network, other)) { continue; }
				for (const auto& attachment : other.attachments) { if (!edgeIds.contains(attachment.edgeId)) { edgeIds << attachment.edgeId; } }
			}
			for (const int edgeId : edgeIds)
			{
				const RoadEdge* edge = network.getEdge(edgeId);
				const auto bez = network.getBezier(edgeId);
				if (!edge || !bez) { continue; }
				float low = Math::Inf, high = -Math::Inf;
				for (const auto& part : edge->parts)
				{
					if (!part.asphalt()) { continue; }
					low = Min({ low, part.offsetA_L, part.offsetB_L }); high = Max({ high, part.offsetA_R, part.offsetB_R });
				}
				if (low < high) { footprints << Footprint{ *bez, low, high, edge->usesDesignHeight() }; }
			}
		}
		// 節点の舗装（切り口どうしを結ぶ多角形）。長い節点ではつながる道路の路面より高くなる所があり、線を覆う。
		const auto cap = JunctionGeometry::build(network, nodeId, false, &world);
		const auto capHeight = [&](const Vec3& p) -> Optional<double>
		{
			const Vec2 q{ p.x, p.z };
			for (const auto& triangle : cap.asphalt.indices)
			{
				const Vec3 a{ cap.asphalt.vertices[triangle.i0].pos }, b{ cap.asphalt.vertices[triangle.i1].pos }, c{ cap.asphalt.vertices[triangle.i2].pos };
				const Vec2 v0{ b.x - a.x, b.z - a.z }, v1{ c.x - a.x, c.z - a.z }, v2{ q.x - a.x, q.y - a.z };
				const double denominator = v0.x * v1.y - v1.x * v0.y;
				if (Abs(denominator) < 1e-9) { continue; }
				const double u = (v2.x * v1.y - v1.x * v2.y) / denominator, v = (v0.x * v2.y - v2.x * v0.y) / denominator;
				if (u < -1e-6 || v < -1e-6 || u + v > 1 + 1e-6) { continue; }
				if (!cap.elevated) { return world.sampleHeight(static_cast<float>(p.x), static_cast<float>(p.z)) + kRoadSurfaceLift + kRoadLineLift; }
				return a.y + (b.y - a.y) * u + (c.y - a.y) * v + kRoadLineLift;
			}
			return none;
		};
		const auto seatHeight = [&](const Vec3& p) -> double
		{
			double best = p.y;
			if (const auto h = capHeight(p)) { best = Max(best, *h); }
			for (const auto& footprint : footprints)
			{
				const double length = footprint.curve.totalLength;
				double nearestS = 0, nearestDistance = Math::Inf;
				for (double s = 0; s <= length; s += 1.0)
				{
					const Vec3 c = footprint.curve.positionAt(static_cast<float>(s));
					const double d = Vec2{ c.x - p.x, c.z - p.z }.lengthSq();
					if (d < nearestDistance) { nearestDistance = d; nearestS = s; }
				}
				if (nearestDistance > Square(Max(Abs(footprint.low), Abs(footprint.high)) + 1.0)) { continue; }
				const auto slice = makeSlice(footprint.curve, world, static_cast<float>(nearestS), kRoadLineLift, footprint.elevated);
				const double lateral = (p - slice.center).dot(slice.right);
				if (lateral < footprint.low - .2 || lateral > footprint.high + .2) { continue; }
				best = Max(best, slice.center.y);
			}
			return best;
		};
		const auto seat = [&](MeshData& mesh) { for (auto& vertex : mesh.vertices) { vertex.pos.y = static_cast<float>(seatHeight(Vec3{ vertex.pos })); } };
		Array<Optional<CubicBezier>> curves;
		// 導流帯のない分合流（料金所側の鼻）は、2 本のランプが長く重なって切り口が節点から遠い（90m 以上）。
		// 切り口どうしを 1 本の 3 次曲線で結ぶと路面からずれて扇状に広がるので、線をつながる道路の形に沿わせる。
		const bool followRoads = set.zones.isEmpty();
		for (const auto& line : set.lines)
		{
			curves << nodeLineCurve(network, nodeId, world, line);
			if (!curves.back() || line.type == LineType::None) { continue; }
			const LineStyle style = lineStyleFor(line.type);
			MeshData md;
			const Array<Vec3> path = followRoads ? nodeLinePath(network, nodeId, world, line) : Array<Vec3>{};
			if (path.size() >= 2) { appendPolylineStrip(md, path, style.lineWidth * .5f, style.dashLen, style.gapLen); }
			else { appendDashedStrip(md.vertices, md.indices, *curves.back(), world, 0, 0, style.lineWidth * .5f, style.dashLen, style.gapLen, 0, curves.back()->totalLength, 0.0f, true); }
			seat(md);
			if (!md.vertices.isEmpty()) { out << RoadRenderer::LaneLineBatch{ style.color.removeSRGBCurve(), Mesh{ md } }; }
		}
		MeshData stripes;
		constexpr float kStripeSpacing = 3.0f, kStripeHalfWidth = .15f;
		// 高速道路の分合流の鼻の路上施設: 導流帯の中のポストコーン（緑の柱・白の反射帯）と、鼻の先の衝撃緩和装置。
		const RoadNode* markingNode = network.getNode(nodeId);
		const bool expresswayNose = markingNode && NodeMarkings::automaticApplies(network, *markingNode);
		MeshData coneBodies, coneBands, cushionYellow, cushionBlack;
		const auto addCylinder = [](MeshData& mesh, const Vec3& base, double radius, double height)
		{
			MeshGeometry::appendMeshData(mesh, MeshData::Cylinder(Float3{ base + Vec3{ 0, height * .5, 0 } }, static_cast<float>(radius), static_cast<float>(height), 10));
		};
		const auto addPostCone = [&](const Vec3& surface)
		{
			const Vec3 ground{ surface.x, seatHeight(surface) - kRoadLineLift, surface.z };
			addCylinder(coneBodies, ground, .11, .05);   // 台座
			addCylinder(coneBodies, ground, .04, .80);   // 柱
			addCylinder(coneBands, ground + Vec3{ 0, .50, 0 }, .046, .07);
			addCylinder(coneBands, ground + Vec3{ 0, .66, 0 }, .046, .07);
		};
		for (const auto& zone : set.zones)
		{
			if (zone.leftLine < 0 || zone.rightLine < 0 || zone.leftLine >= static_cast<int>(curves.size()) || zone.rightLine >= static_cast<int>(curves.size())) { continue; }
			const auto& left = curves[zone.leftLine];
			const auto& right = curves[zone.rightLine];
			if (!left || !right) { continue; }
			const auto at = [](const CubicBezier& curve, double u) { return curve.positionAt(static_cast<float>(Clamp(u, 0.0, 1.0) * curve.totalLength)); };
			// 左の縁の点に向かい合う右の縁の点（最も近い点）。長さの違う 2 本を同じ割合で取ると縞が斜めにずれる。
			const auto across = [&](const Vec3& p) -> double
			{
				double best = 0, bestDistance = Math::Inf;
				constexpr int kSamples = 64;
				for (int k = 0; k <= kSamples; ++k)
				{
					const double v = k / static_cast<double>(kSamples);
					const double d = at(*right, v).distanceFromSq(p);
					if (d < bestDistance) { bestDistance = d; best = v; }
				}
				for (double step = .5 / kSamples; step > 1e-5; step *= .5)
				{
					for (const double v : { best - step, best + step })
					{
						const double d = at(*right, v).distanceFromSq(p);
						if (v >= 0 && v <= 1 && d < bestDistance) { bestDistance = d; best = v; }
					}
				}
				return best;
			};
			// V の先は 2 本が近づく側（分流の鼻・合流の先）へ向ける。
			const double towardNose = at(*left, 0).distanceFrom(at(*right, 0)) <= at(*left, 1).distanceFrom(at(*right, 1)) ? -1.0 : 1.0;
			const double length = Max(1.0, static_cast<double>(left->totalLength));
			for (double s = kStripeSpacing * .5; s < length; s += kStripeSpacing)
			{
				const double u = s / length;
				const Vec3 pl = at(*left, u);
				const double v = across(pl);
				const Vec3 pr = at(*right, v);
				const double width = pl.distanceFrom(pr);
				// 2 本の縁が開き始めた細い区間には縞を描かない（実際の導流帯も幅 1m 程度から縞が入る）。
				if (width < 1.0) { continue; }
				// 縞の向き: 2 本の縁の接線の平均（流れの向き）。
				const Vec3 tangentL = left->tangentAt(static_cast<float>(u * left->totalLength)), tangentR = right->tangentAt(static_cast<float>(v * right->totalLength));
				const Vec3 ahead{ tangentL.x + tangentR.x, 0, tangentL.z + tangentR.z };
				const Vec3 along = ahead.lengthSq() > 1e-8 ? ahead.normalized() : Vec3{ 1, 0, 0 };
				if (zone.pattern == NodeZonePattern::Chevron)
				{
					const Vec3 apex = (pl + pr) * .5 + along * (towardNose * width * .5);
					appendBar(stripes.vertices, stripes.indices, pl, apex, kStripeHalfWidth);
					appendBar(stripes.vertices, stripes.indices, apex, pr, kStripeHalfWidth);
				}
				else
				{
					// 斜線は流れに対して約 60 度（実景のランプの分岐の縞）。細い区間は縞が線に重なって潰れるので描かない。
					if (width < 1.6) { continue; }
					appendBar(stripes.vertices, stripes.indices, pl, at(*right, v + width * .55 / Max(1.0, static_cast<double>(right->totalLength))), kStripeHalfWidth);
				}
			}
			if (!expresswayNose) { continue; }
			// 開いた側（鼻）の端: 2 本の縁の中点。ポストコーンは分流（出口）の導流帯だけ、中心線上の幅 1.2m 以上の区間に 6m 間隔。
			// （出口の直前で本線から割り込むのを防ぐ。合流部には立てない。）
			const double openU = towardNose < 0 ? 1.0 : 0.0;
			const Vec3 noseLeft = at(*left, openU), noseRight = at(*right, across(noseLeft));
			const double noseWidth = noseLeft.distanceFrom(noseRight);
			const bool diverge = NodeMarkings::sameEnd(set.lines[zone.leftLine].from, set.lines[zone.rightLine].from);
			for (double s = 3.0; diverge && s < length; s += 6.0)
			{
				const double u = towardNose < 0 ? s / length : 1.0 - s / length;
				const Vec3 pl = at(*left, u), pr = at(*right, across(pl));
				if (pl.distanceFrom(pr) < 1.2) { continue; }
				addPostCone((pl + pr) * .5);
			}
			if (noseWidth >= 1.5)
			{
				// 衝撃緩和装置: 黄色の円筒（黒の帯）を鼻の先に 2 本並べる。
				const Vec3 ahead = (noseLeft + noseRight) * .5 - (at(*left, Abs(openU - .9)) + at(*right, across(at(*left, Abs(openU - .9))))) * .5;
				const Vec3 direction = Vec3{ ahead.x, 0, ahead.z }.lengthSq() > 1e-6 ? Vec3{ ahead.x, 0, ahead.z }.normalized() : Vec3{ 1, 0, 0 };
				Vec3 nose = (noseLeft + noseRight) * .5 + direction * .6;
				nose.y = seatHeight(nose) - kRoadLineLift;
				for (int drum = 0; drum < 2; ++drum)
				{
					const Vec3 base = nose + direction * (drum * .95);
					addCylinder(cushionYellow, base, .45, .35);
					addCylinder(cushionBlack, base + Vec3{ 0, .35, 0 }, .452, .18);
					addCylinder(cushionYellow, base + Vec3{ 0, .53, 0 }, .45, .37);
				}
			}
		}
		seat(stripes);
		if (!stripes.vertices.isEmpty()) { out << RoadRenderer::LaneLineBatch{ ColorF{ 1, 1, 1 }.removeSRGBCurve(), Mesh{ stripes } }; }
		if (!coneBodies.vertices.isEmpty()) { out << RoadRenderer::LaneLineBatch{ ColorF{ 0.20, 0.62, 0.36 }.removeSRGBCurve(), Mesh{ coneBodies } }; }
		if (!coneBands.vertices.isEmpty()) { out << RoadRenderer::LaneLineBatch{ ColorF{ 0.95, 0.95, 0.93 }.removeSRGBCurve(), Mesh{ coneBands } }; }
		if (!cushionYellow.vertices.isEmpty()) { out << RoadRenderer::LaneLineBatch{ ColorF{ 0.98, 0.78, 0.08 }.removeSRGBCurve(), Mesh{ cushionYellow } }; }
		if (!cushionBlack.vertices.isEmpty()) { out << RoadRenderer::LaneLineBatch{ ColorF{ 0.06, 0.06, 0.06 }.removeSRGBCurve(), Mesh{ cushionBlack } }; }
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


/// @brief 同じ節点から同じ向きへ分かれる（または同じ向きから集まる）2 本の道路の、向かい合う車線境界の線を合わせる。
/// @details 料金所側の鼻では、1 本の路面から分かれる 2 本のランプの端を左右へ寄せ、その寄せを道路の全長で戻す。
///   2 本の中心線は鼻の近くではゆっくり離れるので、その間は相手の車線へ食い込み、向かい合う 2 本の線が X 字に交差する。
///   食い込んでいる間は 2 本の線を中点の 1 本にそろえ、離れたところから導流帯の V 字に開くようにする。
static std::function<Vec3(const Vec3&, const Vec3&)> sharedEndLineAdjust(const RoadNetwork& network, const RoadEdge& edge, float lateral, bool atA)
{
	if (edge.lanes.isEmpty() || edge.roadType != RoadType::Expressway) { return {}; }
	const auto bezier = network.getBezier(edge.id);
	if (!bezier || bezier->totalLength <= 1.0f) { return {}; }
	float laneMin = Math::Inf, laneMax = -Math::Inf;
	for (const auto& lane : edge.lanes) { laneMin = Min(laneMin, atA ? lane.offsetA_L : lane.offsetB_L); laneMax = Max(laneMax, atA ? lane.offsetA_R : lane.offsetB_R); }
	const bool onLeft = Abs(lateral - laneMin) < .05f, onRight = Abs(lateral - laneMax) < .05f;
	if (!onLeft && !onRight) { return {}; }
	struct Sample { Vec2 position; Vec2 boundary; };
	struct Facing { Array<Sample> samples; double side; };
	Array<Facing> facings;
	for (const int nodeId : { edge.nodeA, edge.nodeB })
	{
		const RoadNode* node = network.getNode(nodeId);
		if (!node || !NodeMarkings::automaticApplies(network, *node)) { continue; }
		const bool atStart = edge.nodeA == nodeId;
		const float cut = atStart ? edge.cutoffA : bezier->totalLength - edge.cutoffB;
		Vec3 direction = bezier->tangentAt(cut) * (atStart ? 1.0 : -1.0); direction.y = 0;
		if (direction.lengthSq() < 1e-8) { continue; }
		direction.normalize();
		for (const auto& attachment : node->attachments)
		{
			const RoadEdge* other = network.getEdge(attachment.edgeId);
			if (!other || other->id == edge.id || other->lanes.isEmpty()) { continue; }
			const auto otherBezier = network.getBezier(other->id);
			if (!otherBezier || otherBezier->totalLength <= 1.0f) { continue; }
			const bool otherStart = other->nodeA == nodeId;
			const float otherCut = otherStart ? other->cutoffA : otherBezier->totalLength - other->cutoffB;
			Vec3 otherDirection = otherBezier->tangentAt(otherCut) * (otherStart ? 1.0 : -1.0); otherDirection.y = 0;
			if (otherDirection.lengthSq() < 1e-8 || direction.dot(otherDirection.normalized()) < 0.80) { continue; }
			// 相手が左右どちらにあるか: 節点から少し離れた所の相手の車線の中心を、こちらの横方向へ投影する。
			const float probe = Min(20.0f, Min(bezier->totalLength, otherBezier->totalLength) * .5f);
			const float sHere = atStart ? probe : bezier->totalLength - probe, sThere = otherStart ? probe : otherBezier->totalLength - probe;
			float otherMid = 0;
			{
				const float t = sThere / otherBezier->totalLength;
				float lo = Math::Inf, hi = -Math::Inf;
				for (const auto& lane : other->lanes) { lo = Min(lo, Math::Lerp(lane.offsetA_L, lane.offsetB_L, t)); hi = Max(hi, Math::Lerp(lane.offsetA_R, lane.offsetB_R, t)); }
				otherMid = (lo + hi) * .5f;
			}
			float hereMid = 0;
			{
				const float t = sHere / bezier->totalLength;
				float lo = Math::Inf, hi = -Math::Inf;
				for (const auto& lane : edge.lanes) { lo = Min(lo, Math::Lerp(lane.offsetA_L, lane.offsetB_L, t)); hi = Max(hi, Math::Lerp(lane.offsetA_R, lane.offsetB_R, t)); }
				hereMid = (lo + hi) * .5f;
			}
			const Vec3 there = otherBezier->positionAt(sThere) + tangentToRight(otherBezier->tangentAt(sThere)) * static_cast<double>(otherMid);
			const Vec3 here = bezier->positionAt(sHere) + tangentToRight(bezier->tangentAt(sHere)) * static_cast<double>(hereMid);
			const double side = (there - here).dot(tangentToRight(bezier->tangentAt(sHere)));
			// 向かい合う境界だけ: 相手が右なら自分の右端の線、左なら左端の線。
			if ((side > 0) != onRight) { continue; }
			Facing facing{ {}, side > 0 ? 1.0 : -1.0 };
			for (double s = 0; s <= otherBezier->totalLength; s += 1.0)
			{
				const float t = static_cast<float>(s / otherBezier->totalLength);
				float boundary = side > 0 ? Math::InfF : -Math::InfF;
				for (const auto& lane : other->lanes)
				{
					boundary = side > 0 ? Min(boundary, Math::Lerp(lane.offsetA_L, lane.offsetB_L, t)) : Max(boundary, Math::Lerp(lane.offsetA_R, lane.offsetB_R, t));
				}
				const Vec3 c = otherBezier->positionAt(static_cast<float>(s));
				const Vec3 q = c + tangentToRight(otherBezier->tangentAt(static_cast<float>(s))) * static_cast<double>(boundary);
				facing.samples << Sample{ Vec2{ c.x, c.z }, Vec2{ q.x, q.z } };
			}
			facings << std::move(facing);
		}
	}
	if (facings.isEmpty()) { return {}; }
	return [facings = std::move(facings)](const Vec3& p, const Vec3& right) -> Vec3
	{
		Vec3 result = p;
		for (const auto& facing : facings)
		{
			// 相手の中心線上で最も近い点の境界と比べる。
			const Vec2 flat{ p.x, p.z };
			double best = Math::Inf; Vec2 q{};
			for (const auto& sample : facing.samples)
			{
				const double d = (sample.position - flat).lengthSq();
				if (d < best) { best = d; q = sample.boundary; }
			}
			if (best > Square(12.0)) { continue; }
			const Vec2 r{ right.x, right.z };
			const double depth = (flat - q).dot(r) * facing.side;
			// こちらの線が相手の境界を越えている（相手の車線に入っている）間は中点へ。
			if (depth > 0 && depth < 4.0)
			{
				const Vec2 mid = (flat + q) * .5;
				result = Vec3{ mid.x, p.y, mid.y };
			}
		}
		return result;
	};
}
static Array<RoadRenderer::LaneLineBatch> buildLaneLineBatchesFromPlacements(
	const RoadNetwork& network, const RoadEdge& edge, const CubicBezier& bezier, const World& world,
	float marginA, float marginB, const Array<RoadMarkingPlacement>& placements)
{
	StripRange edgeRange;
	if (!calcStripRange(bezier.totalLength, marginA, marginB, edgeRange)) return {};

	Array<RoadRenderer::LaneLineBatch> batches;
	const MeshData surface = RoadGeometry::roadbedSurface(edge, bezier, world);
	const MeshData overlap = adjacentPavement(network,edge,world,true);
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

		MeshData md;
		LineType type = LineType::SolidWhite;
		float nearest = Math::Inf;
		// 車線の境界に載る線は、A 端と B 端で横位置が違う（ランプの車線寄せなど）ときもその境界に沿わせる。
		float lateralA = marking.lateralCenter, lateralB = marking.lateralCenter;
		bool onBoundary = false;
		for (const auto& lane : edge.lanes)
		{
			for (const auto boundary : { std::tuple<float,float,LineType>{lane.offsetA_L,lane.offsetB_L,lane.lineLeft}, {lane.offsetA_R,lane.offsetB_R,lane.lineRight} })
			{
				const float distance = Abs(marking.lateralCenter-std::get<0>(boundary));
				if (distance < nearest)
				{
					nearest = distance; type = std::get<2>(boundary);
					if (distance < .05f) { lateralB = marking.lateralCenter + (std::get<1>(boundary) - std::get<0>(boundary)); onBoundary = true; }
				}
			}
		}
		// appendDashedStrip の横位置は道路の A 端（s=0）と B 端（s=全長）の値で、その間を線形に移る。
		// 車線境界の線はその境界の両端の値をそのまま使う（節点の区画線の端とずれないように）。
		const float offsetA = onBoundary ? lateralA : Clamp(marking.lateralCenter, startMin, startMax);
		const float offsetB = onBoundary ? lateralB : Clamp(marking.lateralCenter, endMin, endMax);
		const auto style = lineStyleFor(type);
		appendDashedStrip(md.vertices, md.indices, bezier, world, offsetA, offsetB, halfLineWidth,
			style.dashLen, style.gapLen, sStart, sEnd, static_cast<float>(kRoadLineLift), edge.usesDesignHeight(),
			sharedEndLineAdjust(network, edge, lateralA, true));
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

	// 分合流部・手で設定したノード — 切り口どうしを結ぶ区画線と導流帯
	if (NodeMarkings::applies(network, *node))
	{
		Array<LaneLineBatch> custom;
		appendNodeMarkings(custom, network, nodeId, world, NodeMarkings::effective(network, nodeId));
		return custom;
	}

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
			                 lineStyle.lineWidth, lineStyle.color, world, &surface, true);
		}
		if (l.lineRight != LineType::None)
		{
			const auto lineStyle = lineStyleFor(l.lineRight);
			appendBezierLine(batches, l.rightPos, targetPos, l.rightTan, targetTan,
			                 lineStyle.lineWidth, lineStyle.color, world, &surface, true);
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
				                 lineStyle.lineWidth, lineStyle.color, world, &surface, true);
			}
			if (lineR != LineType::None)
			{
				const auto lineStyle = lineStyleFor(lineR);
				appendBezierLine(batches, lA.rightPos, lB.rightPos, lA.rightTan, lB.rightTan,
				                 lineStyle.lineWidth, lineStyle.color, world, &surface, true);
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
