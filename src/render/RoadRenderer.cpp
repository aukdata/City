#include "RoadRenderer.hpp"
#include "ModelLod.hpp"
#include "../road/RoadEnvironment.hpp"
#include "MountainRoadGeometry.hpp"
#include "BridgeStructure.hpp"
#include "../debug/DebugLog.hpp"
#include "../road/RoadArrow.hpp"
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
					pos.y = sl.center.y + src.pos.y + def.heightOffset;
				}
				dst.pos = Float3{ static_cast<float>(pos.x), static_cast<float>(pos.y), static_cast<float>(pos.z) };
				dst.normal = Float3{
					static_cast<float>(sl.right.x) * src.normal.x + src.normal.y * 0.0f + static_cast<float>(tangent.x) * src.normal.z,
					src.normal.y,
					static_cast<float>(sl.right.z) * src.normal.x + src.normal.y * 0.0f + static_cast<float>(tangent.z) * src.normal.z
				};
				dst.tex = def.type == RoadPartType::Sidewalk || def.type == RoadPartType::Curb
					? Float2{lateral, s} : Float2{src.tex.x + static_cast<float>(segment), src.tex.y};
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

// ─────────────────────────────────────────────────────────────────────────────
// RoadRenderer 公開メソッド
// ─────────────────────────────────────────────────────────────────────────────

bool RoadRenderer::loadAssets()
{
	m_asphaltPS = PixelShader::HLSL(U"shaders/hlsl/city_forward.hlsl", U"Asphalt_PS");
	m_pavementPS = PixelShader::HLSL(U"shaders/hlsl/city_forward.hlsl", U"Pavement_PS");
	m_asphaltNormal = Texture{U"assets/third_party/polyhaven/asphalt_floor/asphalt_floor_nor_gl_1k.jpg", TextureDesc::Mipped};
	DBG_LOG(U"[StreetMaterials] asphalt={} pavement={}"_fmt(static_cast<bool>(m_asphaltPS), static_cast<bool>(m_pavementPS)));
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
		m_constructionLodModels[i] = Model{modelLodPath(U"assets/construction/{}.obj"_fmt(models[i]), 2)};
		Model::RegisterDiffuseTextures(m_constructionLodModels[i],TextureDesc::MippedSRGB);
	}
	m_arrowMarkingRegistry.load(U"assets/road_markings");
	m_cableVS = VertexShader::HLSL(U"shaders/hlsl/city_cable.hlsl", U"Cable_VS");
	m_cablePS = PixelShader::HLSL(U"shaders/hlsl/city_cable.hlsl", U"Cable_PS");
	if (!m_cableVS || !m_cablePS) { return false; }
	m_signalRegistry.load(U"assets/signals");
	return m_partRegistry.load(U"assets/road_parts");
}

// ─────────────────────────────────────────────────────────────────────────────
// エッジ描画
// ─────────────────────────────────────────────────────────────────────────────

void RoadRenderer::drawEdge(const RoadEdge& edge, const RoadNetwork& network,
                             float marginA, float marginB, const World& world, bool isClose, bool prepareDetail)
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
		if (!canBuildCache()) { drawFallbackEdge(edge, network, world); return; }
		const auto bez = network.getBezier(edge.id);
		if (!bez) return;
		auto entries = buildPartMeshes(network, edge, *bez, world, marginA, marginB);
		++m_cacheBuildStats.edges;
		m_cacheBuildStats.partsMs += buildTimer.msF();
		m_fallbackEdges.erase(edge.id);
		m_drawnFallbackEdges.erase(edge.id);
		++m_geometryRevision;
		meshIt = m_partMeshCache.emplace(edge.id, std::move(entries)).first;
		m_marginCache[edge.id] = { marginA, marginB };
		// Combined LOD batches use the same strip data as the detailed geometry.
	}

	// ---- 部品ごとに描画 ----
	if (isClose)
	{
		for (const auto& entry : meshIt->second)
		{
			drawSurface(entry, entry.meshPair.detail);
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
	if (isClose || prepareDetail)
	{
		if (!m_markingCacheByEdge.contains(edge.id) && canBuildCache())
		{
			const auto bez = network.getBezier(edge.id);
			if (!bez) return;
			const Stopwatch cacheTimer{StartImmediately::Yes};
			m_markingCacheByEdge[edge.id] = buildEdgeRoadMarkingBatches(network, edge, *bez, world, marginA, marginB);
			m_cacheBuildStats.markingsMs += cacheTimer.msF();
			++m_cacheBuildStats.markings;
		}

		if (const auto markings = m_markingCacheByEdge.find(edge.id); isClose && markings != m_markingCacheByEdge.end())
		{
			for (const auto& batch : markings->second) { batch.mesh.draw(batch.color); }
		}
	}

	// ---- 道路沿い設備（近距離のみ） ----
	if (isClose || prepareDetail)
	{
		if (!m_streetFurnitureCache.contains(edge.id) && canBuildCache())
		{
			const auto bez = network.getBezier(edge.id);
			if (!bez) return;
			const Stopwatch cacheTimer{StartImmediately::Yes};
			m_streetFurnitureCache[edge.id] = buildStreetFurnitureBatches(edge, *bez, world, marginA, marginB);
			m_cacheBuildStats.furnitureMs += cacheTimer.msF();
			++m_cacheBuildStats.furniture;
			++m_geometryRevision;
		}
		if (const auto furniture = m_streetFurnitureCache.find(edge.id); isClose && furniture != m_streetFurnitureCache.end())
		{
			for (const auto& b : furniture->second)
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
	}
	// 規制標識・案内板も接近前に準備し、初回描画で予算外の作業を集中させない。
	if(isClose || prepareDetail)
	{
		if(!m_signCache.contains(edge.id) && canBuildCache())
		{
			const Stopwatch timer{StartImmediately::Yes};m_signCache[edge.id]=buildEdgeSignMeshes(network,edge.id,world);m_cacheBuildStats.furnitureMs+=timer.msF();
		}
		if(!m_guideSignCache.contains(edge.id) && canBuildCache())
		{
			const Stopwatch timer{StartImmediately::Yes};m_guideSignCache[edge.id]=buildEdgeGuideSignDraws(network,edge.id,world);m_cacheBuildStats.furnitureMs+=timer.msF();
		}
		if(const auto found=m_signCache.find(edge.id);isClose && found!=m_signCache.end()) { drawSigns(found->second); }
		if(const auto found=m_guideSignCache.find(edge.id);isClose && found!=m_guideSignCache.end()) { drawGuideSigns(found->second); }
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
		if (!canBuildCache()) { drawFallbackNode(nodeId, network, world); return; }
		const Stopwatch cacheTimer{StartImmediately::Yes};
		auto entries = buildNodeCapParts(network, nodeId, world, 16);
		++m_cacheBuildStats.nodes;
		m_cacheBuildStats.nodesMs += cacheTimer.msF();
		m_fallbackNodes.erase(nodeId);
		++m_geometryRevision;
		m_nodeCapCache[nodeId] = std::move(entries);
	}

	for (const auto& entry : m_nodeCapCache[nodeId])
	{
		const Mesh& mesh = isClose ? entry.meshPair.detail : entry.meshPair.lod;
		drawSurface(entry, mesh);
	}

	// 道路標示（停止線・横断歩道・矢印・ノード内区画線）
	const int64 nodeMarkingCacheKey = static_cast<int64>(nodeId) * 2 + (isClose ? 1 : 0);
	if (!m_markingCacheByNode.contains(nodeMarkingCacheKey) && canBuildCache())
	{
		const Stopwatch cacheTimer{StartImmediately::Yes};
		m_markingCacheByNode[nodeMarkingCacheKey] = buildNodeRoadMarkingBatches(network, nodeId, world, isClose);
		m_cacheBuildStats.markingsMs += cacheTimer.msF();
		++m_cacheBuildStats.markings;
	}
	if (const auto markings = m_markingCacheByNode.find(nodeMarkingCacheKey); markings != m_markingCacheByNode.end())
	{
		for (const auto& batch : markings->second) { batch.mesh.draw(batch.color); }
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// ヘルパー
// ─────────────────────────────────────────────────────────────────────────────

float RoadRenderer::edgeMargin(const RoadEdge& edge, int nodeId)
{
	return (nodeId == edge.nodeA) ? edge.cutoffA : edge.cutoffB;
}

void RoadRenderer::drawSurface(const PartMeshEntry& entry, const Mesh& mesh) const
{
	const auto draw = [&]()
	{
		if (entry.texture) { mesh.draw(*entry.texture, entry.color); }
		else { mesh.draw(entry.color); }
	};
	if (entry.materialType == RoadPartType::Roadbed && m_asphaltPS)
	{
		Graphics3D::SetPSTexture(4, m_asphaltNormal);
		const ScopedCustomShader3D shader{m_asphaltPS};
		draw();
	}
	else if ((entry.materialType == RoadPartType::Sidewalk || entry.materialType == RoadPartType::Curb) && m_pavementPS)
	{
		const ScopedCustomShader3D shader{m_pavementPS};
		draw();
	}
	else { draw(); }
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
			pL.y = sl.center.y;
			pR.y = sl.center.y;
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
				pL.y = sl.center.y;
				pR.y = sl.center.y;
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
	struct MaterialGroup
	{
		const Texture* tex;
		ColorF         color;
		MeshData       md;
	};
	Array<MaterialGroup> groups;


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
				range.sStart, range.sEnd, 1.0f, edge.usesDesignHeight());
			const PartModelData& lodModel = (!model.lods.isEmpty() && !model.lods[0].center.isEmpty()) ? model.lods[0].center : model.center;
			mdLod = buildRoadPartModelStrip(bezier, world, def, lodModel,
				part.offsetA_L, part.offsetA_R, part.offsetB_L, part.offsetB_R,
				range.sStart, range.sEnd, 0.25f, edge.usesDesignHeight());
		}
		else
		{
			mdDetail = buildStripMeshTapered(bezier, world,
				part.offsetA_L, part.offsetA_R,
				part.offsetB_L, part.offsetB_R,
				heightOff,
				range.sStart, range.sEnd, 1.0f, roadPartSideSkirtDrop(part.type, heightOff), edge.usesDesignHeight());
			mdLod = buildStripMeshTapered(bezier, world,
				part.offsetA_L, part.offsetA_R,
				part.offsetB_L, part.offsetB_R,
				heightOff,
				range.sStart, range.sEnd, 0.25f, roadPartSideSkirtDrop(part.type, heightOff), edge.usesDesignHeight());
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
		entry.materialType = part.type;
		entries << std::move(entry);
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
		group->md.vertices.append(mdLod.vertices);
		for (const auto& tri : mdLod.indices)
		{
			group->md.indices << TriangleIndex32{ tri.i0 + indexBase, tri.i1 + indexBase, tri.i2 + indexBase };
		}

	}

	auto& batches=m_partLodBatchCache[edge.id];
	batches.clear();
	batches.reserve(groups.size());
	for (auto& g : groups)
	{
		if (g.md.vertices.isEmpty()) continue;
		orientRoadFaces(g.md);
		batches << PartLodBatch{ Mesh{ g.md }, g.color, g.tex };
	}
	return entries;
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
	MeshData ironMd;
	const auto mountain=MountainRoadGeometry::build(edge,bezier,world,range.sStart,range.sEnd);

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
		float previousArc = sStart;
		Vec2 previousRight{ 0, 0 };
		const int sampleCount = Max(1, static_cast<int>((sEnd - sStart) / spacing));
		for (int i = 0; i <= sampleCount; ++i)
		{
			const float jitterOffset = (jitter > 0.0f)
				? (static_cast<float>(((edge.id * 73856093) ^ (static_cast<int>(partIndex) * 19349663) ^ (i * 83492791)) & 1023) / 1023.0f - 0.5f) * jitter
				: 0.0f;
			const float s = Clamp(sStart + static_cast<float>(i) * spacing + jitterOffset, range.sStart, range.sEnd);
			const Vec3 pos = bezier.positionAt(s);
			if (!RoadEnvironment::outdoorSpan(world,bezier,previousArc,s,edge.useElevation || edge.tunnel)) { previousPole.reset(); }
			previousArc=s;
			if (RoadEnvironment::coveredAt(world,pos,edge.useElevation || edge.tunnel)) { continue; }
			const Vec3 tan3 = bezier.tangentAt(s);
			Vec2 tangent{ static_cast<float>(tan3.x), static_cast<float>(tan3.z) };
			if (tangent.lengthSq() <= 1e-8f) continue;
			tangent.normalize();
			const Vec2 right{ tangent.y, -tangent.x };
			const float yaw = static_cast<float>(std::atan2(tangent.y, tangent.x));
			const double anchorX = pos.x + right.x * static_cast<double>(offset);
			const double anchorZ = pos.z + right.y * static_cast<double>(offset);
			const double terrainY = edge.usesDesignHeight()
				? pos.y
				: world.sampleHeight(static_cast<float>(pos.x), static_cast<float>(pos.z));
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

	// 排水桝と点検蓋は交差点の切り詰め範囲を避け、道路の地表面に沿わせる。
	if (!edge.useElevation)
	{
		constexpr float kDrainSpacing = 18.0f;
		for (const auto& part : edge.parts)
		{
			if (part.type != RoadPartType::Sidewalk || part.build != BuildState::Built) { continue; }
			const float inner = Abs(part.offsetL()) < Abs(part.offsetR()) ? part.offsetL() : part.offsetR();
			const float offset = inner + (inner < 0 ? -.22f : .22f);
			for (float arc = range.sStart + 8; arc < range.sEnd - 8; arc += kDrainSpacing)
			{
				const Vec3 tangent = bezier.tangentAt(arc);
				const Vec3 right = tangentToRight(tangent);
				Vec3 center = bezier.positionAt(arc) + right * offset;
				center.y = makeSlice(bezier,world,arc,kRoadSurfaceLift,edge.usesDesignHeight()).center.y + .155;
				const float yaw = static_cast<float>(Atan2(tangent.z, tangent.x));
				appendOrientedBox(roadsideMd, center, Float3{.70f,.018f,.35f}, yaw);
				for (int bar = -4; bar <= 4; ++bar)
				{
					appendOrientedBox(ironMd, center + tangent * (bar * .072) + Vec3{0,.012,0}, Float3{.022f,.016f,.32f}, yaw);
				}
			}
		}
		constexpr float kCoverSpacing = 47.0f;
		for (float arc = range.sStart + 15; arc < range.sEnd - 12; arc += kCoverSpacing)
		{
			const Vec3 tangent = bezier.tangentAt(arc);
			const Vec3 right = tangentToRight(tangent);
			Vec3 center = bezier.positionAt(arc) + right * .6;
			center.y = makeSlice(bezier,world,arc,kRoadSurfaceLift,edge.usesDesignHeight()).center.y + .012;
			appendCylinder(roadsideMd, center, .32, .012, 24);
			for (int rib = -2; rib <= 2; ++rib)
			{
				appendOrientedBox(ironMd, center + right * (rib * .09) + Vec3{0,.01,0}, Float3{.018f,.006f,.43f}, static_cast<float>(Atan2(right.z,right.x)));
			}
		}
	}
	Array<LaneLineBatch> batches;
	for (const auto& item : {std::pair<const MeshData*,ColorF>{&mountain.wall,ColorF{.43,.45,.40}},
		{&mountain.moss,ColorF{.24,.34,.16}},{&mountain.rail,ColorF{.48,.49,.43}},
		{&mountain.posts,ColorF{.82,.81,.69}},{&mountain.reflectors,ColorF{1,.36,.07}}})
	{
		if (!item.first->indices.isEmpty()) { batches<<LaneLineBatch{item.second.removeSRGBCurve(),Mesh{*item.first}}; }
	}
	if (!poleMd.vertices.isEmpty()) batches << LaneLineBatch{ ColorF{ 0.57, 0.57, 0.53 }.removeSRGBCurve(), Mesh{ poleMd } };
	if (!cableMd.vertices.isEmpty()) batches << LaneLineBatch{ ColorF{ 0.05, 0.05, 0.045 }.removeSRGBCurve(), Mesh{ cableMd }, true };
	if (!markerMd.vertices.isEmpty()) batches << LaneLineBatch{ ColorF{ 0.82, 0.78, 0.64 }.removeSRGBCurve(), Mesh{ markerMd } };
	if (!ironMd.vertices.isEmpty()) { batches << LaneLineBatch{ColorF{.43,.44,.43}.removeSRGBCurve(),Mesh{ironMd}}; }
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
	auto addEntry = [&](MeshData mesh, ColorF color, const Texture* texture, RoadPartType type = RoadPartType::Roadbed)
	{
		if (mesh.vertices.isEmpty() || mesh.indices.isEmpty()) { return; }
		orientRoadFaces(mesh);
		PartMeshEntry entry;
		entry.meshPair.detail = Mesh{ mesh };
		// Keep the same boundary at both distances so the terrain cut never opens a gap.
		entry.meshPair.lod = entry.meshPair.detail;
		entry.color = color;
		entry.texture = texture;
		entry.materialType = type;
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
			addEntry(std::move(mesh), color, texture, band.part.type);
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


// =============================================================================
// 国道路線標識（3D）
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
			const double y = edge.usesDesignHeight()
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
		const double y = edge.usesDesignHeight()
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
			boundsIt = m_boundsCache.emplace(edge.id, edgeBounds(network,edge.id)).first;
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

/// @brief A continuous, inexpensive surface remains visible while exact roadside parts are built.
void RoadRenderer::prepareFallbackEdge(const RoadEdge& edge, const RoadNetwork& network, const World& world)
{
	if (!m_fallbackEdges.contains(edge.id))
	{
		const Stopwatch timer{StartImmediately::Yes};
		Array<PartMeshEntry> entries;
		if (const auto bezier = network.getBezier(edge.id))
		{
			StripRange range;
			if (calcStripRange(bezier->totalLength, edge.cutoffA, edge.cutoffB, range))
			{
				for (const auto& part : edge.parts)
				{
					if (part.build != BuildState::Built || part.placement != RoadPartPlacement::Strip) { continue; }
					const auto [color, height, texture] = getPartVisual(part);
					auto data = buildStripMeshTapered(*bezier, world, part.offsetA_L, part.offsetA_R,
						part.offsetB_L, part.offsetB_R, height, range.sStart, range.sEnd, .25f, 0, edge.usesDesignHeight());
					if (data.indices.isEmpty()) { continue; }
					orientRoadFaces(data);
					const Mesh mesh{data};
					entries << PartMeshEntry{{mesh, mesh}, color, texture, part.type};
				}
			}
		}
		m_fallbackEdges.emplace(edge.id, std::move(entries));
		++m_geometryRevision;
		m_cacheBuildStats.fallbackMs += timer.msF();
	}
}

void RoadRenderer::drawFallbackEdge(const RoadEdge& edge,const RoadNetwork& network,const World& world)
{
	prepareFallbackEdge(edge,network,world);
	m_drawnFallbackEdges.emplace(edge.id);
	for (const auto& entry : m_fallbackEdges.at(edge.id)) { drawSurface(entry, entry.meshPair.detail); }
}

void RoadRenderer::prepareFallbackNode(int nodeId, const RoadNetwork& network, const World& world)
{
	if (!m_fallbackNodes.contains(nodeId))
	{
		const Stopwatch timer{StartImmediately::Yes};
		const auto layout = JunctionGeometry::build(network, nodeId, true, &world);
		auto data = JunctionGeometry::terrainFootprint(layout);
		for (auto& vertex : data.vertices)
		{
			if (!layout.elevated) { vertex.pos.y = static_cast<float>(world.sampleHeight(vertex.pos.x, vertex.pos.z) + kRoadSurfaceLift); }
			vertex.tex = Float2{vertex.pos.x * kWorldUV, vertex.pos.z * kWorldUV};
		}
		orientRoadFaces(data);
		const auto& definition = m_partRegistry.get(U"roadbed_asphalt");
		const Mesh mesh = data.indices.isEmpty() ? Mesh{} : Mesh{data};
		m_fallbackNodes.emplace(nodeId, PartMeshEntry{{mesh, mesh}, ColorF{.29, .30, .32},
			definition.texture ? &*definition.texture : nullptr, RoadPartType::Roadbed});
		m_cacheBuildStats.fallbackMs += timer.msF();
	}
}

void RoadRenderer::drawFallbackNode(int nodeId,const RoadNetwork& network,const World& world)
{
	prepareFallbackNode(nodeId,network,world);
	const auto& entry = m_fallbackNodes.at(nodeId);
	if (entry.meshPair.detail) { drawSurface(entry, entry.meshPair.detail); }
}
