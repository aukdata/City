#include "RoadRenderer.hpp"
#include "../road/SignArtwork.hpp"
#include "BridgeStructure.hpp"
#include "../debug/DebugLog.hpp"
#include "../road/RoadArrow.hpp"
#include "../road/RoadSign.hpp"
#include "../road/RoadGeometry.hpp"
#include "../road/JunctionGeometry.hpp"
#include "../road/RoadMarkingGenerator.hpp"
#include "../road/ObjParser.hpp"
#include "../traffic/TrafficCommon.hpp"
#include "../asset/AssetRegistrar.hpp"
#include <Siv3D/ViewFrustum.hpp>

// ─────────────────────────────────────────────────────────────────────────────
// 内部ヘルパー（無名名前空間）
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	/// @brief 弧長 s での中心・右ベクトルを返す（Y は地形 + リフト）
	struct SliceInfo { Vec3 center; Vec3 right; };
	SliceInfo makeSlice(const CubicBezier& bez, const World& world,
	                    float s, double terrainLift, bool useElevation = false)
	{
		const Vec3  p  = bez.positionAt(s);
		double y;
		if (useElevation)
			y = p.y + terrainLift;  // ノード Y は地形高さなので、通常と同じ terrainLift を加算
		else
		{
			const float gy = world.sampleHeight(static_cast<float>(p.x), static_cast<float>(p.z));
			y = gy + terrainLift;
		}
		return { Vec3{ p.x, y, p.z }, tangentToRight(bez.tangentAt(s)) };
	}

	/// @brief Vertex3D を生成する
	Vertex3D makeVert(const Vec3& pos, float u, float v)
	{
		Vertex3D vt;
		vt.pos    = Float3{ static_cast<float>(pos.x), static_cast<float>(pos.y), static_cast<float>(pos.z) };
		vt.normal = Float3{ 0.0f, 1.0f, 0.0f };
		vt.tex    = Float2{ u, v };
		return vt;
	}


	/// @brief 世界空間 UV 係数 [1/m]。路面素材の短周期反復を抑える。
	constexpr float kWorldUV = 0.5f;

	/// @brief ワールド XZ 座標から世界空間 UV を生成
	Vertex3D makeVertWorldUV(const Vec3& pos)
	{
		return makeVert(pos,
			static_cast<float>(pos.x) * kWorldUV,
			static_cast<float>(pos.z) * kWorldUV);
	}
	Vertex3D makeVertWorldUVWithNormal(const Vec3& pos, const Float3& normal)
	{
		Vertex3D vt = makeVertWorldUV(pos);
		vt.normal = normal;
		return vt;
	}

	/// @brief 四角形を頂点・インデックス配列に追記する（CW ワインディング）
	void appendQuad(Array<TriangleIndex32>& indices,
	                uint32 iL0, uint32 iR0, uint32 iL1, uint32 iR1)
	{
		indices << TriangleIndex32{ iL0, iL1, iR0 };
		indices << TriangleIndex32{ iR0, iL1, iR1 };
	}
	/// @brief Imported road parts and junction patches must face their authored normals.
	void orientRoadFaces(MeshData& mesh)
	{
		for (auto& triangle : mesh.indices)
		{
			const auto& a = mesh.vertices[triangle.i0];
			const auto& b = mesh.vertices[triangle.i1];
			const auto& c = mesh.vertices[triangle.i2];
			if ((b.pos - a.pos).cross(c.pos - a.pos).dot(a.normal + b.normal + c.normal) < 0.0f)
			{
				std::swap(triangle.i1, triangle.i2);
			}
		}
	}
	void appendMeshData(MeshData& dst, const MeshData& src)
	{
		const uint32 offset = static_cast<uint32>(dst.vertices.size());
		dst.vertices.append(src.vertices);
		for (const auto& tri : src.indices)
		{
			dst.indices << TriangleIndex32{ tri.i0 + offset, tri.i1 + offset, tri.i2 + offset };
		}
	}

	/// @brief At acute merges, an approach sidewalk must not continue inside its
	/// neighbour's carriageway beyond the nominal junction cutoff.
	/// @brief CPU cylinders keep round utility hardware in the existing material batches.
	void appendCylinder(MeshData& mesh, Vec3 center, double radius, double height, uint32 sides = 12)
	{
		auto cylinder = MeshData::Cylinder(Float3{center}, radius, height, sides);
		appendMeshData(mesh,cylinder);
	}

	MeshData adjacentPavement(const RoadNetwork& network, const RoadEdge& edge, const World& world)
	{
		MeshData mask;
		const auto bezier = network.getBezier(edge.id);
		if (!bezier) { return mask; }
		HashSet<int> included;
		for (const int nodeId : { edge.nodeA,edge.nodeB })
		{
			const auto* node = network.getNode(nodeId);
			if (!node || node->attachments.size() < 2) { continue; }
			const bool atStart = edge.nodeA == nodeId;
			Vec3 direction = bezier->tangentAt(atStart ? edge.cutoffA : bezier->totalLength-edge.cutoffB) * (atStart ? 1.0 : -1.0);
			direction.y = 0.0;
			if (direction.lengthSq() < 1e-8) { continue; }
			direction.normalize();
			for (const auto& attachment : node->attachments)
			{
				const auto* other = network.getEdge(attachment.edgeId);
				if (!other || other->id == edge.id || other->useElevation != edge.useElevation || !other->isRoadbedBuilt()
					|| (other->edgeState != EdgeState::Open && other->edgeState != EdgeState::Existing)) { continue; }
				const auto otherBezier = network.getBezier(other->id);
				if (!otherBezier) { continue; }
				const bool otherStart = other->nodeA == nodeId;
				Vec3 otherDirection = otherBezier->tangentAt(otherStart ? other->cutoffA : otherBezier->totalLength-other->cutoffB) * (otherStart ? 1.0 : -1.0);
				otherDirection.y = 0.0;
				if (otherDirection.lengthSq() < 1e-8 || direction.dot(otherDirection.normalized()) < 0.80) { continue; }
				if (included.insert(other->id).second)
				{
					RoadEdge covered = *other;
					if (other->id < edge.id)
					{
						// Give overlapping roadside surfaces one owner to avoid coplanar
						// curb/sidewalk faces fighting after two near-parallel roads merge.
						for (auto& part : covered.parts)
						{
							if (RoadGeometry::isStructuralStrip(part)) { part.type = RoadPartType::Roadbed; }
						}
					}
					appendMeshData(mask,RoadGeometry::roadbedSurface(covered,*otherBezier,world));
				}
			}
		}
		return mask;
	}

	void rotateMeshY(MeshData& mesh, float cx, float cz, float angle)
	{
		if (angle == 0.0f) return;
		const float cosA = Math::Cos(angle);
		const float sinA = Math::Sin(angle);
		for (auto& v : mesh.vertices)
		{
			const float dx = v.pos.x - cx;
			const float dz = v.pos.z - cz;
			v.pos.x = cx + dx * cosA - dz * sinA;
			v.pos.z = cz + dx * sinA + dz * cosA;
			const float nx = v.normal.x;
			const float nz = v.normal.z;
			v.normal.x = nx * cosA - nz * sinA;
			v.normal.z = nx * sinA + nz * cosA;
		}
	}

	void appendOrientedBox(MeshData& dst, const Vec3& center, const Float3& size, float angle)
	{
		MeshData box = MeshData::Box(Float3{ static_cast<float>(center.x), static_cast<float>(center.y), static_cast<float>(center.z) }, size);
		rotateMeshY(box, static_cast<float>(center.x), static_cast<float>(center.z), angle);
		appendMeshData(dst, box);
	}

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

	/// @brief エッジ部品メッシュの弧長範囲を計算する
	/// @details マージンで両端をトリムし、接合部の隙間対策として kOverlap だけ伸ばす。
	///   戻り値 false の場合は描画範囲が無いため生成をスキップする。
	struct StripRange { float sStart; float sEnd; };
	bool calcStripRange(float totalLen, float marginA, float marginB, StripRange& out)
	{
		constexpr float kOverlap = 0.1f;
		out.sStart = Max(marginA - kOverlap, 0.0f);
		out.sEnd   = Min(totalLen - marginB + kOverlap, totalLen);
		return (out.sStart < out.sEnd - 0.1f);
	}

	/// @brief RoadPartType のフォールバック色と高さオフセットを返す
	void getPartDefaults(RoadPartType type, ColorF& color, float& heightOff)
	{
		switch (type)
		{
		case RoadPartType::Roadbed:   color = ColorF{ 0.56, 0.56, 0.53 }; break;
		case RoadPartType::Sidewalk:  color = ColorF{ 0.78, 0.77, 0.72 }; heightOff = 0.035f; break;
		case RoadPartType::Curb:      color = ColorF{ 0.84, 0.83, 0.78 }; heightOff = 0.040f; break;
		case RoadPartType::Median:    color = ColorF{ 0.70, 0.69, 0.65 }; break;
		case RoadPartType::Shoulder:  color = ColorF{ 0.60, 0.60, 0.56 }; break;
		case RoadPartType::Slope:     color = ColorF{ 0.45, 0.58, 0.35 }; break;
		case RoadPartType::Guardrail: color = ColorF{ 0.82, 0.82, 0.82 }; break;
		case RoadPartType::Wall:      color = ColorF{ 0.78, 0.78, 0.76 }; break;
		default: break;
		}
	}

	float roadPartSideSkirtDrop(RoadPartType type, float heightOffset)
	{
		switch (type)
		{
		case RoadPartType::Curb:
			return Max(0.035f, heightOffset + 0.010f);
		case RoadPartType::Roadbed:
			return Max(0.180f, heightOffset + 0.045f);
		case RoadPartType::RoadsideGutter:
		case RoadPartType::Gutter:
			return Max(0.120f, heightOffset + 0.035f);
		case RoadPartType::Sidewalk:
			return Max(0.030f, heightOffset + 0.008f);
		case RoadPartType::Shoulder:
		case RoadPartType::Median:
			return Max(0.025f, heightOffset + 0.006f);
		default:
			return 0.0f;
		}
	}
	MeshData buildRoadPartModelStrip(const CubicBezier& bezier, const World& world,
	                                 const RoadPartDef& def, const PartModelData& model,
	                                 float offsetA_L, float offsetA_R,
	                                 float offsetB_L, float offsetB_R,
	                                 float sStart, float sEnd, float lodFactor,
	                                 bool useElevation)
	{
		if (model.vertices.isEmpty() || model.indices.isEmpty()) return MeshData{};
		const float spanLen = sEnd - sStart;
		if (spanLen <= 0.1f) return MeshData{};
		const float totalLength = bezier.totalLength;
		const float unitLen = Max(0.1f, def.modelUnitLen);
		const float unitWidth = Max(0.001f, def.modelUnitWidth);
		const int segmentCount = Clamp(static_cast<int>(spanLen / unitLen * lodFactor) + 1,
			3, static_cast<int>(120 * lodFactor));

		MeshData md;
		md.vertices.reserve(model.vertices.size() * segmentCount);
		md.indices.reserve(model.indices.size() * segmentCount);

		for (int segment = 0; segment < segmentCount; ++segment)
		{
			const float segT0 = segment / static_cast<float>(segmentCount);
			const float segT1 = (segment + 1) / static_cast<float>(segmentCount);
			const float segS0 = Math::Lerp(sStart, sEnd, segT0);
			const float segS1 = Math::Lerp(sStart, sEnd, segT1);
			const uint32 indexBase = static_cast<uint32>(md.vertices.size());

			for (const Vertex3D& src : model.vertices)
			{
				const float localAcross = Clamp(src.pos.x / unitWidth, 0.0f, 1.0f);
				const float localAlong = Clamp(src.pos.z / unitLen, 0.0f, 1.0f);
				const float s = Math::Lerp(segS0, segS1, localAlong);
				const float roadT = (totalLength > 0.0f) ? Clamp(s / totalLength, 0.0f, 1.0f) : 0.0f;
				const float offsetL = Math::Lerp(offsetA_L, offsetB_L, roadT);
				const float offsetR = Math::Lerp(offsetA_R, offsetB_R, roadT);
				const float lateral = Math::Lerp(offsetL, offsetR, localAcross);
				const auto sl = makeSlice(bezier, world, s, kRoadSurfaceLift, useElevation);
				Vec3 tangent = bezier.tangentAt(s);
				if (tangent.lengthSq() <= 1e-8) tangent = Vec3{ 1.0, 0.0, 0.0 };
				tangent.normalize();

				Vertex3D dst = src;
				Vec3 pos = sl.center + sl.right * static_cast<double>(lateral) + Vec3{ 0.0, src.pos.y + def.heightOffset, 0.0 };
				if (!useElevation)
				{
					pos.y = world.sampleHeight(static_cast<float>(pos.x), static_cast<float>(pos.z)) + kRoadSurfaceLift + src.pos.y + def.heightOffset;
				}
				dst.pos = Float3{ static_cast<float>(pos.x), static_cast<float>(pos.y), static_cast<float>(pos.z) };
				dst.normal = Float3{
					static_cast<float>(sl.right.x) * src.normal.x + src.normal.y * 0.0f + static_cast<float>(tangent.x) * src.normal.z,
					src.normal.y,
					static_cast<float>(sl.right.z) * src.normal.x + src.normal.y * 0.0f + static_cast<float>(tangent.z) * src.normal.z
				};
				dst.tex = Float2{ src.tex.x + static_cast<float>(segment), src.tex.y };
				md.vertices << dst;
			}

			for (const TriangleIndex32& tri : model.indices)
			{
				md.indices << TriangleIndex32{ tri.i0 + indexBase, tri.i1 + indexBase, tri.i2 + indexBase };
			}
		}

		return md;
	}
	/// @brief LineType → 色・線幅
	struct LineStyle { ColorF color; float lineWidth; float dashLen; float gapLen; };
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

	/// @brief エッジの cutoff 位置でのオフセット付き線位置 + 接線方向を計算する
	struct EdgeLineInfo { Vec3 pos; Vec3 tangent; };
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
		const double lineY = edge.useElevation
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

// ─────────────────────────────────────────────────────────────────────────────
// RoadRenderer 公開メソッド
// ─────────────────────────────────────────────────────────────────────────────

bool RoadRenderer::loadAssets()
{
	m_constructionEarthPS=PixelShader::HLSL(U"shaders/hlsl/city_forward.hlsl",U"Earth_PS");
	m_constructionAggregatePS=PixelShader::HLSL(U"shaders/hlsl/city_forward.hlsl",U"Aggregate_PS");
	m_constructionSoilNormal=Texture{U"assets/third_party/polyhaven/brown_mud/brown_mud_nor_dx_1k.jpg",TextureDesc::Mipped};
	m_constructionGravelNormal=Texture{U"assets/third_party/polyhaven/gravel_ground_01/gravel_ground_01_nor_dx_1k.jpg",TextureDesc::Mipped};
	if(!m_constructionEarthPS || !m_constructionAggregatePS || !m_constructionSoilNormal || !m_constructionGravelNormal) return false;
	m_constructionSoil = Texture{U"assets/third_party/polyhaven/brown_mud/brown_mud_diff_1k.jpg",TextureDesc::MippedSRGB};
	m_constructionGravel = Texture{U"assets/third_party/polyhaven/gravel_ground_01/gravel_ground_01_diff_1k.jpg",TextureDesc::MippedSRGB};
	m_constructionConcrete = Texture{U"assets/third_party/polyhaven/concrete_wall_001/concrete_wall_001_diff_1k.jpg",TextureDesc::MippedSRGB};
	const std::array<String,4> models{U"excavator",U"road_roller",U"asphalt_paver",U"mobile_crane"};
	for(size_t i=0;i<models.size();++i)
	{
		m_constructionModels[i]=Model{U"assets/construction/{}.obj"_fmt(models[i])};
		if(m_constructionModels[i].isEmpty()) return false;
		Model::RegisterDiffuseTextures(m_constructionModels[i],TextureDesc::MippedSRGB);
	}
	m_arrowMarkingRegistry.load(U"assets/road_markings");
	m_cableVS = VertexShader::HLSL(U"shaders/hlsl/city_cable.hlsl", U"Cable_VS");
	m_cablePS = PixelShader::HLSL(U"shaders/hlsl/city_cable.hlsl", U"Cable_PS");
	if (!m_cableVS || !m_cablePS) { return false; }
	m_signalRegistry.load(U"assets/signals");
	return m_partRegistry.load(U"assets/road_parts");
}

void RoadRenderer::render(const RoadNetwork& network, const World& world,
                          const ViewFrustum& frustum, Vec3 cameraPos)
{
	// 可視判定・地形起因のキャッシュ失効・エッジ/ノード描画を 1 フレーム内でまとめて回す。
	m_visibleEdges.clear();
	const Size viewport = Scene::Size();
	m_cableView->viewport = Float4{ static_cast<float>(viewport.x), static_cast<float>(viewport.y),
		1.0f / viewport.x, 1.0f / viewport.y };
	Profiler::EnableAssetCreationWarning(false);

	synchronizeTerrainChanges(world, network);
	// ---- エッジ描画（端をノード半幅分カット）----
	const float camX = static_cast<float>(cameraPos.x);
	const float camZ = static_cast<float>(cameraPos.z);
	constexpr float kDrawMaxDistSqF = static_cast<float>(kDrawMaxDistSq);
	constexpr float kLodDistSqF     = static_cast<float>(kLodDistSq);


	for (const RoadEdge& edge : network.edges())
	{
		if (edge.id == -1) continue;

		// バウンディング情報をキャッシュから取得（なければ計算してキャッシュ）
		auto boundsIt = m_boundsCache.find(edge.id);
		if (boundsIt == m_boundsCache.end())
		{
			const RoadNode* nA = network.getNode(edge.nodeA);
			const RoadNode* nB = network.getNode(edge.nodeB);
			if (!nA || !nB) continue;
			EdgeBounds b;
			b.center = Float3{
				static_cast<float>((nA->position.x + nB->position.x) * 0.5),
				static_cast<float>((nA->position.y + nB->position.y) * 0.5),
				static_cast<float>((nA->position.z + nB->position.z) * 0.5)
			};
			const float chordSq = static_cast<float>((nA->position - nB->position).lengthSq());
			const float arcR    = edge.length * 0.6f;
			b.radiusSq = Max(arcR * arcR, chordSq * 0.36f);
			boundsIt = m_boundsCache.emplace(edge.id, b).first;
		}
		const auto& bounds = boundsIt->second;

		// 距離チェックを先に（安価: float 演算のみ）
		const float dx = bounds.center.x - camX;
		const float dz = bounds.center.z - camZ;
		const float distSq = dx * dx + dz * dz;
		if (distSq > kDrawMaxDistSqF) continue;
		const bool isClose = distSq < kLodDistSqF;

		// 視錐台カリング（距離チェックを通過した分のみ）
		const float radius = Math::Sqrt(bounds.radiusSq);
		if (!frustum.intersects(Sphere{ Vec3{ bounds.center }, static_cast<double>(radius) })) continue;

		m_visibleEdges.emplace(edge.id);

		const float mA = edgeMargin(edge, edge.nodeA);
		const float mB = edgeMargin(edge, edge.nodeB);

		const Stopwatch edgeTimer{StartImmediately::Yes};
		drawEdge(edge, network, mA, mB, world, isClose);
		if (edgeTimer.msF() > 8.0) { DBG_LOG(U"[RoadBuild] edge={} length={:.1f} ms={:.2f}"_fmt(edge.id,edge.length,edgeTimer.msF())); }
	}

	// ---- ノードキャップ描画（交差点フィル）----
	for (const RoadNode& node : network.nodes())
	{
		if (node.id < 0) continue;

		// 距離チェック（float 演算のみ）
		const float nodeDx = static_cast<float>(node.position.x) - camX;
		const float nodeDz = static_cast<float>(node.position.z) - camZ;
		const float nodeDistSq = nodeDx * nodeDx + nodeDz * nodeDz;
		if (nodeDistSq > kDrawMaxDistSqF) continue;

		// 視錐台カリング
		if (!frustum.intersects(Sphere{ node.position, 30.0 })) continue;

		const bool isClose = nodeDistSq < kLodDistSqF;
		const Stopwatch nodeTimer{StartImmediately::Yes};
		drawNodeCap(network, node.id, world, isClose);
		if (nodeTimer.msF() > 8.0) { DBG_LOG(U"[RoadBuild] node={} arms={} ms={:.2f}"_fmt(node.id,node.attachments.size(),nodeTimer.msF())); }
	}
}

void RoadRenderer::synchronizeTerrainChanges(const World& world, const RoadNetwork& network)
{
	// 地形変更時は該当チャンク内のエッジ/ノードのキャッシュのみクリアする
	for (const Chunk* chunk : world.getActiveChunks())
	{
		if (!chunk) { continue; }
		const int64 key = chunkCoordToKey(chunk->coord);
		if (!chunk->meshDirty) { m_dirtyTerrainObserved.erase(key); continue; }
		if (!m_dirtyTerrainObserved.insert(key).second) { continue; }
		const double cx = chunk->coord.x * static_cast<double>(CHUNK_SIZE);
		const double cz = chunk->coord.y * static_cast<double>(CHUNK_SIZE);
		const double cs = CHUNK_SIZE;
		for (const auto& node : network.nodes())
		{
			if (node.id < 0) continue;
			if (node.position.x >= cx && node.position.x < cx + cs &&
			    node.position.z >= cz && node.position.z < cz + cs)
			{
				eraseNodeCaches(node.id);
				for (const auto& att : node.attachments)
					eraseEdgeCaches(att.edgeId);
			}
		}
	}
}

void RoadRenderer::renderShadowCasters(Vec3 focus, double radius)
{
	for (const auto& [id,cache] : m_constructionCache)
	{
		const auto bounds = m_boundsCache.find(id);
		if (bounds == m_boundsCache.end()) continue;
		if (Vec3{bounds->second.center}.distanceFrom(focus) > radius + Sqrt(bounds->second.radiusSq)) continue;
		for (const auto& surface : cache.surfaces) surface.meshPair.detail.draw(ColorF{1});
		for (const auto& detail : cache.details) detail.mesh.draw(ColorF{1});
		for (const auto& [index,transform] : cache.machines) m_constructionModels[index].draw(transform);
	}
	for (const auto& [edgeId, entries] : m_partLodBatchCache)
	{
		const auto bounds = m_boundsCache.find(edgeId);
		if (bounds == m_boundsCache.end()) { continue; }
		const Vec3 delta = Vec3{ bounds->second.center } - focus;
		const double reach = radius + Sqrt(bounds->second.radiusSq);
		if (delta.x * delta.x + delta.z * delta.z > reach * reach) { continue; }
		for (const auto& entry : entries) { entry.mesh.draw(ColorF{ 1 }); }
		if (const auto furniture = m_streetFurnitureCache.find(edgeId); furniture != m_streetFurnitureCache.end())
		{
			for (const auto& batch : furniture->second)
			{
				if (!batch.cable) { batch.mesh.draw(ColorF{ 1 }); }
			}
		}
		if (const auto piers = m_pierMeshCache.find(edgeId); piers != m_pierMeshCache.end())
		{
			for (const auto& mesh : piers->second) { mesh.draw(ColorF{ 1 }); }
		}
		if (const auto signs = m_signCache.find(edgeId); signs != m_signCache.end()) { drawSigns(signs->second); }
		if (const auto signs = m_guideSignCache.find(edgeId); signs != m_guideSignCache.end()) { drawGuideSigns(signs->second); }
	}
	for (const auto& [routeId, signs] : m_routeSignCache)
	{
		for (const auto& sign : signs)
		{
			if (sign.poleTop.distanceFrom(focus) > radius + 40) { continue; }
			if (m_signPoleMesh) { m_signPoleMesh->draw(sign.poleMat, ColorF{ 1 }); }
			if (const Mesh* board = getSignBoardMesh(sign.type)) { board->draw(sign.boardMat, ColorF{ 1 }); }
		}
	}
}

void RoadRenderer::eraseEdgeCaches(int edgeId)
{
	m_constructionCache.erase(edgeId);
	if (m_partMeshCache.contains(edgeId) || m_streetFurnitureCache.contains(edgeId)
		|| m_pierMeshCache.contains(edgeId) || m_signCache.contains(edgeId) || m_guideSignCache.contains(edgeId))
	{
		++m_geometryRevision;
	}
	m_partMeshCache.erase(edgeId);
	m_partLodBatchCache.erase(edgeId);
	m_markingCacheByEdge.erase(edgeId);
	m_streetFurnitureCache.erase(edgeId);
	m_marginCache.erase(edgeId);
	m_boundsCache.erase(edgeId);
	m_pierMeshCache.erase(edgeId);
	m_signCache.erase(edgeId);
	m_guideSignCache.erase(edgeId);
	m_guideSignTexAllReady = false;  // エッジ変更時は案内標識テクスチャを再チェック
}

void RoadRenderer::eraseNodeCaches(int nodeId)
{
	if (m_nodeCapCache.contains(nodeId) || m_signalAttachGeomCache.contains(nodeId)) { ++m_geometryRevision; }
	m_nodeCapCache.erase(nodeId);
	m_markingCacheByNode.erase(static_cast<int64>(nodeId) * 2);
	m_markingCacheByNode.erase(static_cast<int64>(nodeId) * 2 + 1);



	m_signalAttachGeomCache.erase(nodeId);
	m_nodeCapWireCache.erase(nodeId);
}

void RoadRenderer::invalidateEdgeCache(int edgeId, int nodeA, int nodeB)
{
	eraseEdgeCaches(edgeId);
	// route は複数エッジを跨ぐため、どのエッジ変更でも全 route 描画情報を再計算
	m_routeSignCache.clear();
	if (nodeA >= 0 || nodeB >= 0)
	{
		if (nodeA >= 0) eraseNodeCaches(nodeA);
		if (nodeB >= 0) eraseNodeCaches(nodeB);
	}
	else
	{
		m_nodeCapCache.clear();
		m_nodeCapWireCache.clear();
		m_markingCacheByNode.clear();


	}
}

void RoadRenderer::invalidateAllCaches()
{
	m_constructionCache.clear();
	++m_geometryRevision;
	m_dirtyTerrainObserved.clear();
	m_partMeshCache.clear();
	m_partLodBatchCache.clear();
	m_markingCacheByEdge.clear();
	m_streetFurnitureCache.clear();
	m_marginCache.clear();
	m_nodeCapCache.clear();
	m_nodeCapWireCache.clear();
	m_markingCacheByNode.clear();



	m_boundsCache.clear();
	m_signalMeshCache.clear();
	m_signCache.clear();
	m_routeSignCache.clear();
	m_guideSignCache.clear();
	m_guideSignTexAllReady = false;  // 標識内容が変わった可能性があるため再チェック
	m_signalAttachGeomCache.clear();
	m_signPoleMesh.reset();
	m_guidePoleMesh.reset();
	m_signBoardMeshes.clear();
	RoadSign::reloadPoleMetadata();
	GuideSign::reloadPoleMetadata();
}

void RoadRenderer::invalidateCachesAroundNode(int nodeId, const RoadNetwork& network)
{
	eraseNodeCaches(nodeId);
	const RoadNode* node = network.getNode(nodeId);
	if (node)
	{
		for (const auto& att : node->attachments)
			eraseEdgeCaches(att.edgeId);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// エッジ描画
// ─────────────────────────────────────────────────────────────────────────────

void RoadRenderer::drawEdge(const RoadEdge& edge, const RoadNetwork& network,
                             float marginA, float marginB, const World& world, bool isClose)
{
	// エッジ単位で路面部品・車線線・標識・橋脚までの描画責務をまとめる。
	if (edge.edgeState == EdgeState::Planned) return;
	if (edge.edgeState == EdgeState::UnderConstruction)
	{
		drawConstruction(edge, network, world, isClose);
		return;
	}

	// マージンが変わった場合はキャッシュを破棄して再構築する
	if (auto it = m_marginCache.find(edge.id); it != m_marginCache.end())
	{
		if (it->second.atNodeA != marginA || it->second.atNodeB != marginB)
		{
			eraseEdgeCaches(edge.id);
			eraseNodeCaches(edge.nodeA);
			eraseNodeCaches(edge.nodeB);
		}
	}

	// ---- 部品ごとのメッシュを構築・キャッシュ ----
	Stopwatch buildTimer{StartImmediately::Yes};
	auto meshIt = m_partMeshCache.find(edge.id);
	if (meshIt == m_partMeshCache.end())
	{
		const auto bez = network.getBezier(edge.id);
		if (!bez) return;
		auto entries = buildPartMeshes(network, edge, *bez, world, marginA, marginB);
		if (entries.isEmpty()) return;
		++m_geometryRevision;
		meshIt = m_partMeshCache.emplace(edge.id, std::move(entries)).first;
		m_marginCache[edge.id] = { marginA, marginB };
		if (buildTimer.msF()>5) { DBG_LOG(U"[RoadPhase] edge={} partsMs={:.2f}"_fmt(edge.id,buildTimer.msF())); }
		buildTimer.restart();
		// 遠距離用 combined LOD バッチも同じベジェから同時に構築する
		m_partLodBatchCache[edge.id] = buildPartLodBatches(network, edge, *bez, world, marginA, marginB);
	}

	if (buildTimer.msF()>5) { DBG_LOG(U"[RoadPhase] edge={} lodMs={:.2f}"_fmt(edge.id,buildTimer.msF())); }
	// ---- 部品ごとに描画 ----
	if (isClose)
	{
		for (const auto& entry : meshIt->second)
		{
			if (entry.texture)
				entry.meshPair.detail.draw(*entry.texture, entry.color);
			else
				entry.meshPair.detail.draw(entry.color);
		}
	}
	else
	{
		// 遠距離: 同マテリアルを結合した LOD バッチで draw call を削減
		const auto lodIt = m_partLodBatchCache.find(edge.id);
		if (lodIt != m_partLodBatchCache.end())
		{
			for (const auto& batch : lodIt->second)
			{
				if (batch.texture)
					batch.mesh.draw(*batch.texture, batch.color);
				else
					batch.mesh.draw(batch.color);
			}
		}
	}

	// ---- 道路標示（遠方では描画しない） ----
	if (isClose)
	{
		if (!m_markingCacheByEdge.contains(edge.id))
		{
			const auto bez = network.getBezier(edge.id);
			if (!bez) return;
			m_markingCacheByEdge[edge.id] = buildEdgeRoadMarkingBatches(network, edge, *bez, world, marginA, marginB);
		}

		for (const auto& b : m_markingCacheByEdge[edge.id])
			b.mesh.draw(b.color);
	}

	// ---- 道路沿い設備（近距離のみ） ----
	if (isClose)
	{
		if (!m_streetFurnitureCache.contains(edge.id))
		{
			const auto bez = network.getBezier(edge.id);
			if (!bez) return;
			m_streetFurnitureCache[edge.id] = buildStreetFurnitureBatches(edge, *bez, world, marginA, marginB);
		}
		for (const auto& b : m_streetFurnitureCache[edge.id])
		{
			if (b.cable)
			{
				Graphics3D::SetVSConstantBuffer(4, m_cableView);
				const ScopedCustomShader3D shader{ m_cableVS, m_cablePS };
				const ScopedRenderStates3D state{ BlendState::Default2D, RasterizerState::SolidCullNone, DepthStencilState::DepthTest };
				b.mesh.draw(ColorF{ 1 });
			}
			else { b.mesh.draw(b.color); }
		}
	}
	// ---- 道路標識（近距離のみ） ----
	if (isClose && !edge.signs.isEmpty())
	{
		if (!m_signCache.contains(edge.id))
			m_signCache[edge.id] = buildEdgeSignMeshes(network, edge.id, world);
		drawSigns(m_signCache[edge.id]);
	}

	// ---- 案内標識（近距離のみ） ----
	if (isClose)
	{
		if (!m_guideSignCache.contains(edge.id))
			m_guideSignCache[edge.id] = buildEdgeGuideSignDraws(network, edge.id, world);
		if (!m_guideSignCache[edge.id].isEmpty())
			drawGuideSigns(m_guideSignCache[edge.id]);
	}

	// ---- 橋脚描画 ----
	if (edge.useElevation)
	{
		if (!m_pierMeshCache.contains(edge.id))
		{
			const auto bez = network.getBezier(edge.id);
			if (bez)
			{
				Array<Mesh> piers;
				for (const auto& obj : network.objects())
				{
					if (obj.id < 0 || obj.parentEdgeId != edge.id) continue;
					if (obj.type != RoadObjectType::Pier) continue;

					const Vec3 pos = bez->positionAt(Clamp(obj.arcPos, 0.0f, bez->totalLength));
					const Vec3 tan = bez->tangentAt(Clamp(obj.arcPos, 0.0f, bez->totalLength));
					const float terrainY = world.sampleHeight(
						static_cast<float>(pos.x), static_cast<float>(pos.z));
					constexpr float kRoadBedThickness = 0.5f;
					const float topY = static_cast<float>(pos.y + kRoadSurfaceLift) - kRoadBedThickness;
					const float height = topY - terrainY;
					if (height < 1.0f) continue;

					piers << Mesh{BridgeStructure::pier(pos,tangentToRight(tan),terrainY,topY-.65,edge.totalWidth())};
				}
				piers << Mesh{BridgeStructure::girders(*bez,edge.totalWidth(),bez->totalLength)};
				m_pierMeshCache[edge.id] = std::move(piers);
			}
		}

		const auto& meshes=m_pierMeshCache[edge.id];
		for(size_t i=0;i<meshes.size();++i)
		{
			if(i+1==meshes.size()) meshes[i].draw(ColorF{.24,.29,.31}.removeSRGBCurve());
			else meshes[i].draw(m_constructionConcrete,ColorF{1});
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// ノードキャップ描画
// ─────────────────────────────────────────────────────────────────────────────

void RoadRenderer::drawNodeCap(const RoadNetwork& network, int nodeId, const World& world, bool isClose)
{
	const RoadNode* node = network.getNode(nodeId);
	if (!node || node->attachments.size() < 2) return;

	if (!m_nodeCapCache.contains(nodeId))
	{
		auto entries = buildNodeCapParts(network, nodeId, world, 16);
		if (entries.isEmpty()) return;
		m_nodeCapCache[nodeId] = std::move(entries);
	}

	for (const auto& entry : m_nodeCapCache[nodeId])
	{
		const Mesh& mesh = isClose ? entry.meshPair.detail : entry.meshPair.lod;
		if (entry.texture)
			mesh.draw(*entry.texture, entry.color);
		else
			mesh.draw(entry.color);
	}

	// 道路標示（停止線・横断歩道・矢印・ノード内区画線）
	const int64 nodeMarkingCacheKey = static_cast<int64>(nodeId) * 2 + (isClose ? 1 : 0);
	if (!m_markingCacheByNode.contains(nodeMarkingCacheKey))
	{
		m_markingCacheByNode[nodeMarkingCacheKey] = buildNodeRoadMarkingBatches(network, nodeId, world, isClose);
	}
	for (const auto& b : m_markingCacheByNode[nodeMarkingCacheKey])
	{
		b.mesh.draw(b.color);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// ヘルパー
// ─────────────────────────────────────────────────────────────────────────────

float RoadRenderer::edgeMargin(const RoadEdge& edge, int nodeId)
{
	return (nodeId == edge.nodeA) ? edge.cutoffA : edge.cutoffB;
}

RoadRenderer::PartVisual RoadRenderer::getPartVisual(const RoadPart& part) const
{
	float heightOff = 0.0f;
	ColorF color{ 0.35 };
	const Texture* tex = nullptr;

	if (!part.defId.isEmpty())
	{
		const auto& def = m_partRegistry.get(part.defId);
		heightOff = def.heightOffset;
		color     = def.color;
		if (def.texture) tex = &(*def.texture);
	}
	else
	{
		getPartDefaults(part.type, color, heightOff);
	}

	// Registry colors are authored in sRGB; the HDR lighting pass expects linear albedo.
	if (part.type != RoadPartType::Roadbed) { color = color.removeSRGBCurve(); }
	return { color, heightOff, tex };
}

// ─────────────────────────────────────────────────────────────────────────────
// メッシュ生成
// ─────────────────────────────────────────────────────────────────────────────

MeshData RoadRenderer::buildStripMesh(const CubicBezier& bezier, const World& world,
                                      float offsetL, float offsetR, float heightOffset,
                                      float sStart, float sEnd, float lodFactor,
                                      bool useElevation) const
{
	const float spanLen = sEnd - sStart;
	if (spanLen <= 0.1f) return MeshData{};

	const int N = Clamp(static_cast<int>(spanLen / 2.0f * lodFactor) + 1,
	                    3, static_cast<int>(100 * lodFactor));

	Array<Vertex3D> vertices;
	vertices.reserve((N + 1) * 2);

	for (int i = 0; i <= N; ++i)
	{
		const float s  = sStart + (i / static_cast<float>(N)) * spanLen;
		const auto  sl = makeSlice(bezier, world, s, kRoadSurfaceLift + static_cast<double>(heightOffset), useElevation);
		const Vec3  pL = sl.center + sl.right * static_cast<double>(offsetL);
		const Vec3  pR = sl.center + sl.right * static_cast<double>(offsetR);
		vertices << makeVertWorldUV(pL);
		vertices << makeVertWorldUV(pR);
	}

	Array<TriangleIndex32> indices;
	indices.reserve(N * 2);
	for (int i = 0; i < N; ++i)
	{
		appendQuad(indices,
		           static_cast<uint32>(i * 2),
		           static_cast<uint32>(i * 2 + 1),
		           static_cast<uint32>(i * 2 + 2),
		           static_cast<uint32>(i * 2 + 3));
	}

	return MeshData{ vertices, indices };
}

MeshData RoadRenderer::buildStripMeshTapered(const CubicBezier& bezier, const World& world,
                                              float offsetA_L, float offsetA_R,
                                              float offsetB_L, float offsetB_R,
                                              float heightOffset,
                                              float sStart, float sEnd, float lodFactor,
                                              float sideSkirtDrop,
                                              bool useElevation) const
{
	const float spanLen = sEnd - sStart;
	if (spanLen <= 0.1f) return MeshData{};

	const float totalLength = bezier.totalLength;

	const int N = Clamp(static_cast<int>(spanLen / 2.0f * lodFactor) + 1,
	                    3, static_cast<int>(100 * lodFactor));
	const bool addSideSkirts = (sideSkirtDrop > 0.001f);

	Array<Vertex3D> vertices;
	vertices.reserve((N + 1) * (addSideSkirts ? 6 : 2));

	for (int i = 0; i <= N; ++i)
	{
		const float s  = sStart + (i / static_cast<float>(N)) * spanLen;
		const float t  = (totalLength > 0.0f) ? Clamp(s / totalLength, 0.0f, 1.0f) : 0.0f;
		const float oL = Math::Lerp(offsetA_L, offsetB_L, t);
		const float oR = Math::Lerp(offsetA_R, offsetB_R, t);
		const auto  sl = makeSlice(bezier, world, s, kRoadSurfaceLift + static_cast<double>(heightOffset), useElevation);
		Vec3 pL = sl.center + sl.right * static_cast<double>(oL);
		Vec3 pR = sl.center + sl.right * static_cast<double>(oR);
		if (!useElevation)
		{
			pL.y = world.sampleHeight(static_cast<float>(pL.x), static_cast<float>(pL.z)) + kRoadSurfaceLift + static_cast<double>(heightOffset);
			pR.y = world.sampleHeight(static_cast<float>(pR.x), static_cast<float>(pR.z)) + kRoadSurfaceLift + static_cast<double>(heightOffset);
		}
		vertices << makeVertWorldUV(pL);
		vertices << makeVertWorldUV(pR);
	}

	Array<TriangleIndex32> indices;
	indices.reserve(N * (addSideSkirts ? 6 : 2));
	for (int i = 0; i < N; ++i)
	{
		appendQuad(indices,
		           static_cast<uint32>(i * 2),
		           static_cast<uint32>(i * 2 + 1),
		           static_cast<uint32>(i * 2 + 2),
		           static_cast<uint32>(i * 2 + 3));
	}

	if (addSideSkirts)
	{
		const uint32 sideBase = static_cast<uint32>(vertices.size());
		for (int i = 0; i <= N; ++i)
		{
			const float s  = sStart + (i / static_cast<float>(N)) * spanLen;
			const float t  = (totalLength > 0.0f) ? Clamp(s / totalLength, 0.0f, 1.0f) : 0.0f;
			const float oL = Math::Lerp(offsetA_L, offsetB_L, t);
			const float oR = Math::Lerp(offsetA_R, offsetB_R, t);
			const auto  sl = makeSlice(bezier, world, s, kRoadSurfaceLift + static_cast<double>(heightOffset), useElevation);
			Vec3 pL = sl.center + sl.right * static_cast<double>(oL);
			Vec3 pR = sl.center + sl.right * static_cast<double>(oR);
			if (!useElevation)
			{
				pL.y = world.sampleHeight(static_cast<float>(pL.x), static_cast<float>(pL.z)) + kRoadSurfaceLift + static_cast<double>(heightOffset);
				pR.y = world.sampleHeight(static_cast<float>(pR.x), static_cast<float>(pR.z)) + kRoadSurfaceLift + static_cast<double>(heightOffset);
			}
			const Vec3  bL{ pL.x, pL.y - static_cast<double>(sideSkirtDrop), pL.z };
			const Vec3  bR{ pR.x, pR.y - static_cast<double>(sideSkirtDrop), pR.z };
			const Float3 leftNormal{ static_cast<float>(-sl.right.x), 0.0f, static_cast<float>(-sl.right.z) };
			const Float3 rightNormal{ static_cast<float>(sl.right.x), 0.0f, static_cast<float>(sl.right.z) };
			vertices << makeVertWorldUVWithNormal(pL, leftNormal);
			vertices << makeVertWorldUVWithNormal(bL, leftNormal);
			vertices << makeVertWorldUVWithNormal(pR, rightNormal);
			vertices << makeVertWorldUVWithNormal(bR, rightNormal);
		}
		for (int i = 0; i < N; ++i)
		{
			const uint32 a = sideBase + static_cast<uint32>(i * 4);
			const uint32 b = sideBase + static_cast<uint32>((i + 1) * 4);
			indices << TriangleIndex32{ a, b + 1, a + 1 };
			indices << TriangleIndex32{ a, b, b + 1 };
			indices << TriangleIndex32{ a + 2, a + 3, b + 3 };
			indices << TriangleIndex32{ a + 2, b + 3, b + 2 };
		}
	}

	return MeshData{ vertices, indices };
}
Array<PartMeshEntry> RoadRenderer::buildPartMeshes(const RoadNetwork& network, const RoadEdge& edge, const CubicBezier& bezier,
                                                    const World& world,
                                                    float marginA, float marginB)
{
	// 断面部品をテーパー付きの帯メッシュへ展開し、近景と遠景の両 LOD を同時に用意する。
	StripRange range;
	if (!calcStripRange(bezier.totalLength, marginA, marginB, range)) return {};

	Array<PartMeshEntry> entries;
	const MeshData overlappingPavement = adjacentPavement(network,edge,world);

	for (const auto& part : edge.parts)
	{
		if (part.build != BuildState::Built || part.placement != RoadPartPlacement::Strip) continue;

		auto [color, heightOff, tex] = getPartVisual(part);
		if (part.type == RoadPartType::Roadbed)
		{
			if (edge.roadType == RoadType::Arterial) color = ColorF{ 0.29, 0.30, 0.32 };
			else if (edge.roadType == RoadType::LocalRoad) color = ColorF{ 0.29, 0.30, 0.32 };
		}

		const RoadPartDef& def = m_partRegistry.get(part.defId);
		const RoadPartModel& model = m_partRegistry.getModel(part.defId);
		MeshData mdDetail;
		MeshData mdLod;
		if (part.type != RoadPartType::Roadbed && !model.center.isEmpty())
		{
			mdDetail = buildRoadPartModelStrip(bezier, world, def, model.center,
				part.offsetA_L, part.offsetA_R, part.offsetB_L, part.offsetB_R,
				range.sStart, range.sEnd, 1.0f, edge.useElevation);
			const PartModelData& lodModel = (!model.lods.isEmpty() && !model.lods[0].center.isEmpty()) ? model.lods[0].center : model.center;
			mdLod = buildRoadPartModelStrip(bezier, world, def, lodModel,
				part.offsetA_L, part.offsetA_R, part.offsetB_L, part.offsetB_R,
				range.sStart, range.sEnd, 0.25f, edge.useElevation);
		}
		else
		{
			mdDetail = buildStripMeshTapered(bezier, world,
				part.offsetA_L, part.offsetA_R,
				part.offsetB_L, part.offsetB_R,
				heightOff,
				range.sStart, range.sEnd, 1.0f, roadPartSideSkirtDrop(part.type, heightOff), edge.useElevation);
			mdLod = buildStripMeshTapered(bezier, world,
				part.offsetA_L, part.offsetA_R,
				part.offsetB_L, part.offsetB_R,
				heightOff,
				range.sStart, range.sEnd, 0.25f, roadPartSideSkirtDrop(part.type, heightOff), edge.useElevation);
		}
		if (part.type != RoadPartType::Roadbed && !overlappingPavement.indices.isEmpty())
		{
			mdDetail = RoadGeometry::excludeSurface(mdDetail,overlappingPavement);
			mdLod = RoadGeometry::excludeSurface(mdLod,overlappingPavement);
		}
		if (mdDetail.vertices.isEmpty()) continue;

		PartMeshEntry entry;
		orientRoadFaces(mdDetail);
		orientRoadFaces(mdLod);
		entry.meshPair.detail = Mesh{ mdDetail };
		entry.meshPair.lod    = mdLod.vertices.isEmpty() ? Mesh{ mdDetail } : Mesh{ mdLod };
		entry.color   = color;
		entry.texture = tex;
		entries << std::move(entry);
	}

	return entries;
}

Array<PartLodBatch> RoadRenderer::buildPartLodBatches(const RoadNetwork& network, const RoadEdge& edge, const CubicBezier& bezier,
                                                       const World& world,
                                                       float marginA, float marginB) const
{
	const MeshData overlappingPavement = adjacentPavement(network,edge,world);
	StripRange range;
	if (!calcStripRange(bezier.totalLength, marginA, marginB, range)) return {};

	// 同マテリアル（tex + color）の部品メッシュを 1 バッファに統合して draw call を削減する
	struct MaterialGroup
	{
		const Texture* tex;
		ColorF         color;
		MeshData       md;
	};
	Array<MaterialGroup> groups;

	for (const auto& part : edge.parts)
	{
		if (part.build != BuildState::Built || part.placement != RoadPartPlacement::Strip) continue;

		auto [color, heightOff, tex] = getPartVisual(part);
		if (part.type == RoadPartType::Roadbed)
		{
			if (edge.roadType == RoadType::Arterial) color = ColorF{ 0.29, 0.30, 0.32 };
			else if (edge.roadType == RoadType::LocalRoad) color = ColorF{ 0.29, 0.30, 0.32 };
		}

		const RoadPartDef& def = m_partRegistry.get(part.defId);
		const RoadPartModel& model = m_partRegistry.getModel(part.defId);
		MeshData md;
		if (part.type != RoadPartType::Roadbed && !model.center.isEmpty())
		{
			const PartModelData& lodModel = (!model.lods.isEmpty() && !model.lods[0].center.isEmpty()) ? model.lods[0].center : model.center;
			md = buildRoadPartModelStrip(bezier, world, def, lodModel,
				part.offsetA_L, part.offsetA_R, part.offsetB_L, part.offsetB_R,
				range.sStart, range.sEnd, 0.25f, edge.useElevation);
		}
		else
		{
			md = buildStripMeshTapered(bezier, world,
				part.offsetA_L, part.offsetA_R,
				part.offsetB_L, part.offsetB_R,
				heightOff,
				range.sStart, range.sEnd, 0.25f, roadPartSideSkirtDrop(part.type, heightOff), edge.useElevation);
		}
		if (part.type != RoadPartType::Roadbed && !overlappingPavement.indices.isEmpty())
		{
			md = RoadGeometry::excludeSurface(md,overlappingPavement);
		}
		if (md.vertices.isEmpty()) continue;

		// 既存グループを検索（部品数は通常 ≤10 なので線形探索で十分）
		MaterialGroup* group = nullptr;
		for (auto& g : groups)
		{
			if (g.tex == tex && g.color == color)
			{
				group = &g;
				break;
			}
		}
		if (!group)
		{
			groups << MaterialGroup{ tex, color, MeshData{} };
			group = &groups.back();
		}

		const uint32 indexBase = static_cast<uint32>(group->md.vertices.size());
		group->md.vertices.append(md.vertices);
		for (const auto& tri : md.indices)
		{
			group->md.indices << TriangleIndex32{ tri.i0 + indexBase, tri.i1 + indexBase, tri.i2 + indexBase };
		}
	}

	Array<PartLodBatch> batches;
	batches.reserve(groups.size());
	for (auto& g : groups)
	{
		if (g.md.vertices.isEmpty()) continue;
		orientRoadFaces(g.md);
		batches << PartLodBatch{ Mesh{ g.md }, g.color, g.tex };
	}
	return batches;
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
			                  sStart, sEnd, static_cast<float>(kRoadLineLift), edge.useElevation);
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
			                  sStart, sEnd, static_cast<float>(kRoadLineLift), edge.useElevation);
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
			                  static_cast<float>(kRoadLineLift), edge.useElevation);
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

Array<RoadRenderer::LaneLineBatch> buildLaneLineBatchesFromPlacements(
	const RoadNetwork& network, const RoadEdge& edge, const CubicBezier& bezier, const World& world,
	float marginA, float marginB, const Array<RoadMarkingPlacement>& placements);
Array<RoadRenderer::LaneLineBatch> buildCrosswalkBatchesFromPlacements(
	const RoadNetwork& network, const Array<RoadMarkingPlacement>& placements, const World& world);

Array<RoadRenderer::LaneLineBatch> buildLaneLineBatchesFromPlacements(
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
			style.dashLen, style.gapLen, sStart, sEnd, static_cast<float>(kRoadLineLift), edge.useElevation);
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

Array<RoadRenderer::LaneLineBatch> RoadRenderer::buildStreetFurnitureBatches(
	const RoadEdge& edge, const CubicBezier& bezier,
	const World& world,
	float marginA, float marginB) const
{
	StripRange range;
	if (!calcStripRange(bezier.totalLength, marginA, marginB, range)) return {};

	MeshData poleMd;
	MeshData cableMd;
	MeshData roadsideMd;
	MeshData markerMd;

	for (size_t partIndex = 0; partIndex < edge.parts.size(); ++partIndex)
	{
		const RoadPart& part = edge.parts[partIndex];
		if (part.build != BuildState::Built || part.placement != RoadPartPlacement::RepeatAlongEdge)
		{
			continue;
		}

		const float offset = (part.offsetL() + part.offsetR()) * 0.5f;
		float spacing = part.repeatSpacing;
		float jitter = part.repeatJitter;
		if (!part.defId.isEmpty())
		{
			const RoadPartDef& def = m_partRegistry.get(part.defId);
			if (part.useDefinitionRepeatSpacing) spacing = def.repeatSpacing;
			if (part.useDefinitionRepeatJitter) jitter = def.repeatJitter;
		}
		spacing = Max(spacing, 4.0f);
		jitter = Max(jitter, 0.0f);
		const float objectHalfDepth = (part.type == RoadPartType::UtilityPole) ? 0.12f : 0.35f;
		const float sStart = range.sStart + spacing * 0.5f;
		const float sEnd = range.sEnd - spacing * 0.25f;
		if (sEnd <= sStart) continue;

		Optional<Vec3> previousPole;
		Vec2 previousRight{ 0, 0 };
		const int sampleCount = Max(1, static_cast<int>((sEnd - sStart) / spacing));
		for (int i = 0; i <= sampleCount; ++i)
		{
			const float jitterOffset = (jitter > 0.0f)
				? (static_cast<float>(((edge.id * 73856093) ^ (static_cast<int>(partIndex) * 19349663) ^ (i * 83492791)) & 1023) / 1023.0f - 0.5f) * jitter
				: 0.0f;
			const float s = Clamp(sStart + static_cast<float>(i) * spacing + jitterOffset, range.sStart, range.sEnd);
			const Vec3 pos = bezier.positionAt(s);
			const Vec3 tan3 = bezier.tangentAt(s);
			Vec2 tangent{ static_cast<float>(tan3.x), static_cast<float>(tan3.z) };
			if (tangent.lengthSq() <= 1e-8f) continue;
			tangent.normalize();
			const Vec2 right{ tangent.y, -tangent.x };
			const float yaw = static_cast<float>(std::atan2(tangent.y, tangent.x));
			const double anchorX = pos.x + right.x * static_cast<double>(offset);
			const double anchorZ = pos.z + right.y * static_cast<double>(offset);
			const double terrainY = edge.useElevation
				? pos.y
				: world.sampleHeight(static_cast<float>(anchorX), static_cast<float>(anchorZ));
			const float groundY = static_cast<float>(RoadGeometry::furnitureBaseY(edge, pos, terrainY));

			if (part.type == RoadPartType::UtilityPole)
			{
				const Vec3 poleCenter{ anchorX, groundY + 5.1f, anchorZ };
				const float armYaw = yaw + static_cast<float>(Math::HalfPi);
				auto shaft = MeshData::Cylinder(Float3{poleCenter},0.18,10.2,12);
				for (auto& vertex : shaft.vertices)
				{
					const float taper = Math::Lerp(1.0f,0.6f,Clamp((vertex.pos.y-groundY)/10.2f,0.0f,1.0f));
					vertex.pos.x = static_cast<float>(anchorX + (vertex.pos.x-anchorX)*taper);
					vertex.pos.z = static_cast<float>(anchorZ + (vertex.pos.z-anchorZ)*taper);
				}
				appendMeshData(poleMd,shaft);
				for (const double level : { 3.6, 2.8 })
				{
					appendOrientedBox(roadsideMd,poleCenter+Vec3{0,level,0},Float3{1.9f,0.10f,0.13f},armYaw);
					appendCylinder(roadsideMd,poleCenter+Vec3{0,level,0},0.19,0.12);
					for (const double armOffset : { -0.75,0.0,0.75 })
					{
						const Vec3 anchor = poleCenter + Vec3{right.x*armOffset,level+.18,right.y*armOffset};
						appendCylinder(markerMd,anchor,.055,.32,8);
						for (const double ring : { -.10,0.0,.10 }) { appendCylinder(markerMd,anchor+Vec3{0,ring,0},.10,.055,8); }
					}
				}
				// Pole-mounted transformer, mounting straps, bushings and service conduit.
				const Vec3 transformer = poleCenter + Vec3{-right.x*.43,1.65,-right.y*.43};
				appendCylinder(roadsideMd,transformer,.27,.92);
				appendCylinder(roadsideMd,transformer+Vec3{0,.49,0},.31,.07);
				appendCylinder(markerMd,transformer+Vec3{0,.63,0},.065,.22,8);
				appendOrientedBox(roadsideMd,poleCenter+Vec3{-right.x*.20,1.55,-right.y*.20},Float3{.65f,.09f,.15f},armYaw);
				appendCylinder(roadsideMd,Vec3{anchorX+right.x*.19,groundY+2.5,anchorZ+right.y*.19},.022,4.6,6);
				appendOrientedBox(markerMd,Vec3{anchorX,groundY+1.3,anchorZ},Float3{.23f,.35f,.23f},yaw);
				for (int rung = 0; rung < 11; ++rung)
				{
					const float side = rung%2 == 0 ? -1.0f : 1.0f;
					appendOrientedBox(roadsideMd,Vec3{anchorX+right.x*.22*side,groundY+2.1+rung*.52,anchorZ+right.y*.22*side},Float3{.23f,.032f,.035f},armYaw);
				}
				if (previousPole)
				{
					for (const float cableSide : { -0.75f, 0.0f, 0.75f })
					{
						const Vec3 from = *previousPole + Vec3{ previousRight.x * cableSide, 3.95, previousRight.y * cableSide };
						const Vec3 to = poleCenter + Vec3{ right.x * cableSide, 3.95, right.y * cableSide };
						const double sag = Min(0.65, from.distanceFrom(to) * 0.015);
						constexpr int kCableSegments = 6;
						Vec3 previous = from;
						for (int segment = 1; segment <= kCableSegments; ++segment)
						{
							const double fraction = static_cast<double>(segment) / kCableSegments;
							const Vec3 current = from.lerp(to, fraction) + Vec3{ 0, -4.0 * sag * fraction * (1.0 - fraction), 0 };
							const Vec3 delta = current - previous;
							const double length = delta.length();
							if (length > 0.001)
							{
								const uint32 base = static_cast<uint32>(cableMd.vertices.size());
								const Float3 direction{ delta / length };
								for (const Vec3 endpoint : { previous, current })
								{
									for (const float side : { -1.0f, 1.0f })
									{
										cableMd.vertices << Vertex3D{ Float3{ endpoint }, direction, Float2{ side, 0.014f } };
									}
								}
								cableMd.indices << TriangleIndex32{ base, base + 1, base + 2 } << TriangleIndex32{ base + 2, base + 1, base + 3 };
							}
							previous = current;
						}
					}
				}
				previousPole = poleCenter;
				previousRight = right;
			}
			else if (part.type == RoadPartType::RoadsideObject)
			{
				const Vec3 center{ anchorX, groundY + 0.85f, anchorZ };
				appendOrientedBox(roadsideMd, center, Float3{ 0.45f, 1.70f, objectHalfDepth * 2.0f }, yaw);
			}
		}
	}

	Array<LaneLineBatch> batches;
	if (!poleMd.vertices.isEmpty()) batches << LaneLineBatch{ ColorF{ 0.57, 0.57, 0.53 }.removeSRGBCurve(), Mesh{ poleMd } };
	if (!cableMd.vertices.isEmpty()) batches << LaneLineBatch{ ColorF{ 0.05, 0.05, 0.045 }.removeSRGBCurve(), Mesh{ cableMd }, true };
	if (!markerMd.vertices.isEmpty()) batches << LaneLineBatch{ ColorF{ 0.82, 0.78, 0.64 }.removeSRGBCurve(), Mesh{ markerMd } };
	if (!roadsideMd.vertices.isEmpty()) batches << LaneLineBatch{ ColorF{ 0.30, 0.33, 0.33 }.removeSRGBCurve(), Mesh{ roadsideMd } };
	return batches;
}

Array<PartMeshEntry> RoadRenderer::buildNodeCapParts(const RoadNetwork& network, int nodeId,
                                                     const World& world, [[maybe_unused]] int div,
                                                     bool onlyOpenEdges)
{
	const auto layout = JunctionGeometry::build(network, nodeId, onlyOpenEdges, &world);
	Array<PartMeshEntry> entries;
	if (layout.asphalt.vertices.isEmpty()) { return entries; }
	auto addEntry = [&](MeshData mesh, ColorF color, const Texture* texture)
	{
		if (mesh.vertices.isEmpty() || mesh.indices.isEmpty()) { return; }
		orientRoadFaces(mesh);
		PartMeshEntry entry;
		entry.meshPair.detail = Mesh{ mesh };
		// Keep the same boundary at both distances so the terrain cut never opens a gap.
		entry.meshPair.lod = entry.meshPair.detail;
		entry.color = color;
		entry.texture = texture;
		entries << std::move(entry);
	};
	MeshData asphalt = layout.asphalt;
	for (auto& vertex : asphalt.vertices)
	{
		if (!layout.elevated) { vertex.pos.y = static_cast<float>(world.sampleHeight(vertex.pos.x, vertex.pos.z) + kRoadSurfaceLift); }
	}
	const auto& roadbedDefinition = m_partRegistry.get(U"roadbed_asphalt");
	ColorF asphaltColor{ 0.29, 0.30, 0.32 };
	for (const auto& attachment : network.getNode(nodeId)->attachments)
	{
		const auto* edge = network.getEdge(attachment.edgeId);
		if (edge && edge->roadType == RoadType::Arterial) { asphaltColor = ColorF{ 0.29, 0.30, 0.32 }; break; }
	}
	addEntry(std::move(asphalt), asphaltColor, roadbedDefinition.texture ? &*roadbedDefinition.texture : nullptr);
	// Acute arms may overlap beyond their nominal cut mouths. Clip roadside
	// bands against the entire connected pavement, not only the cap polygon.
	MeshData connectedPavement = layout.asphalt;
	for (const auto& attachment : network.getNode(nodeId)->attachments)
	{
		const auto* edge = network.getEdge(attachment.edgeId);
		if (!edge || !edge->isRoadbedBuilt() || (edge->edgeState != EdgeState::Open && edge->edgeState != EdgeState::Existing)) { continue; }
		if (const auto bezier = network.getBezier(edge->id)) { appendMeshData(connectedPavement,RoadGeometry::roadbedSurface(*edge,*bezier,world)); }
	}
	for (const auto& corner : layout.corners)
	{
		for (const auto& band : corner.bands)
		{
			const auto [color, heightOffset, texture] = getPartVisual(band.part);
			const auto& definition = m_partRegistry.get(band.part.defId);
			const auto& model = m_partRegistry.getModel(band.part.defId).center;
			MeshData mesh;
			for (size_t segment = 1; segment < corner.sections.size(); ++segment)
			{
				const auto& start = corner.sections[segment-1];
				const auto& end = corner.sections[segment];
				const uint32 base = static_cast<uint32>(mesh.vertices.size());
				if (!model.isEmpty())
				{
					for (const auto& source : model.vertices)
					{
						const double across = Clamp(source.pos.x / Max(0.001f, definition.modelUnitWidth), 0.0f, 1.0f);
						const double along = Clamp(source.pos.z / Max(0.1f, definition.modelUnitLen), 0.0f, 1.0f);
						Vec3 position = JunctionGeometry::bandPosition(start, band, across).lerp(JunctionGeometry::bandPosition(end, band, across), along);
						if (!layout.elevated) { position.y = world.sampleHeight(static_cast<float>(position.x), static_cast<float>(position.z)) + kRoadSurfaceLift; }
						position.y += source.pos.y + heightOffset;
						Vertex3D vertex = makeVertWorldUV(position);
						const Vec3 outward = start.outward.lerp(end.outward, along).normalized();
						const Vec3 alongVector{ -outward.z, 0.0, outward.x };
						vertex.normal = Float3{ outward * source.normal.x + Vec3{ 0.0, source.normal.y, 0.0 } + alongVector * source.normal.z };
						mesh.vertices << vertex;
					}
					for (const auto& triangle : model.indices)
					{
						mesh.indices << TriangleIndex32{ base+triangle.i0, base+triangle.i1, base+triangle.i2 };
					}
				}
				else
				{
					for (const auto& section : { start, end })
					{
						for (const double across : { 0.0, 1.0 })
						{
							Vec3 position = JunctionGeometry::bandPosition(section, band, across);
							if (!layout.elevated) { position.y = world.sampleHeight(static_cast<float>(position.x), static_cast<float>(position.z)) + kRoadSurfaceLift; }
							position.y += heightOffset;
							mesh.vertices << makeVertWorldUV(position);
						}
					}
					appendQuad(mesh.indices, base, base+1, base+2, base+3);
				}
			}
			mesh = RoadGeometry::excludeSurface(mesh,connectedPavement);
			addEntry(std::move(mesh), color, texture);
		}
	}
	return entries;
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
	const bool anyElev = network.isNodeElevated(nodeId);
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

		const double lineY = edge->useElevation
			? pos.y + kRoadLineLift
			: world.sampleHeight(static_cast<float>(pos.x), static_cast<float>(pos.z)) + kRoadLineLift;

		const Vec3 p0{ pos.x + right.x * entryMin, lineY, pos.z + right.z * entryMin };
		const Vec3 p1{ pos.x + right.x * entryMax, lineY, pos.z + right.z * entryMax };
		appendStraightLine(batches, p0, p1, 0.3f, ColorF{ 1.0, 1.0, 1.0 });
	}
	return batches;
}

Array<RoadRenderer::LaneLineBatch> buildCrosswalkBatchesFromPlacements(
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

bool RoadRenderer::computeSignTransforms(const CubicBezier& bezier, const World& world,
	float arcLen, float lateralOffset, bool boardFacesTan,
	float boardOffsetX, float boardOffsetY, float boardOffsetZ,
	bool useElevation, Mat4x4& outPole, Mat4x4& outBoard, Vec3& outPoleTop)
{
	if (arcLen < 0.0f || arcLen > bezier.totalLength) return false;

	const Vec3 roadPos = bezier.positionAt(arcLen);
	const Vec3 rawTan  = bezier.tangentAt(arcLen);
	const Vec3 right   = tangentToRight(rawTan);

	const double anchorX = roadPos.x + right.x * static_cast<double>(lateralOffset);
	const double anchorZ = roadPos.z + right.z * static_cast<double>(lateralOffset);
	// 標識の接地 Y は Roadbed の高さに合わせる（路面リフト込み）
	// 高架: bezier Y / 非高架: 地形 Y
	const double baseY = useElevation
		? roadPos.y
		: static_cast<double>(world.sampleHeight(static_cast<float>(anchorX), static_cast<float>(anchorZ)));
	const double groundY = baseY + kRoadSurfaceLift;

	const Vec3   frontDir = boardFacesTan ? rawTan : -rawTan;
	const double dLen = Math::Sqrt(frontDir.x * frontDir.x + frontDir.z * frontDir.z);
	if (dLen < 1e-6) return false;
	const float yaw = static_cast<float>(Math::Atan2(frontDir.x / dLen, frontDir.z / dLen));

	// ポール: OBJ 実寸で配置（スケール無し）、yaw のみ適用
	outPole = Mat4x4::RotateY(yaw)
		* Mat4x4::Translate(Float3{
			static_cast<float>(anchorX),
			static_cast<float>(groundY),
			static_cast<float>(anchorZ) });

	// 看板: local (offsetX, offsetY, offsetZ) → world
	// 行ベクトル規約 (v' = v * M) で以下の順に適用:
	//   1) local 座標に offset を加算 (Translate(offset))
	//   2) RotateY(rotated) — yaw+π で local +Z を driver 反対方向（= -frontDir）に向け、
	//      看板正面（local -Z）が driver 方向を向くようにする
	//   3) anchor へ平行移動
	// 軸: X=driver から見た横方向, Y=垂直, Z=道路の長手方向（driver 逆向き）
	const float rotated = yaw + static_cast<float>(Math::Pi);
	outBoard = Mat4x4::Translate(Float3{ boardOffsetX, boardOffsetY, boardOffsetZ })
		* Mat4x4::RotateY(rotated)
		* Mat4x4::Translate(Float3{
			static_cast<float>(anchorX),
			static_cast<float>(groundY),
			static_cast<float>(anchorZ) });
	outPoleTop = Vec3{ anchorX, groundY + boardOffsetY, anchorZ };
	return true;
}

Array<RoadRenderer::SignDraw> RoadRenderer::buildEdgeSignMeshes(
	const RoadNetwork& network, int edgeId, const World& world) const
{
	const RoadEdge* edge = network.getEdge(edgeId);
	if (!edge || !edge->isRoadbedBuilt()) return {};
	const auto bez = network.getBezier(edgeId);
	if (!bez) return {};

	Array<SignDraw> batches;

	for (const auto& sp : edge->signs)
	{
		if (sp.type == RoadSignType::None) continue;

		const bool atA = (sp.nodeEndId == edge->nodeA);
		const bool atB = (sp.nodeEndId == edge->nodeB);
		if (!atA && !atB) continue;

		// 弧長位置: cutoff + arcOffset を内側方向に
		const float cutoff = atA ? edge->cutoffA : edge->cutoffB;
		const float arc = atA
			? (cutoff + sp.arcOffset)
			: (bez->totalLength - cutoff - sp.arcOffset);

		// 看板は driver に向ける: nodeA 側 → driver は B→A → 看板正面は +tan
		//                          nodeB 側 → driver は A→B → 看板正面は -tan
		const bool entering=sp.type==RoadSignType::NoEntry || sp.type==RoadSignType::OneWay;
		const bool boardFacesTan = entering ? !atA : atA;

		Mat4x4 poleMat, boardMat;
		Vec3   poleTop;
		const auto& meta = RoadSign::poleMetadata();
		if (!computeSignTransforms(*bez, world, arc, sp.lateralOffset, boardFacesTan,
		                           meta.offsetX, meta.offsetY, meta.offsetZ,
		                           edge->useElevation, poleMat, boardMat, poleTop))
			continue;

		SignDraw signDraw;
		signDraw.poleMat  = poleMat;
		signDraw.boardMat = boardMat;
		signDraw.poleTop  = poleTop;
		signDraw.type     = sp.type;
		signDraw.auxNumber = sp.auxValue;
		batches << signDraw;
	}

	return batches;
}

Array<RoadRenderer::SignDraw> RoadRenderer::buildRouteSignDraws(
	const RoadRoute& route, const RoadNetwork& network, const World& world) const
{
	Array<SignDraw> out;
	if (route.kind != RoadRouteKind::NationalRoute) return out;
	if (route.number <= 0 || route.edgeIds.isEmpty()) return out;

	if (!m_routeSignTexCache.contains(route.number)) return out;

	const auto& routeMeta = RoadSign::poleMetadata();

	for (const auto& [edgeId, arcLen] : network.routeSignAnchors(route))
	{
		const auto bezier = network.getBezier(edgeId);
		if (!bezier) continue;

		const RoadEdge* edge = network.getEdge(edgeId);
		const float roadRightEdge = edge
			? RoadSign::roadbedExtentsOf(*edge).right
			: 5.0f;
		const float lateral = roadRightEdge + static_cast<float>(RoadSign::kSideMargin_m);

		Mat4x4 poleMat, boardMat;
		Vec3   poleTop;
		if (!computeSignTransforms(*bezier, world, arcLen, lateral,
		                           /*boardFacesTan=*/false,
		                           routeMeta.offsetX, routeMeta.offsetY, routeMeta.offsetZ,
		                           edge ? edge->useElevation : false,
		                           poleMat, boardMat, poleTop))
			continue;

		SignDraw signDraw;
		signDraw.poleMat   = poleMat;
		signDraw.boardMat  = boardMat;
		signDraw.poleTop   = poleTop;
		signDraw.type      = RoadSignType::NationalRoute;
		signDraw.auxNumber = route.number;
		out << signDraw;
	}
	return out;
}

const Mesh* RoadRenderer::getSignBoardMesh(RoadSignType type)
{
	if (type == RoadSignType::None) return nullptr;
	const auto& vis = RoadSign::visualOf(type);
	if (vis.shapeObjPath.isEmpty()) return nullptr;

	const String key{ vis.shapeObjPath };
	if (auto it = m_signBoardMeshes.find(key); it != m_signBoardMeshes.end())
		return &it->second;

	const MeshData md = RoadSign::CreateBoardMesh(type);
	if (md.vertices.isEmpty()) return nullptr;
	return &m_signBoardMeshes.emplace(key, Mesh{ md }).first->second;
}

void RoadRenderer::drawSigns(const Array<SignDraw>& draws)
{
	if (draws.isEmpty()) return;

	// OBJ ロード（遅延初期化）
	if (not m_signPoleMesh)
	{
		const auto parsed = ObjParser::parse(U"assets/signs/sign_pole.obj");
		MeshData combined;
		for (const auto& pd : parsed)
		{
			if (pd.isEmpty()) continue;
			const uint32 base = static_cast<uint32>(combined.vertices.size());
			combined.vertices.insert(combined.vertices.end(), pd.vertices.begin(), pd.vertices.end());
			for (const auto& t : pd.indices)
				combined.indices << TriangleIndex32{ base + t.i0, base + t.i1, base + t.i2 };
		}
		if (!combined.vertices.isEmpty()) m_signPoleMesh = Mesh{ combined };
	}

	// ポール
	for (const auto& signDraw : draws)
	{
		if (m_signPoleMesh) m_signPoleMesh->draw(signDraw.poleMat, RoadSign::kPoleColor.removeSRGBCurve());
	}

	// 看板裏面: 灰色（前面ポリゴンをカリングして裏面のみ表示）
	{
		const ScopedRenderStates3D states{ RasterizerState::SolidCullFront };
		for (const auto& signDraw : draws)
		{
			const Mesh* boardMesh = getSignBoardMesh(signDraw.type);
			if (!boardMesh) continue;
			boardMesh->draw(signDraw.boardMat, ColorF{ 0.55 }.removeSRGBCurve());
		}
	}

	// 看板前面: テクスチャ（裏面ポリゴンをカリングして前面のみ表示）
	{
		const ScopedRenderStates3D states{ BlendState::Default2D, RasterizerState::SolidCullBack };
		for (const auto& signDraw : draws)
		{
			const Mesh* boardMesh = getSignBoardMesh(signDraw.type);
			if (!boardMesh) continue;

			const auto& vis = RoadSign::visualOf(signDraw.type);
			if (!vis.textureAssetName.isEmpty())
			{
				boardMesh->draw(signDraw.boardMat, TextureAsset(vis.textureAssetName));
				continue;
			}
			if (SignArtwork::dynamic(signDraw.type))
			{
				if (auto found=m_regulatorySignTexCache.find(SignArtwork::key(signDraw.type,signDraw.auxNumber));found!=m_regulatorySignTexCache.end()) { boardMesh->draw(signDraw.boardMat,found->second); }
			}
			if (signDraw.type == RoadSignType::NationalRoute)
			{
				if (auto it = m_routeSignTexCache.find(signDraw.auxNumber); it != m_routeSignTexCache.end())
					boardMesh->draw(signDraw.boardMat, it->second);
			}
		}
	}
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
					if (edge->useElevation)
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

Array<std::pair<Vec3, Vec3>> RoadRenderer::buildNodeCapWireLines(
	const RoadNetwork& network, int nodeId, const World& world) const
{
	const auto layout = JunctionGeometry::build(network, nodeId, false);
	Array<std::pair<Vec3, Vec3>> lines;
	for (const auto& corner : layout.corners)
	{
		for (size_t index = 1; index < corner.sections.size(); ++index)
		{
			Vec3 a = corner.sections[index-1].position;
			Vec3 b = corner.sections[index].position;
			if (!layout.elevated)
			{
				a.y = world.sampleHeight(static_cast<float>(a.x), static_cast<float>(a.z)) + kRoadSurfaceLift;
				b.y = world.sampleHeight(static_cast<float>(b.x), static_cast<float>(b.z)) + kRoadSurfaceLift;
			}
			a.y += 0.015;
			b.y += 0.015;
			lines << std::pair<Vec3, Vec3>{ a, b };
		}
	}
	return lines;
}

// ---------------------------------------------------------------------------
// 信号メッシュキャッシュ
// ---------------------------------------------------------------------------

const Mesh* RoadRenderer::getSignalMesh(const String& defId, const String& meshName)
{
	auto& cache = m_signalMeshCache[defId];
	if (auto it = cache.meshes.find(meshName); it != cache.meshes.end())
		return &it->second;

	const SignalModel* model = m_signalRegistry.getModel(defId);
	if (!model) return nullptr;

	auto mit = model->meshes.find(meshName);
	if (mit == model->meshes.end()) return nullptr;

	const auto& partModelData = mit->second;
	if (partModelData.isEmpty()) return nullptr;

	cache.meshes[meshName] = Mesh{ MeshData{ partModelData.vertices, partModelData.indices } };
	return &cache.meshes[meshName];
}

// ---------------------------------------------------------------------------
// 信号機描画
// ---------------------------------------------------------------------------

// =============================================================================
// 信号描画ヘルパー
// =============================================================================

HashTable<int, RoadRenderer::EdgeSignalSummary>
RoadRenderer::buildEdgeSignalSummaries(const RoadNode& node,
                                        const SimGraph& simGraph,
                                        const TrafficLight* tl)
{
	HashTable<int, EdgeSignalSummary> summaries;
	for (const auto& conn : node.laneConnections)
	{
		const TurnType turn = TrafficCommon::classifyTurn(simGraph, conn);
		if (turn == TurnType::UTurn) continue;  // 描画上は無視

		const bool connGreen = tl ? tl->isGreen(conn.id) : true;
		EdgeSignalSummary& sum = summaries[conn.fromEdgeId];
		switch (turn)
		{
		case TurnType::Straight: sum.hasStraight = true; if (connGreen) sum.straightGreen = true; break;
		case TurnType::Left:     sum.hasLeft     = true; if (connGreen) sum.leftGreen     = true; break;
		case TurnType::Right:    sum.hasRight    = true; if (connGreen) sum.rightGreen    = true; break;
		default: break;
		}
	}
	return summaries;
}

// =============================================================================
// 信号機描画
// =============================================================================

void RoadRenderer::ensureSignalAttachGeomCache(const RoadNode& node, const RoadNetwork& network,
                                               const World& world, bool elevated,
                                               Array<SignalAttachGeomCache>& cacheArr) const
{
	// サイズが一致していればキャッシュ済み。道路変更時は呼び出し側で erase される
	if (cacheArr.size() == node.attachments.size()) return;

	cacheArr.assign(node.attachments.size(), SignalAttachGeomCache{});

	for (size_t ai = 0; ai < node.attachments.size(); ++ai)
	{
		const auto& att = node.attachments[ai];
		if (att.control != TrafficControl::Signal) continue;

		const RoadEdge* edge = network.getEdge(att.edgeId);
		if (!edge) continue;
		const auto bez = network.getBezier(att.edgeId);
		if (!bez) continue;

		const bool  isNodeA   = (edge->nodeA == node.id);
		const float cutoff    = isNodeA ? edge->cutoffA : edge->cutoffB;
		const float cutoffArc = isNodeA ? cutoff : (bez->totalLength - cutoff);
		const Vec3  cutPos    = bez->positionAt(cutoffArc);
		const Vec3  tan       = bez->tangentAt(cutoffArc);
		const Vec3  faceDir   = isNodeA ? tan : Vec3{ -tan.x, -tan.y, -tan.z };
		const float yaw       = static_cast<float>(Math::Atan2(faceDir.x, faceDir.z));
		const Vec3  rightVec  = tangentToRight(tan);

		// 進入方向から見た右肩側に信号を立てる。Roadbed パーツの端 = 路面端。
		// ノード端（A/B）でのオフセットを使用する
		const bool entryOnRight = !isNodeA;
		float roadEdgeOffset = 0.0f;
		bool  foundRoadbed   = false;
		for (const auto& part : edge->parts)
		{
			if (part.type != RoadPartType::Roadbed) continue;
			const float oL = isNodeA ? part.offsetA_L : part.offsetB_L;
			const float oR = isNodeA ? part.offsetA_R : part.offsetB_R;
			const float edgePos = entryOnRight ? oR : oL;
			if (!foundRoadbed)
			{
				roadEdgeOffset = edgePos;
				foundRoadbed   = true;
			}
			else
			{
				roadEdgeOffset = entryOnRight ? Max(roadEdgeOffset, edgePos) : Min(roadEdgeOffset, edgePos);
			}
		}
		if (!foundRoadbed)
		{
			roadEdgeOffset = entryOnRight ? edge->totalWidth() * 0.5f : -edge->totalWidth() * 0.5f;
		}

		const double signalY = elevated
			? cutPos.y + kRoadSurfaceLift
			: static_cast<double>(world.sampleHeight(
				static_cast<float>(cutPos.x), static_cast<float>(cutPos.z))) + kRoadSurfaceLift;

		const Vec3 signalPos{ cutPos.x - rightVec.x * roadEdgeOffset, signalY, cutPos.z - rightVec.z * roadEdgeOffset };
		cacheArr[ai].baseMat = Mat4x4::RotateY(yaw)
			* Mat4x4::Translate(Float3{
				static_cast<float>(signalPos.x),
				static_cast<float>(signalPos.y),
				static_cast<float>(signalPos.z) });
		cacheArr[ai].valid = true;
	}
}

void RoadRenderer::drawSignals(const RoadNetwork& network, const SimGraph& simGraph,
                               const World& world,
                               const HashTable<int, TrafficLight>& trafficLights,
                               GameTime gameNow, Vec3 cameraPos)
{
	constexpr double kSignalDrawMaxDistSq = 500.0 * 500.0;

	// 交差点ごとの進入方向サマリーと取付行列を再利用しながら、信号灯器をまとめて描画する。
	for (const auto& node : network.nodes())
	{
		if (node.id < 0 || !node.signalPlacement)
		{
			continue;
		}

		const auto& sigPlacement = *node.signalPlacement;
		const SignalDef* def = m_signalRegistry.getDef(sigPlacement.signalDefId);
		const SignalModel* model = def ? m_signalRegistry.getModel(sigPlacement.signalDefId) : nullptr;
		if (!def || !model || !model->texture)
		{
			continue;
		}

		// 距離カリング
		const double dx = node.position.x - cameraPos.x;
		const double dz = node.position.z - cameraPos.z;
		if (dx * dx + dz * dz > kSignalDrawMaxDistSq)
		{
			continue;
		}

		const bool elevated = network.isNodeElevated(node.id);

		// この交差点の LaneConnection を進入エッジ別 × 旋回別に集計する。
		// attachments ループの中で繰り返し計算しないようここで一度だけ作る。
		const auto tlIt = trafficLights.find(node.id);
		const TrafficLight* tl = (tlIt != trafficLights.end()) ? &tlIt->second : nullptr;

		// フェーズが変化したときのみ再構築（毎フレームのアロケーションを回避）
		const int phaseIdx = tl ? tl->currentPhaseIndex() : -1;
		auto& sCache = m_signalSummaryCache[node.id];
		if (sCache.lastPhaseIdx != phaseIdx)
		{
			sCache.summaries    = buildEdgeSignalSummaries(node, simGraph, tl);
			sCache.lastPhaseIdx = phaseIdx;
		}
		const auto& edgeSummaries = sCache.summaries;

		// アタッチメントの変換行列キャッシュを構築（未構築の場合のみ）
		auto& geomCacheArr = m_signalAttachGeomCache[node.id];
		ensureSignalAttachGeomCache(node, network, world, elevated, geomCacheArr);

		for (size_t ai = 0; ai < node.attachments.size(); ++ai)
		{
			const auto& att = node.attachments[ai];
			if (att.control != TrafficControl::Signal) continue;
			if (!geomCacheArr[ai].valid) continue;

			const Mat4x4& baseMat = geomCacheArr[ai].baseMat;

			// 筐体メッシュ描画
			if (const Mesh* bodyMesh = getSignalMesh(sigPlacement.signalDefId, def->bodyMeshName))
			{
				PhongMaterial bodyMat;
				bodyMat.ambientColor = ColorF{ 0.5 };
				bodyMat.diffuseColor = ColorF{ 1.0 };
				bodyMat.hasDiffuseTexture = true;
				bodyMesh->draw(baseMat, *model->texture, bodyMat);
			}

			// 進入エッジ単位の集計サマリーを引き当てる（loop 外で計算済み）
			const auto sumIt = edgeSummaries.find(att.edgeId);
			const EdgeSignalSummary sum = (sumIt != edgeSummaries.end())
				? sumIt->second
				: EdgeSignalSummary{};

			// メインランプ: 直進 LaneConnection が青なら緑。直進が無ければ全方向の論理和
			const bool isGreen = sum.hasStraight
				? sum.straightGreen
				: (sum.leftGreen || sum.rightGreen);

			bool isYellow = false;
			if (tl)
			{
				const float elapsed = tl->phaseElapsed(gameNow);
				const float duration = tl->currentPhaseDuration();
				if (isGreen && duration > 0.0f && (duration - elapsed) < kYellowDuration)
				{
					isYellow = true;
				}
			}

			// メインランプ描画
			for (size_t li = 0; li < def->lamps.size(); ++li)
			{
				const auto& lampDef = def->lamps[li];
				const Mesh* lampMesh = getSignalMesh(sigPlacement.signalDefId, lampDef.meshName);
				if (!lampMesh)
				{
					continue;
				}

				// ランプの状態を決定
				String stateId = U"off";
				if (lampDef.stateIds.contains(U"green") && isGreen && !isYellow)
				{
					stateId = U"green";
				}
				else if (lampDef.stateIds.contains(U"yellow") && isYellow)
				{
					stateId = U"yellow";
				}
				else if (lampDef.stateIds.contains(U"red") && !isGreen && !isYellow)
				{
					stateId = U"red";
				}

				auto stIt = def->states.find(stateId);
				if (stIt == def->states.end())
				{
					stIt = def->states.find(U"off");
				}
				if (stIt == def->states.end())
				{
					continue;
				}

				const auto& state = stIt->second;
				const TextureRegion texRegion = (*model->texture)(
					static_cast<int>(state.uvRect.x),
					static_cast<int>(state.uvRect.y),
					static_cast<int>(state.uvRect.z),
					static_cast<int>(state.uvRect.w));

				lampMesh->draw(baseMat, texRegion);
			}

			// sub_lamp（矢印信号）描画 — フェーズから自動導出
			// 進入エッジに属する LaneConnection を旋回別に分類し、
			// メインランプが赤のときに該当方向が青の場合のみ矢印を点灯する。
			// 配置列: arrow_left=0, arrow_straight=1, arrow_right=2
			if (def->subLamp)
			{
				const auto& subLampDef = *def->subLamp;
				const Mesh* subLampMesh = getSignalMesh(sigPlacement.signalDefId, subLampDef.meshName);
				const Mesh* subBodyMesh = getSignalMesh(sigPlacement.signalDefId, subLampDef.bodyMeshName);

				// メインランプが青/黄のときは矢印は点灯しない（重複を避ける）
				const bool mainLit = (isGreen || isYellow);

				// 各方向の点灯判定: 該当 LaneConnection が青 かつ メインが赤
				struct ArrowSlot { int col; String stateId; bool lit; };
				Array<ArrowSlot> slots;
				if (sum.hasLeft)
					slots << ArrowSlot{ 0, U"arrow_left",     sum.leftGreen     && !mainLit };
				if (sum.hasStraight)
					slots << ArrowSlot{ 1, U"arrow_straight", sum.straightGreen && !mainLit };
				if (sum.hasRight)
					slots << ArrowSlot{ 2, U"arrow_right",    sum.rightGreen    && !mainLit };

				for (const auto& s : slots)
				{
					const int row = s.col / subLampDef.cols;
					const int c   = s.col % subLampDef.cols;
					const Float3 offset{
						subLampDef.colStride.x * c + subLampDef.rowStride.x * row,
						subLampDef.colStride.y * c + subLampDef.rowStride.y * row,
						subLampDef.colStride.z * c + subLampDef.rowStride.z * row
					};

					const Mat4x4 subLampMat = Mat4x4::Translate(offset) * baseMat;

					if (subBodyMesh)
					{
						PhongMaterial subBodyMat;
						subBodyMat.ambientColor = ColorF{ 0.5 };
						subBodyMat.diffuseColor = ColorF{ 1.0 };
						subBodyMat.hasDiffuseTexture = true;
						subBodyMesh->draw(subLampMat, *model->texture, subBodyMat);
					}

					if (subLampMesh)
					{
						const String renderStateId = s.lit ? s.stateId : U"off";
						auto renderStIt = def->states.find(renderStateId);
						if (renderStIt == def->states.end())
						{
							continue;
						}
						const auto& renderState = renderStIt->second;

						const TextureRegion texRegion = (*model->texture)(
							static_cast<int>(renderState.uvRect.x),
							static_cast<int>(renderState.uvRect.y),
							static_cast<int>(renderState.uvRect.z),
							static_cast<int>(renderState.uvRect.w));

						subLampMesh->draw(subLampMat, texRegion);
					}
				}
			}
		}
	}

}

// =============================================================================
// 国道路線標識（3D）
// =============================================================================

namespace
{
	/// @brief 国道標識を生成・描画する対象となる route かどうか
	bool isDrawableNationalRoute(const RoadRoute& route)
	{
		return route.id >= 0
			&& route.kind == RoadRouteKind::NationalRoute
			&& route.number > 0;
	}
}

void RoadRenderer::prepareRouteSignTextures(const RoadNetwork& network)
{
	// A fixed small shared palette avoids traversing every road each frame.
	for (const auto type : {RoadSignType::SpeedLimit,RoadSignType::OneWay,RoadSignType::CurveWarning})
	{
		const int start=type==RoadSignType::SpeedLimit ? 20 : 0,end=type==RoadSignType::SpeedLimit ? 100 : 2,step=type==RoadSignType::SpeedLimit ? 10 : 1;
		for (int value=start;value<=end;value+=step)
		{
			const int key=SignArtwork::key(type,value);
			if (m_regulatorySignTexCache.contains(key)) { continue; }
			RenderTexture texture{256,256,ColorF{0,0},TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::No,HasMipMap::Yes};
			{
				const ScopedRenderTarget2D target{texture};
				const ScopedRenderStates2D blend{BlendState::Opaque};
				SignArtwork::draw(type,value,FontAsset(Asset::Arial24)); Graphics2D::Flush();
			}
			texture.generateMips(); m_regulatorySignTexCache.emplace(key,std::move(texture));
		}
	}
	const Texture& baseTex = TextureAsset(Asset::NationalRoadSign);
	if (not baseTex) return;

	const Font& fontNum = FontAsset(Asset::Arial24);
	constexpr int kTexSize = 256;

	for (const auto& route : network.routes())
	{
		if (!isDrawableNationalRoute(route)) continue;
		if (m_routeSignTexCache.contains(route.number)) continue;

		// 路線番号ごとに 1 回だけ看板テクスチャを合成し、以後は共有キャッシュから使い回す。
		// 透明背景に看板 + 号数を合成（透過部分は alpha=0）
		// 3D Mesh::draw が mipmap サンプルするため HasMipMap::Yes を指定し、生成する。
		RenderTexture rt{ kTexSize, kTexSize, ColorF{ 0.0, 0.0 },
		                  TextureFormat::R8G8B8A8_Unorm_SRGB,
		                  HasDepth::No, HasMipMap::Yes };
		{
			const ScopedRenderTarget2D target2d{ rt };
			// ベース看板はアルファをそのまま書き写したい → Opaque で上書き
			{
				const ScopedRenderStates2D blend{ BlendState::Opaque };
				baseTex.resized(kTexSize).draw(0, 0);
				Graphics2D::Flush();
			}
			// 号数は透明背景でも見える Default2D で重ねる
			{
				const ScopedRenderStates2D blend{ BlendState::Default2D };
				const double numSize = kTexSize * 0.361;
				fontNum(route.number).drawAt(
					numSize,
					Vec2{ kTexSize * 0.5, kTexSize * 0.5 - kTexSize * 0.02 },
					Palette::White);
				Graphics2D::Flush();
			}
		}
		rt.generateMips();
		m_routeSignTexCache[route.number] = std::move(rt);
	}
}

void RoadRenderer::drawRouteSigns(const RoadNetwork& network, const World& world, [[maybe_unused]] Vec3 cameraPos)
{
	for (const auto& route : network.routes())
	{
		if (!isDrawableNationalRoute(route)) continue;

		if (!m_routeSignCache.contains(route.id))
			m_routeSignCache[route.id] = buildRouteSignDraws(route, network, world);

		drawSigns(m_routeSignCache[route.id]);
	}
}

// =============================================================================
// 案内標識（GuideSign）
// =============================================================================

namespace
{
	/// @brief GuideSignPlacement の内容からテクスチャキャッシュキーを計算
	uint64 guideSignTexKey(const GuideSignPlacement& g)
	{
		// FNV-1a 64bit
		uint64 h = 14695981039346656037ull;
		auto mix = [&](uint64 v) {
			h ^= v;
			h *= 1099511628211ull;
		};
		auto mixStr = [&](const String& s) {
			for (char32_t c : s) mix(static_cast<uint64>(c));
			mix(0xff);  // 区切り
		};
		auto mixFloat = [&](float f) {
			mix(static_cast<uint64>(static_cast<int64>(f * 1000.0f + (f >= 0 ? 0.5f : -0.5f))));
		};
		mix(static_cast<uint64>(g.kind));
		mixFloat(g.widthOverride);
		mixFloat(g.heightOverride);
		mixFloat(static_cast<float>(g.bgColor.r));
		mixFloat(static_cast<float>(g.bgColor.g));
		mixFloat(static_cast<float>(g.bgColor.b));
		mixFloat(static_cast<float>(g.bgColor.a));
		mix(g.showReading ? 1 : 0);
		mix(static_cast<uint64>(g.elements.size()));
		for (const auto& el : g.elements)
		{
			mix(static_cast<uint64>(el.kind));
			mixFloat(el.posX);
			mixFloat(el.posY);
			mixFloat(el.scale);
			mixFloat(el.arrowAngle);
			mixStr(el.text);
			mixStr(el.reading);
		}
		return h;
	}

}

namespace
{
	/// @brief 1基分の案内標識テクスチャを合成する
	/// @details SRGB format でサンプリング時に線形空間へ自動変換させる（ルート看板と色合いを合わせる）
	///   HasMipMap なし（Test 検証: HasMipMap::Yes だと描画内容がキャプチャ先に反映されない）
	RenderTexture buildGuideSignTexture(const GuideSignPlacement& g, const Font& fontJa, const Font& fontNum)
	{
		const auto   bs      = GuideSign::computeBoardSizeFor(g);
		const Size   texSize = GuideSign::guideSignTexSize(bs.width, bs.height);
		const ColorF bg      = GuideSign::resolveBgColor(g.bgColor);

		// SRGB フォーマットへのクリア色は linear 値として解釈されて sRGB 符号化される。
		// bgColor (21/255, 87/255, 161/255) を linear とみなして encode した結果が物理バイトに
		// 入るため、3D でサンプリングしたとき国道アイコン PNG（Mipped / Unorm 読み込み）と
		// 同じ明るめの青 ≒ (94,193,230) で表示され両者の色味が一致する。
		RenderTexture rt{ static_cast<uint32>(texSize.x), static_cast<uint32>(texSize.y), bg,
		                  TextureFormat::R8G8B8A8_Unorm_SRGB };
		{
			const ScopedRenderTarget2D target{ rt };
			const ScopedRenderStates2D blend{ BlendState::Default2D };
			GuideSign::renderContents(g, texSize, fontJa, fontNum);
			Graphics2D::Flush();
		}
		rt.generateMips();
		return rt;
	}
}

void RoadRenderer::prepareGuideSignTextures(const RoadNetwork& network)
{
	// 全テクスチャ準備済みなら即リターン（毎フレーム 5000 件を走査するコストを回避）
	if (m_guideSignTexAllReady) return;

	// 地名は太字で表示（現物に合わせる）
	const Font& fontJa  = FontAsset(Asset::CJK32Bold);
	const Font& fontNum = FontAsset(Asset::Arial24);
	if (!fontJa || !fontNum) return;

	int missCount = 0;

	for (const auto& g : network.guideSigns())
	{
		if (g.id < 0) continue;
		if (g.elements.isEmpty()) continue;

		// 板面内容のハッシュ単位でテクスチャ化し、同一内容の標識はエッジをまたいで共有する。
		const uint64 key = guideSignTexKey(g);
		if (m_guideSignTexCache.contains(key)) continue;
		++missCount;

		m_guideSignTexCache[key] = buildGuideSignTexture(g, fontJa, fontNum);
	}

	// キャッシュミスがなくなったら準備完了フラグをセット
	if (missCount == 0)
		m_guideSignTexAllReady = true;
}

Array<RoadRenderer::GuideSignDraw> RoadRenderer::buildEdgeGuideSignDraws(
	const RoadNetwork& network, int edgeId, const World& world) const
{
	const RoadEdge* edge = network.getEdge(edgeId);
	if (!edge || !edge->isRoadbedBuilt()) return {};
	const auto bez = network.getBezier(edgeId);
	if (!bez) return {};

	Array<GuideSignDraw> out;

	for (const auto& g : network.guideSigns())
	{
		if (g.id < 0 || g.parentEdgeId != edgeId) continue;
		if (g.elements.isEmpty()) continue;

		const bool atA = (g.nodeEndId == edge->nodeA);
		const bool atB = (g.nodeEndId == edge->nodeB);
		if (!atA && !atB) continue;

		// 弧長位置: cutoff + arcOffset を内側方向に
		const float cutoff = atA ? edge->cutoffA : edge->cutoffB;
		const float arc = atA
			? (cutoff + g.arcOffset)
			: (bez->totalLength - cutoff - g.arcOffset);

		// 看板は driver に向ける: nodeA 側 → driver は B→A → 看板正面は +tan
		//                          nodeB 側 → driver は A→B → 看板正面は -tan
		const bool boardFacesTan = atA;

		const auto bs = GuideSign::computeBoardSizeFor(g);
		const auto& meta = GuideSign::poleMetadata();

		Mat4x4 poleMat, boardMat;
		Vec3   poleTop;
		if (!computeSignTransforms(*bez, world, arc, g.lateralOffset, boardFacesTan,
		                           meta.offsetX, meta.offsetY, meta.offsetZ,
		                           edge->useElevation, poleMat, boardMat, poleTop))
			continue;

		MeshData md = GuideSign::CreateBoardMesh(bs.width, bs.height);
		if (md.vertices.isEmpty()) continue;

		GuideSignDraw d;
		d.poleMat   = poleMat;
		d.boardMat  = boardMat;
		d.poleTop   = poleTop;
		d.boardMesh = Mesh{ md };
		d.texKey    = guideSignTexKey(g);
		out << d;
	}
	return out;
}

const Texture* RoadRenderer::getGuideSignCachedTexture(const GuideSignPlacement& g) const
{
	const uint64 key = guideSignTexKey(g);
	if (auto it = m_guideSignTexCache.find(key); it != m_guideSignTexCache.end())
		return &it->second;
	return nullptr;
}

// =============================================================================
// 選択アウトライン用シルエット描画（書き込み先を差し替え、通常パスと同じメッシュ・変換で描画）
// =============================================================================

void RoadRenderer::drawEdgeSilhouette(int edgeId, const RoadNetwork& network, const World& world,
                                       const ColorF& color)
{
	const RoadEdge* edge = network.getEdge(edgeId);
	if (!edge) return;

	// 必要ならキャッシュ構築
	auto meshIt = m_partMeshCache.find(edgeId);
	if (meshIt == m_partMeshCache.end())
	{
		const auto bez = network.getBezier(edgeId);
		if (!bez) return;
		const float mA = edgeMargin(*edge, edge->nodeA);
		const float mB = edgeMargin(*edge, edge->nodeB);
		auto entries = buildPartMeshes(network, *edge, *bez, world, mA, mB);
		if (entries.isEmpty()) return;
		meshIt = m_partMeshCache.emplace(edgeId, std::move(entries)).first;
		m_marginCache[edgeId] = { mA, mB };
	}

	for (const auto& entry : meshIt->second)
		entry.meshPair.detail.draw(color);

	// 橋脚
	if (edge->useElevation)
	{
		if (const auto it = m_pierMeshCache.find(edgeId); it != m_pierMeshCache.end())
			for (const auto& m : it->second) m.draw(color);
	}
}

void RoadRenderer::drawNodeSilhouette(int nodeId, const RoadNetwork& network, const World& world,
                                       const ColorF& color)
{
	const RoadNode* node = network.getNode(nodeId);
	if (!node || node->attachments.size() < 2) return;

	if (!m_nodeCapCache.contains(nodeId))
	{
		auto entries = buildNodeCapParts(network, nodeId, world, 16);
		if (entries.isEmpty()) return;
		m_nodeCapCache.emplace(nodeId, std::move(entries));
	}
	const auto it = m_nodeCapCache.find(nodeId);
	if (it == m_nodeCapCache.end()) return;
	for (const auto& entry : it->second)
		entry.meshPair.detail.draw(color);
}

void RoadRenderer::drawSignalSilhouette(int nodeId, const RoadNetwork& network, const World& world,
                                         const ColorF& color)
{
	const RoadNode* node = network.getNode(nodeId);
	if (!node || !node->signalPlacement) return;

	const auto& sigPlacement = *node->signalPlacement;
	const SignalDef* def = m_signalRegistry.getDef(sigPlacement.signalDefId);
	if (!def) return;

	const bool elevated = network.isNodeElevated(nodeId);
	auto& geomCacheArr = m_signalAttachGeomCache[nodeId];
	ensureSignalAttachGeomCache(*node, network, world, elevated, geomCacheArr);

	for (size_t ai = 0; ai < node->attachments.size(); ++ai)
	{
		const auto& att = node->attachments[ai];
		if (att.control != TrafficControl::Signal) continue;
		if (ai >= geomCacheArr.size() || !geomCacheArr[ai].valid) continue;

		const Mat4x4& baseMat = geomCacheArr[ai].baseMat;
		if (const Mesh* bodyMesh = getSignalMesh(sigPlacement.signalDefId, def->bodyMeshName))
			bodyMesh->draw(baseMat, color);
		for (const auto& lampDef : def->lamps)
		{
			if (const Mesh* lampMesh = getSignalMesh(sigPlacement.signalDefId, lampDef.meshName))
				lampMesh->draw(baseMat, color);
		}
		if (def->subLamp)
		{
			if (const Mesh* subMesh = getSignalMesh(sigPlacement.signalDefId, def->subLamp->meshName))
				subMesh->draw(baseMat, color);
		}
	}
}

void RoadRenderer::drawGuideSignSilhouette(int signId, const RoadNetwork& network, const World& world,
                                            const ColorF& color)
{
	const GuideSignPlacement* target = nullptr;
	for (const auto& g : network.guideSigns())
	{
		if (g.id == signId) { target = &g; break; }
	}
	if (!target) return;

	const RoadEdge* edge = network.getEdge(target->parentEdgeId);
	if (!edge || !edge->isRoadbedBuilt()) return;
	const auto bez = network.getBezier(target->parentEdgeId);
	if (!bez) return;

	const bool atA = (target->nodeEndId == edge->nodeA);
	const bool atB = (target->nodeEndId == edge->nodeB);
	if (!atA && !atB) return;

	const float cutoff = atA ? edge->cutoffA : edge->cutoffB;
	const float arc = atA ? (cutoff + target->arcOffset)
	                      : (bez->totalLength - cutoff - target->arcOffset);
	const bool boardFacesTan = atA;

	const auto bs = GuideSign::computeBoardSizeFor(*target);
	const auto& meta = GuideSign::poleMetadata();

	Mat4x4 poleMat, boardMat;
	Vec3 poleTop;
	if (!computeSignTransforms(*bez, world, arc, target->lateralOffset, boardFacesTan,
	                           meta.offsetX, meta.offsetY, meta.offsetZ,
	                           edge->useElevation, poleMat, boardMat, poleTop))
		return;

	if (not m_guidePoleMesh)
	{
		const MeshData md = GuideSign::CreatePoleMesh();
		if (not md.vertices.isEmpty()) m_guidePoleMesh = Mesh{ md };
	}
	if (m_guidePoleMesh)
	{
		const ScopedRenderStates3D states{ RasterizerState::SolidCullNone };
		m_guidePoleMesh->draw(poleMat, color);
	}

	MeshData md = GuideSign::CreateBoardMesh(bs.width, bs.height);
	if (!md.vertices.isEmpty())
	{
		const Mesh boardMesh{ md };
		const ScopedRenderStates3D states{ RasterizerState::SolidCullNone };
		boardMesh.draw(boardMat, color);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// ワイヤーフレーム描画 (Planned / UnderConstruction)
// ─────────────────────────────────────────────────────────────────────────────

void RoadRenderer::drawEdgeWireframe(const RoadEdge& edge, const RoadNetwork& network,
                                      const World& world, ColorF color)
{
	const auto bezOpt = network.getBezier(edge.id);
	if (!bezOpt) return;
	const CubicBezier& bez = *bezOpt;

	// 全エッジ幅を包む左端・右端オフセットを計算
	float offL = 0.0f, offR = 0.0f;
	bool hasAny = false;
	for (const auto& part : edge.parts)
	{
		const float pL = Min(part.offsetA_L, part.offsetB_L);
		const float pR = Max(part.offsetA_R, part.offsetB_R);
		if (!hasAny)
		{
			offL = pL; offR = pR;
			hasAny = true;
		}
		else
		{
			offL = Min(offL, pL);
			offR = Max(offR, pR);
		}
	}
	if (!hasAny) { offL = -3.0f; offR = 3.0f; }

	constexpr int kSegs = 20;
	const float mA = edgeMargin(edge, edge.nodeA);
	const float mB = edgeMargin(edge, edge.nodeB);
	const float sStart = mA;
	const float sEnd   = bez.totalLength - mB;
	if (sEnd <= sStart) return;

	// 長手方向の線（左端・右端それぞれ）
	for (int side = 0; side < 2; ++side)
	{
		const float off = (side == 0) ? offL : offR;
		Vec3 prev{};
		for (int i = 0; i <= kSegs; ++i)
		{
			const float s = sStart + (sEnd - sStart) * (i / static_cast<float>(kSegs));
			const Vec3 p = bez.positionAt(s);
			const Vec3 r = tangentToRight(bez.tangentAt(s));
			const double y = edge.useElevation
				? p.y + kRoadSurfaceLift + 0.1
				: world.sampleHeight(static_cast<float>(p.x), static_cast<float>(p.z)) + kRoadSurfaceLift + 0.1;
			const Vec3 cur = Vec3{ p.x, y, p.z } + r * static_cast<double>(off);
			if (i > 0) Line3D{ prev, cur }.draw(color);
			prev = cur;
		}
	}

	// A 端・B 端の幅方向の線
	for (int endIdx = 0; endIdx < 2; ++endIdx)
	{
		const float s = (endIdx == 0) ? sStart : sEnd;
		const Vec3 p = bez.positionAt(s);
		const Vec3 r = tangentToRight(bez.tangentAt(s));
		const double y = edge.useElevation
			? p.y + kRoadSurfaceLift + 0.1
			: world.sampleHeight(static_cast<float>(p.x), static_cast<float>(p.z)) + kRoadSurfaceLift + 0.1;
		const Vec3 center{ p.x, y, p.z };
		Line3D{ center + r * static_cast<double>(offL), center + r * static_cast<double>(offR) }.draw(color);
	}
}

void RoadRenderer::drawNodeCapWireframe(const RoadNetwork& network, int nodeId, const World& world)
{
	const RoadNode* node = network.getNode(nodeId);
	if (!node || node->attachments.size() < 2) return;

	// Planned/UnderConstruction エッジが1つでもあるノードのみ描画
	bool hasWireEdge = false;
	for (const auto& att : node->attachments)
	{
		const RoadEdge* edge = network.getEdge(att.edgeId);
		if (!edge) { continue; }
		if (edge->edgeState == EdgeState::Planned)
		{
			hasWireEdge = true;
			break;
		}
	}
	if (!hasWireEdge) return;

	if (!m_nodeCapWireCache.contains(nodeId))
	{
		m_nodeCapWireCache[nodeId] = buildNodeCapWireLines(network, nodeId, world);
	}

	const ColorF white = ColorF{ 1.0 };
	for (const auto& [a, b] : m_nodeCapWireCache[nodeId])
	{
		Line3D{ a, b }.draw(white);
	}
}

void RoadRenderer::renderWireframes(const RoadNetwork& network, const World& world,
                                     const ViewFrustum& frustum, Vec3 cameraPos)
{
	const float camX = static_cast<float>(cameraPos.x);
	const float camZ = static_cast<float>(cameraPos.z);
	constexpr float kDrawMaxDistSqF = static_cast<float>(kDrawMaxDistSq);

	const ColorF wirePlanned        = ColorF{ 1.0, 1.0, 1.0, 1.0 };
	const ColorF wireUnderConstruct = ColorF{ 1.0, 0.8, 0.2, 1.0 };

	for (const RoadEdge& edge : network.edges())
	{
		if (edge.id < 0) continue;
		if (edge.edgeState != EdgeState::Planned) continue;

		// 距離チェック
		auto boundsIt = m_boundsCache.find(edge.id);
		if (boundsIt == m_boundsCache.end())
		{
			const RoadNode* nA = network.getNode(edge.nodeA);
			const RoadNode* nB = network.getNode(edge.nodeB);
			if (!nA || !nB) continue;
			EdgeBounds b;
			b.center = Float3{
				static_cast<float>((nA->position.x + nB->position.x) * 0.5),
				static_cast<float>((nA->position.y + nB->position.y) * 0.5),
				static_cast<float>((nA->position.z + nB->position.z) * 0.5)
			};
			const float chordSq = static_cast<float>((nA->position - nB->position).lengthSq());
			const float arcR    = edge.length * 0.6f;
			b.radiusSq = Max(arcR * arcR, chordSq * 0.36f);
			boundsIt = m_boundsCache.emplace(edge.id, b).first;
		}
		const auto& bounds = boundsIt->second;
		const float dx = bounds.center.x - camX;
		const float dz = bounds.center.z - camZ;
		if (dx * dx + dz * dz > kDrawMaxDistSqF) continue;

		const float radius = Math::Sqrt(bounds.radiusSq);
		if (!frustum.intersects(Sphere{ Vec3{ bounds.center }, static_cast<double>(radius) })) continue;

		const ColorF wireColor = (edge.edgeState == EdgeState::Planned)
			? wirePlanned : wireUnderConstruct;
		drawEdgeWireframe(edge, network, world, wireColor);
	}

	// ワイヤーノードキャップ（交差点）
	for (const RoadNode& node : network.nodes())
	{
		if (node.id < 0) continue;
		const float ndx = static_cast<float>(node.position.x) - camX;
		const float ndz = static_cast<float>(node.position.z) - camZ;
		if (ndx * ndx + ndz * ndz > kDrawMaxDistSqF) continue;
		if (!frustum.intersects(Sphere{ node.position, 30.0 })) continue;
		drawNodeCapWireframe(network, node.id, world);
	}
}

void RoadRenderer::drawGuideSigns(const Array<GuideSignDraw>& draws)
{
	if (draws.isEmpty()) return;

	// 2 本柱フレーム（OBJ ロード、遅延初期化）
	if (not m_guidePoleMesh)
	{
		const MeshData md = GuideSign::CreatePoleMesh();
		if (not md.vertices.isEmpty()) m_guidePoleMesh = Mesh{ md };
	}
	if (not m_guidePoleMesh) return;

	// ポール
	{
		const ScopedRenderStates3D states{ RasterizerState::SolidCullNone };
		for (const auto& d : draws)
			m_guidePoleMesh->draw(d.poleMat, RoadSign::kPoleColor.removeSRGBCurve());
	}

	// 看板裏面: 灰色（前面ポリゴンをカリングして裏面のみ表示）
	{
		const ScopedRenderStates3D states{ RasterizerState::SolidCullFront };
		for (const auto& d : draws)
			d.boardMesh.draw(d.boardMat, ColorF{ 0.55 }.removeSRGBCurve());
	}

	// 看板前面: テクスチャ（裏面ポリゴンをカリングして前面のみ表示）
	{
		const ScopedRenderStates3D states{ BlendState::Default2D, RasterizerState::SolidCullBack };
		for (const auto& d : draws)
		{
			if (auto itRt = m_guideSignTexCache.find(d.texKey); itRt != m_guideSignTexCache.end())
				d.boardMesh.draw(d.boardMat, itRt->second);
		}
	}
}
