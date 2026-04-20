#include "RoadRenderer.hpp"
#include "../road/RoadArrow.hpp"
#include "../road/RoadSign.hpp"
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
			const float gy = world.computeHeight(static_cast<float>(p.x), static_cast<float>(p.z));
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

	/// @brief 世界空間タイリング係数 [1/m]。道路・交差点の大きさに依らず一定スケールで繰り返す。
	constexpr float kWorldUV = 1.0f;

	/// @brief ワールド XZ 座標から世界空間 UV を生成
	Vertex3D makeVertWorldUV(const Vec3& pos)
	{
		return makeVert(pos,
			static_cast<float>(pos.x) * kWorldUV,
			static_cast<float>(pos.z) * kWorldUV);
	}

	/// @brief 四角形を頂点・インデックス配列に追記する（CW ワインディング）
	void appendQuad(Array<TriangleIndex32>& indices,
	                uint32 iL0, uint32 iR0, uint32 iL1, uint32 iR1)
	{
		indices << TriangleIndex32{ iL0, iL1, iR0 };
		indices << TriangleIndex32{ iR0, iL1, iR1 };
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
		float terrainLift = 2.05f,
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
		float terrainLift = 2.05f,
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
		case RoadPartType::Roadbed:   color = ColorF{ 0.36, 0.36, 0.36 }; break;
		case RoadPartType::Sidewalk:  color = ColorF{ 0.72, 0.70, 0.68 }; heightOff = 0.15f; break;
		case RoadPartType::Curb:      color = ColorF{ 0.78, 0.76, 0.72 }; heightOff = 0.15f; break;
		case RoadPartType::Median:    color = ColorF{ 0.75, 0.73, 0.70 }; break;
		case RoadPartType::Shoulder:  color = ColorF{ 0.40, 0.40, 0.40 }; break;
		case RoadPartType::Slope:     color = ColorF{ 0.45, 0.58, 0.35 }; break;
		case RoadPartType::Guardrail: color = ColorF{ 0.82, 0.82, 0.82 }; break;
		case RoadPartType::Wall:      color = ColorF{ 0.78, 0.78, 0.76 }; break;
		default: break;
		}
	}

	/// @brief LineType → 色・線幅
	struct LineStyle { ColorF color; float lineWidth; float dashLen; float gapLen; };
	LineStyle lineStyleFor(LineType lt)
	{
		switch (lt)
		{
		case LineType::SolidWhite:   return { ColorF{1,1,1}, 0.15f, 0, 0 };
		case LineType::DashedWhite:  return { ColorF{1,1,1}, 0.15f, 8, 12 };
		case LineType::SolidYellow:  return { ColorF{1,0.9,0}, 0.20f, 0, 0 };
		case LineType::DoubleYellow: return { ColorF{1,0.9,0}, 0.20f, 0, 0 };
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
		const Vec3 pos = bez->positionAt(s);
		const Vec3 rawTan = bez->tangentAt(s);
		const Vec3 right = tangentToRight(rawTan);
		const Vec3 tan = isNodeA ? -rawTan : rawTan;
		const double lineY = edge.useElevation
			? pos.y + kRoadLineLift
			: world.computeHeight(static_cast<float>(pos.x), static_cast<float>(pos.z)) + kRoadLineLift;
		return { Vec3{ pos.x, lineY, pos.z } + right * static_cast<double>(offset), tan };
	}

	/// @brief 2点間をベジェ曲線で結ぶ車線ライン
	/// @param tanFrom from 地点の進行方向（外向き）
	/// @param tanTo   to 地点の進行方向（外向き）
	void appendBezierLine(Array<RoadRenderer::LaneLineBatch>& out,
	                      const Vec3& from, const Vec3& to,
	                      const Vec3& tanFrom, const Vec3& tanTo,
	                      float lineWidth, const ColorF& color, [[maybe_unused]] const World& world)
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

		constexpr int kDiv = 8;
		const double hw = static_cast<double>(lineWidth * 0.5f);
		MeshData md;

		for (int k = 0; k <= kDiv; ++k)
		{
			const double t = k / static_cast<double>(kDiv);
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
	m_arrowMarkingRegistry.load(U"assets/road_markings");
	m_signalRegistry.load(U"assets/signals");
	return m_partRegistry.load(U"assets/road_parts");
}

void RoadRenderer::render(const RoadNetwork& network, const World& world,
                          const ViewFrustum& frustum, Vec3 cameraPos)
{
	m_visibleEdges.clear();
	Profiler::EnableAssetCreationWarning(false);

	// 地形変更時は該当チャンク内のエッジ/ノードのキャッシュのみクリアする
	for (const Chunk* chunk : world.getActiveChunks())
	{
		if (!chunk || !chunk->meshDirty) continue;
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

		drawEdge(edge, network, mA, mB, world, isClose);
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
		drawNodeCap(network, node.id, world, isClose);
	}
}

void RoadRenderer::eraseEdgeCaches(int edgeId)
{
	m_partMeshCache.erase(edgeId);
	m_partLodBatchCache.erase(edgeId);
	m_laneCache.erase(edgeId);
	m_marginCache.erase(edgeId);
	m_boundsCache.erase(edgeId);
	m_pierMeshCache.erase(edgeId);
	m_signCache.erase(edgeId);
	m_guideSignCache.erase(edgeId);
	m_guideSignTexAllReady = false;  // エッジ変更時は案内標識テクスチャを再チェック
}

void RoadRenderer::eraseNodeCaches(int nodeId)
{
	m_nodeCapCache.erase(nodeId);
	m_nodeCapLaneCache.erase(nodeId);
	m_stopLineCache.erase(nodeId);
	m_laneArrowCache.erase(nodeId);
	m_signalAttachGeomCache.erase(nodeId);
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
		m_nodeCapLaneCache.clear();
		m_stopLineCache.clear();
		m_laneArrowCache.clear();
	}
}

void RoadRenderer::invalidateAllCaches()
{
	m_partMeshCache.clear();
	m_partLodBatchCache.clear();
	m_laneCache.clear();
	m_marginCache.clear();
	m_nodeCapCache.clear();
	m_nodeCapLaneCache.clear();
	m_stopLineCache.clear();
	m_laneArrowCache.clear();
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
	auto meshIt = m_partMeshCache.find(edge.id);
	if (meshIt == m_partMeshCache.end())
	{
		const auto bez = network.getBezier(edge.id);
		if (!bez) return;
		auto entries = buildPartMeshes(edge, *bez, world, marginA, marginB);
		if (entries.isEmpty()) return;
		meshIt = m_partMeshCache.emplace(edge.id, std::move(entries)).first;
		m_marginCache[edge.id] = { marginA, marginB };
		// 遠距離用 combined LOD バッチも同じベジェから同時に構築する
		m_partLodBatchCache[edge.id] = buildPartLodBatches(edge, *bez, world, marginA, marginB);
	}

	// ---- 部品ごとに描画 ----
	if (isClose)
	{
		for (const auto& entry : meshIt->second)
		{
			if (entry.texture)
				entry.meshPair.detail.draw(*entry.texture, entry.color.removeSRGBCurve());
			else
				entry.meshPair.detail.draw(entry.color.removeSRGBCurve());
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
					batch.mesh.draw(*batch.texture, batch.color.removeSRGBCurve());
				else
					batch.mesh.draw(batch.color.removeSRGBCurve());
			}
		}
	}

	// ---- 車線区画線（遠方では描画しない） ----
	if (isClose)
	{
		if (!m_laneCache.contains(edge.id))
		{
			const auto bez = network.getBezier(edge.id);
			if (!bez) return;
			m_laneCache[edge.id] = buildLaneLineBatches(edge, *bez, world, marginA, marginB);
		}

		for (const auto& b : m_laneCache[edge.id])
			b.mesh.draw(b.color);
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
					const float terrainY = world.computeHeight(
						static_cast<float>(pos.x), static_cast<float>(pos.z));
					constexpr float kRoadBedThickness = 0.5f;
					const float topY = static_cast<float>(pos.y + kRoadSurfaceLift) - kRoadBedThickness;
					const float height = topY - terrainY;
					if (height < 1.0f) continue;

					// 直方体メッシュ: 幅 2m × 奥行 1.5m × 高さ
					constexpr float kPierW = 2.0f;
					constexpr float kPierD = 1.5f;

					const Float3 center{ static_cast<float>(pos.x),
				                     terrainY + height * 0.5f,
				                     static_cast<float>(pos.z) };
					piers << Mesh{ MeshData::Box(center, Float3{ kPierW, height, kPierD }) };
				}
				m_pierMeshCache[edge.id] = std::move(piers);
			}
		}

		const ColorF pierColor = ColorF{ 0.55, 0.53, 0.50 }.removeSRGBCurve();
		for (const auto& m : m_pierMeshCache[edge.id])
			m.draw(pierColor);
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
			mesh.draw(*entry.texture, entry.color.removeSRGBCurve());
		else
			mesh.draw(entry.color.removeSRGBCurve());
	}

	// 車線区画線（近距離のみ）
	if (isClose)
	{
		if (!m_nodeCapLaneCache.contains(nodeId))
			m_nodeCapLaneCache[nodeId] = buildNodeCapLaneLines(network, nodeId, world);

		for (const auto& b : m_nodeCapLaneCache[nodeId])
			b.mesh.draw(b.color);
	}

	// 停止線（Stop / Signal の Entry 側のみ）
	if (!m_stopLineCache.contains(nodeId))
		m_stopLineCache[nodeId] = buildStopLineBatches(network, nodeId, world);

	for (const auto& b : m_stopLineCache[nodeId])
		b.mesh.draw(b.color);

	// 路面標示矢印（近距離のみ）
	if (isClose)
	{
		if (!m_laneArrowCache.contains(nodeId))
		{
			m_laneArrowCache[nodeId] = buildLaneArrowMeshes(network, nodeId, world);
		}
		for (const auto& b : m_laneArrowCache[nodeId])
		{
			b.mesh.draw(b.color);
		}
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
		const auto  sl = makeSlice(bezier, world, s, 2.0 + static_cast<double>(heightOffset), useElevation);
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

Array<PartMeshEntry> RoadRenderer::buildPartMeshes(const RoadEdge& edge, const CubicBezier& bezier,
                                                    const World& world,
                                                    float marginA, float marginB)
{
	StripRange range;
	if (!calcStripRange(bezier.totalLength, marginA, marginB, range)) return {};

	Array<PartMeshEntry> entries;

	for (const auto& part : edge.parts)
	{
		if (part.build != BuildState::Built) continue;

		const float oL = part.offset;
		const float oR = part.offset + part.width;

		const auto [color, heightOff, tex] = getPartVisual(part);

		const MeshData mdDetail = buildStripMesh(bezier, world, oL, oR, heightOff,
		                                         range.sStart, range.sEnd, 1.0f, edge.useElevation);
		if (mdDetail.vertices.isEmpty()) continue;
		const MeshData mdLod = buildStripMesh(bezier, world, oL, oR, heightOff,
		                                      range.sStart, range.sEnd, 0.25f, edge.useElevation);

		PartMeshEntry entry;
		entry.meshPair.detail = Mesh{ mdDetail };
		entry.meshPair.lod    = mdLod.vertices.isEmpty() ? Mesh{ mdDetail } : Mesh{ mdLod };
		entry.color   = color;
		entry.texture = tex;
		entries << std::move(entry);
	}

	return entries;
}

Array<PartLodBatch> RoadRenderer::buildPartLodBatches(const RoadEdge& edge, const CubicBezier& bezier,
                                                       const World& world,
                                                       float marginA, float marginB) const
{
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
		if (part.build != BuildState::Built) continue;

		const float oL = part.offset;
		const float oR = part.offset + part.width;
		const auto [color, heightOff, tex] = getPartVisual(part);

		const MeshData md = buildStripMesh(bezier, world, oL, oR, heightOff,
		                                   range.sStart, range.sEnd, 0.25f, edge.useElevation);
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
			                  sStart, sEnd, 2.05f, edge.useElevation);
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
			                  sStart, sEnd, 2.05f, edge.useElevation);
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
			                  2.05f, edge.useElevation);
			if (!md.vertices.isEmpty())
			{
				batches << LaneLineBatch{ ColorF{ 1, 1, 1 }.removeSRGBCurve(), Mesh{ md } };
			}
		}
	}

	return batches;
}

Array<PartMeshEntry> RoadRenderer::buildNodeCapParts(const RoadNetwork& network, int nodeId,
                                                     const World& world, int div)
{
	const RoadNode* node = network.getNode(nodeId);
	if (!node || node->attachments.size() < 2) return {};

	// ---- ① 各エッジの切断点情報を収集（角度順ソート）----
	struct EdgeCapInfo
	{
		Vec3  capTan;     // ノード外向き XZ 正規化接線
		Vec3  capCenter;  // 切断点中心（Y=地形+リフト）
		Vec3  right;      // XZ 直角右ベクトル
		double angle;
		const RoadEdge* edge;
		bool  isNodeA;
	};

	Array<EdgeCapInfo> infos;
	for (const auto& att : node->attachments)
	{
		const RoadEdge* edge = network.getEdge(att.edgeId);
		if (!edge) { continue; }
		const auto bezOpt = network.getBezier(edge->id);
		if (!bezOpt) { continue; }
		const CubicBezier& bez = *bezOpt;

		const bool isNodeA = (edge->nodeA == nodeId);
		const float capRad = isNodeA ? edge->cutoffA : edge->cutoffB;
		constexpr float kOverlap = 0.1f;

		Vec3 capPos, capTan;
		if (isNodeA)
		{
			const float s = Clamp(capRad - kOverlap, 0.0f, bez.totalLength * 0.45f);
			capPos = bez.positionAt(s);
			capTan = bez.tangentAt(s);
		}
		else
		{
			const float s = Clamp(bez.totalLength - capRad + kOverlap,
			                      bez.totalLength * 0.55f, bez.totalLength);
			capPos = bez.positionAt(s);
			capTan = -bez.tangentAt(s);
		}

		const Vec2 tanXZ{ capTan.x, capTan.z };
		const double tanLen = tanXZ.length();
		if (tanLen < 1e-6) { continue; }
		const Vec2 tanNorm = tanXZ / tanLen;
		capTan = Vec3{ tanNorm.x, 0.0, tanNorm.y };

		const Vec3 right = tangentToRight(capTan);
		const double capY = edge->useElevation
			? capPos.y + kRoadSurfaceLift
			: world.computeHeight(static_cast<float>(capPos.x), static_cast<float>(capPos.z)) + kRoadSurfaceLift;

		EdgeCapInfo info;
		info.capTan   = capTan;
		info.capCenter = Vec3{ capPos.x, capY, capPos.z };
		info.right    = right;
		info.angle    = Math::Atan2(capTan.z, capTan.x);
		info.edge     = edge;
		info.isNodeA  = isNodeA;
		infos << info;
	}
	if (static_cast<int>(infos.size()) < 2) return {};
	infos.sort_by([](const EdgeCapInfo& a, const EdgeCapInfo& b) { return a.angle < b.angle; });

	const int N = static_cast<int>(infos.size());
	Array<PartMeshEntry> entries;

	// ---- ② Roadbed は旧アルゴリズム（フィレット+ブリッジ+中心ポリゴン）----
	{
		ColorF roadbedColor{ 0.36, 0.36, 0.36 };
		const Texture* roadbedTex = nullptr;
		for (const auto& info : infos)
		{
			for (const auto& part : info.edge->parts)
			{
				if (part.type == RoadPartType::Roadbed && !part.defId.isEmpty())
				{
					const auto& def = m_partRegistry.get(part.defId);
					roadbedColor = def.color;
					if (def.texture) roadbedTex = &(*def.texture);
					break;
				}
			}
			if (roadbedTex) break;
		}

		constexpr int lodDiv = 2;
		const MeshData mdDetail = buildNodeCapMeshForRange(network, nodeId, world, div, -999, 999, 0.0f);
		if (!mdDetail.vertices.isEmpty())
		{
			const MeshData mdLod = buildNodeCapMeshForRange(network, nodeId, world, lodDiv, -999, 999, 0.0f);
			PartMeshEntry entry;
			entry.meshPair.detail = Mesh{ mdDetail };
			entry.meshPair.lod    = mdLod.vertices.isEmpty() ? Mesh{ mdDetail } : Mesh{ mdLod };
			entry.color   = roadbedColor;
			entry.texture = roadbedTex;
			entries << std::move(entry);
		}
	}

	// ---- ③ 非Roadbed部品: 隣接ペアごとにペアリングして帯メッシュ生成 ----

	struct PartStrip
	{
		RoadPartType type;
		float innerOff;  // 中心に近い端（正の距離）
		float outerOff;  // 外側端（正の距離、innerOff < outerOff）
		float heightOff;
		ColorF color;
		const Texture* tex;
	};

	// エッジの片側（左 or 右）の部品リストを取得。中心から外側順にソート
	// outward フレームで offset を計算: capCenter + right * outwardOff が正しいワールド位置
	auto getStrips = [&](const EdgeCapInfo& info, bool leftSide) -> Array<PartStrip>
	{
		Array<PartStrip> strips;
		for (const auto& part : info.edge->parts)
		{
			if (part.build != BuildState::Built) continue;
			if (part.type == RoadPartType::Roadbed) continue;

			// A→B フレームの offset → outward フレーム（ノード外向き）
			float oL = part.offset;
			float oR = part.offset + part.width;
			// B端: outward = -(A→B方向) なので反転
			if (!info.isNodeA) { const float t = -oR; oR = -oL; oL = t; }

			// leftSide: outward offset < 0 の部品
			// rightSide: outward offset > 0 の部品
			const bool isLeft = (oR <= 0.01f);
			const bool isRight = (oL >= -0.01f);
			if (leftSide && !isLeft) continue;
			if (!leftSide && !isRight) continue;

			const auto [color, heightOff, tex] = getPartVisual(part);

			if (leftSide)
				strips << PartStrip{ part.type, Math::Abs(oR), Math::Abs(oL), heightOff, color, tex };
			else
				strips << PartStrip{ part.type, oL, oR, heightOff, color, tex };
		}
		strips.sort_by([](const PartStrip& a, const PartStrip& b) { return a.innerOff < b.innerOff; });
		return strips;
	};

	// ベジェ帯メッシュ生成ヘルパー
	auto makeBezBand = [&](const Vec3& p0i, const Vec3& p0o, const Vec3& p3i, const Vec3& p3o,
	                       const Vec3& tan0, const Vec3& tan3, const PartStrip& s) -> Optional<PartMeshEntry>
	{
		const Vec2 p0i_xz{ p0i.x, p0i.z }, p0o_xz{ p0o.x, p0o.z };
		const Vec2 p3i_xz{ p3i.x, p3i.z }, p3o_xz{ p3o.x, p3o.z };
		const double dI = Max((p3i_xz - p0i_xz).length() / 3.0, 0.5);
		const double dO = Max((p3o_xz - p0o_xz).length() / 3.0, 0.5);
		const Vec2 t0xz{ -tan0.x, -tan0.z }, t3xz{ -tan3.x, -tan3.z };

		auto bez = [](const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& d, double t) {
			const double m = 1.0 - t;
			return a*(m*m*m) + b*(3*m*m*t) + c*(3*m*t*t) + d*(t*t*t);
		};

		MeshData md;
		for (int k = 0; k < div; ++k)
		{
			const float t0 = k / static_cast<float>(div);
			const float t1 = (k+1) / static_cast<float>(div);
			const Vec2 i0 = bez(p0i_xz, p0i_xz+t0xz*dI, p3i_xz+t3xz*dI, p3i_xz, t0);
			const Vec2 i1 = bez(p0i_xz, p0i_xz+t0xz*dI, p3i_xz+t3xz*dI, p3i_xz, t1);
			const Vec2 o0 = bez(p0o_xz, p0o_xz+t0xz*dO, p3o_xz+t3xz*dO, p3o_xz, t0);
			const Vec2 o1 = bez(p0o_xz, p0o_xz+t0xz*dO, p3o_xz+t3xz*dO, p3o_xz, t1);
			const double y0 = p0i.y + (p3i.y - p0i.y) * t0;
			const double y1 = p0i.y + (p3i.y - p0i.y) * t1;
			const uint32 b = static_cast<uint32>(md.vertices.size());
			md.vertices << makeVertWorldUV({i0.x,y0,i0.y}) << makeVertWorldUV({o0.x,y0,o0.y})
			            << makeVertWorldUV({i1.x,y1,i1.y}) << makeVertWorldUV({o1.x,y1,o1.y});
			appendQuad(md.indices, b, b+1, b+2, b+3);
		}
		if (md.vertices.isEmpty()) return none;
		PartMeshEntry e;
		e.meshPair.detail = Mesh{md}; e.meshPair.lod = Mesh{md};
		e.color = s.color; e.texture = s.tex;
		return e;
	};

	for (int i = 0; i < N; ++i)
	{
		const int next = (i + 1) % N;
		const EdgeCapInfo& ei = infos[i];
		const EdgeCapInfo& en = infos[next];

		const auto stripsL = getStrips(ei, true);
		const auto stripsR = getStrips(en, false);

		// ペアリング: L を走査し R から同種 type を探す。部品タイプの canonical 順でソート
		struct PairEntry
		{
			int idxL, idxR;
			RoadPartType type;
		};

		// 部品タイプの canonical 順序（中心→外側）
		auto typeOrder = [](RoadPartType t) -> int
		{
			switch (t)
			{
			case RoadPartType::Shoulder:  return 0;
			case RoadPartType::Curb:      return 1;
			case RoadPartType::Gutter:    return 2;
			case RoadPartType::Sidewalk:  return 3;
			case RoadPartType::BikeLane:  return 4;
			case RoadPartType::Guardrail: return 5;
			case RoadPartType::Wall:      return 6;
			case RoadPartType::Slope:     return 7;
			default: return 99;
			}
		};

		Array<PairEntry> pairs;
		{
			HashSet<int> usedR;
			for (size_t jL = 0; jL < stripsL.size(); ++jL)
			{
				int matchR = -1;
				for (size_t jR = 0; jR < stripsR.size(); ++jR)
				{
					if (usedR.contains(static_cast<int>(jR))) continue;
					if (stripsR[jR].type == stripsL[jL].type)
					{ matchR = static_cast<int>(jR); usedR.insert(matchR); break; }
				}
				pairs << PairEntry{ static_cast<int>(jL), matchR, stripsL[jL].type };
			}
			for (size_t jR = 0; jR < stripsR.size(); ++jR)
			{
				if (!usedR.contains(static_cast<int>(jR)))
					pairs << PairEntry{ -1, static_cast<int>(jR), stripsR[jR].type };
			}
			pairs.sort_by([&](const PairEntry& a, const PairEntry& b) { return typeOrder(a.type) < typeOrder(b.type); });
		}

		// Roadbed 外端（テーパーの innerBound 初期値）
		auto roadbedOuter = [&](const EdgeCapInfo& info) -> double
		{
			double r = 0.0;
			for (const auto& p : info.edge->parts)
			{
				if (p.type != RoadPartType::Roadbed || p.build != BuildState::Built) continue;
				float oL = p.offset, oR = p.offset + p.width;
				if (!info.isNodeA) { const float t = -oR; oR = -oL; oL = t; }
				r = Max(r, static_cast<double>(Max(Math::Abs(oL), Math::Abs(oR))));
			}
			return r;
		};
		const double rbOutR = roadbedOuter(en);
		const double rbOutL = roadbedOuter(ei);

		// テーパー先: 前後のペア済み部品の隣接境界の中間
		auto findTaperOff = [&](size_t pi, bool forR) -> double
		{
			const double defInner = forR ? rbOutR : rbOutL;
			double innerBound = defInner;
			double outerBound = 1e9;
			for (size_t k = 0; k < pairs.size(); ++k)
			{
				const int idx = forR ? pairs[k].idxR : pairs[k].idxL;
				if (idx < 0) continue;
				const auto& s = forR ? stripsR[idx] : stripsL[idx];
				if (k < pi) innerBound = Max(innerBound, static_cast<double>(s.outerOff));
				if (k > pi) { outerBound = Min(outerBound, static_cast<double>(s.innerOff)); break; }
			}
			if (outerBound > 1e8) outerBound = innerBound;
			return (innerBound + outerBound) * 0.5;
		};

		for (size_t pi = 0; pi < pairs.size(); ++pi)
		{
			const auto& pe = pairs[pi];
			const bool hasL = (pe.idxL >= 0);
			const bool hasR = (pe.idxR >= 0);
			const PartStrip& ref = hasL ? stripsL[pe.idxL] : stripsR[pe.idxR];

			Vec3 p0inner, p0outer;
			if (hasL)
			{
				const auto& s = stripsL[pe.idxL];
				const Vec3 h{ 0, static_cast<double>(s.heightOff), 0 };
				p0inner = ei.capCenter - ei.right * static_cast<double>(s.innerOff) + h;
				p0outer = ei.capCenter - ei.right * static_cast<double>(s.outerOff) + h;
			}
			else
			{
				const Vec3 h{ 0, static_cast<double>(ref.heightOff), 0 };
				const double off = findTaperOff(pi, false);
				p0inner = ei.capCenter - ei.right * off + h;
				p0outer = p0inner;
			}

			Vec3 p3inner, p3outer;
			if (hasR)
			{
				const auto& s = stripsR[pe.idxR];
				const Vec3 h{ 0, static_cast<double>(s.heightOff), 0 };
				p3inner = en.capCenter + en.right * static_cast<double>(s.innerOff) + h;
				p3outer = en.capCenter + en.right * static_cast<double>(s.outerOff) + h;
			}
			else
			{
				const Vec3 h{ 0, static_cast<double>(ref.heightOff), 0 };
				const double off = findTaperOff(pi, true);
				p3inner = en.capCenter + en.right * off + h;
				p3outer = p3inner;
			}

			if (auto e = makeBezBand(p0inner, p0outer, p3inner, p3outer, ei.capTan, en.capTan, ref))
				entries << std::move(*e);
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
	if (node->type == NodeType::Intersection)
	{
		return {};
	}

	Array<LaneLineBatch> batches;

	// === Joint (Blend) — 別メソッドで処理 ===
	if (node->attachments.size() == 2 && node->type == NodeType::Joint
	    && node->transition == NodeTransition::Blend)
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
		: world.computeHeight(static_cast<float>(node->position.x),
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
			: world.computeHeight(static_cast<float>(pos.x), static_cast<float>(pos.z)) + kRoadLineLift;

		const Vec3 p0{ pos.x + right.x * entryMin, lineY, pos.z + right.z * entryMin };
		const Vec3 p1{ pos.x + right.x * entryMax, lineY, pos.z + right.z * entryMax };
		appendStraightLine(batches, p0, p1, 0.3f, ColorF{ 1.0, 1.0, 1.0 });
	}
	return batches;
}

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

	// 参照フレーム: edgeA の切断点を原点とし、右ベクトルと前進方向を幾何的に取得
	const auto refZero  = calcEdgeLine(*edgeA, 0.0f);
	const auto refUnit  = calcEdgeLine(*edgeA, 1.0f);
	const Vec3 refRight = (refUnit.pos - refZero.pos).normalized();
	const Vec3 refForward = Vec3{ refZero.tangent.x, 0.0, refZero.tangent.z }.normalized();
	const Vec3 refOrigin  = node.position;

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
			const Vec3  laneCenter = (infoL.pos + infoR.pos) * 0.5;

			const double lProj = (infoL.pos - laneCenter).dot(refRight);
			const bool   swap  = (lProj > 0.0);

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
			info.centerCommon = static_cast<float>((laneCenter - refOrigin).dot(refRight));

			const bool outgoing = (L.dir == LaneDir::Forward) == isNodeAEdge;
			const Vec3 travelDir = outgoing ? -infoL.tangent : infoL.tangent;
			info.rawFlow = (travelDir.dot(refForward) > 0.0) ? 0 : 1;
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
			                 lineStyle.lineWidth, lineStyle.color, world);
		}
		if (l.lineRight != LineType::None)
		{
			const auto lineStyle = lineStyleFor(l.lineRight);
			appendBezierLine(batches, l.rightPos, targetPos, l.rightTan, targetTan,
			                 lineStyle.lineWidth, lineStyle.color, world);
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
			const LineType lineL = (lA.lineLeft  != LineType::None) ? lA.lineLeft  : lB.lineLeft;
			const LineType lineR = (lA.lineRight != LineType::None) ? lA.lineRight : lB.lineRight;
			if (lineL != LineType::None)
			{
				const auto lineStyle = lineStyleFor(lineL);
				appendBezierLine(batches, lA.leftPos, lB.leftPos, lA.leftTan, lB.leftTan,
				                 lineStyle.lineWidth, lineStyle.color, world);
			}
			if (lineR != LineType::None)
			{
				const auto lineStyle = lineStyleFor(lineR);
				appendBezierLine(batches, lA.rightPos, lB.rightPos, lA.rightTan, lB.rightTan,
				                 lineStyle.lineWidth, lineStyle.color, world);
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
		return std::make_pair(
			outerIsRight ? pY.rightPos : pY.leftPos,
			outerIsRight ? pY.rightTan : pY.leftTan);
	};

	const auto fallbackFromA = calcEdgeLine(*edgeB, 0.0f);
	const auto fallbackFromB = calcEdgeLine(*edgeA, 0.0f);

	for (int xIdx : unpairedA)
	{
		const auto tgt = computeTaperTarget(xIdx, true);
		const Vec3 tPos = tgt ? tgt->first  : fallbackFromA.pos;
		const Vec3 tTan = tgt ? tgt->second : fallbackFromA.tangent;
		drawTaper(infosA[xIdx], tPos, tTan);
	}
	for (int xIdx : unpairedB)
	{
		const auto tgt = computeTaperTarget(xIdx, false);
		const Vec3 tPos = tgt ? tgt->first  : fallbackFromB.pos;
		const Vec3 tTan = tgt ? tgt->second : fallbackFromB.tangent;
		drawTaper(infosB[xIdx], tPos, tTan);
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
		: static_cast<double>(world.computeHeight(static_cast<float>(anchorX), static_cast<float>(anchorZ)));
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
		const bool boardFacesTan = atA;

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

	/// @brief 1路線あたりの国道標識設置本数（route 全長を等分した各区間中央に配置）
	constexpr int kSignsPerRoute = 2;
	const auto& routeMeta = RoadSign::poleMetadata();

	const int n = static_cast<int>(route.edgeIds.size());
	for (int k = 0; k < kSignsPerRoute; ++k)
	{
		const double tGlobal = (k + 0.5) / static_cast<double>(kSignsPerRoute);
		const double pos     = tGlobal * n;
		const int    idx     = Clamp(static_cast<int>(pos), 0, n - 1);
		const float  tLocal  = static_cast<float>(pos - idx);

		const auto bezier = network.getBezier(route.edgeIds[idx]);
		if (not bezier) continue;

		const RoadEdge* edge = network.getEdge(route.edgeIds[idx]);
		const float roadRightEdge = edge
			? RoadSign::roadbedExtentsOf(*edge).right
			: 5.0f;
		const float lateral = roadRightEdge + static_cast<float>(RoadSign::kSideMargin_m);
		const float arcLen  = tLocal * bezier->totalLength;

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
						vy = world.computeHeight(static_cast<float>(clPos.x), static_cast<float>(clPos.z)) + kRoadLineLift;
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

MeshData RoadRenderer::buildNodeCapMeshForRange(const RoadNetwork& network, int nodeId,
                                                const World& world, int div,
                                                float partOffsetL, float partOffsetR, float heightOffset) const
{
	// ④ 中心ポリゴン: 各フィレット中点のファン三角形化（N≥3 のみ）
	// 中心頂点を使わないため、傾斜地でのテント状クリースが生じない。

	const RoadNode* node = network.getNode(nodeId);
	if (!node || node->attachments.size() < 2) return MeshData{};

	// ---- ① 各エッジの切断点情報を収集 ----
	struct EdgeInfo
	{
		Vec3   capTan;       ///< ノードから外向きの XZ 正規化接線
		Vec3   leftCorner;   ///< 切断点の左端（Y=地形+リフト）
		Vec3   rightCorner;  ///< 切断点の右端（Y=地形+リフト）
		double angle;        ///< XZ 平面上の外向き角度（ソート用）
	};

	Array<EdgeInfo> infos;

	for (const auto& att : node->attachments)
	{
		const int eid = att.edgeId;
		const RoadEdge* edge = network.getEdge(eid);
		if (!edge) { continue; }
		const auto bezOpt = network.getBezier(eid);
		if (!bezOpt) { continue; }
		const CubicBezier& bez = *bezOpt;

		// このエッジ自身の Roadbed 部品の幅範囲を集計する
		const bool isNodeA = (edge->nodeA == nodeId);
		float edgeOffL = partOffsetL;
		float edgeOffR = partOffsetR;
		bool hasOwnRoadbed = false;
		for (const auto& p : edge->parts)
		{
			if (p.type != RoadPartType::Roadbed || p.build != BuildState::Built) { continue; }
			if (!hasOwnRoadbed)
			{
				edgeOffL = p.offset;
				edgeOffR = p.offset + p.width;
				hasOwnRoadbed = true;
			}
			else
			{
				edgeOffL = Min(edgeOffL, p.offset);
				edgeOffR = Max(edgeOffR, p.offset + p.width);
			}
		}
		const double oL = isNodeA ?  static_cast<double>(edgeOffL)
		                           : -static_cast<double>(edgeOffR);
		const double oR = isNodeA ?  static_cast<double>(edgeOffR)
		                           : -static_cast<double>(edgeOffL);
		const float  capRad = isNodeA ? edge->cutoffA : edge->cutoffB;

		// 道路メッシュと同じ s 値（kOverlap 分だけカットオフより手前）を使う
		constexpr float kOverlap = 0.1f;
		Vec3 capPos, capTan;
		if (edge->nodeA == nodeId)
		{
			const float s = Clamp(capRad - kOverlap, 0.0f, bez.totalLength * 0.45f);
			capPos = bez.positionAt(s);
			capTan = bez.tangentAt(s);
		}
		else
		{
			const float s = Clamp(bez.totalLength - capRad + kOverlap,
			                      bez.totalLength * 0.55f, bez.totalLength);
			capPos = bez.positionAt(s);
			capTan = -bez.tangentAt(s);
		}

		const Vec2   tanXZ  = Vec2{ capTan.x, capTan.z };
		const double tanLen = tanXZ.length();
		if (tanLen < 1e-6) { continue; }
		const Vec2 tanNorm = tanXZ / tanLen;
		capTan = Vec3{ tanNorm.x, 0.0, tanNorm.y };

		const Vec3  right    = tangentToRight(capTan);
		const double capY = edge->useElevation
			? capPos.y + kRoadSurfaceLift + static_cast<double>(heightOffset)
			: world.computeHeight(static_cast<float>(capPos.x), static_cast<float>(capPos.z))
			  + kRoadSurfaceLift + static_cast<double>(heightOffset);
		const Vec3  capCenter{ capPos.x, capY, capPos.z };

		EdgeInfo info;
		info.capTan      = capTan;
		info.leftCorner  = capCenter + right * oL;
		info.rightCorner = capCenter + right * oR;
		info.angle       = Math::Atan2(capTan.z, capTan.x);
		infos << info;
	}

	if (static_cast<int>(infos.size()) < 2) return MeshData{};

	infos.sort_by([](const EdgeInfo& a, const EdgeInfo& b) { return a.angle < b.angle; });

	const int N = static_cast<int>(infos.size());

	// ---- ② フィレット曲線を生成 ----
	// fillet[i]: infos[i].leftCorner → infos[(i+1)%N].rightCorner の DIV+1 点
	// Pavecity の bezier{road1_edge, road2_edge, -road1_dir, -road2_dir} に対応
	const int DIV  = div;     // 偶数（6=通常、2=LOD）
	const int HALF = DIV / 2;

	Array<Array<Vec3>> fillets(N);
	for (int i = 0; i < N; ++i)
	{
		const int       next = (i + 1) % N;
		const EdgeInfo& ei   = infos[i];
		const EdgeInfo& en   = infos[next];

		const Vec2   p0xz{ ei.leftCorner.x,  ei.leftCorner.z  };
		const Vec2   p3xz{ en.rightCorner.x, en.rightCorner.z };
		const double dist  = (p3xz - p0xz).length();
		const double scale = Max(dist / 3.0, 0.5);
		const Vec2   p1xz  = p0xz + Vec2{ -ei.capTan.x, -ei.capTan.z } * scale;
		const Vec2   p2xz  = p3xz + Vec2{ -en.capTan.x, -en.capTan.z } * scale;

		Array<Vec3> pts;
		pts.reserve(DIV + 1);
		for (int k = 0; k <= DIV; ++k)
		{
			// 端点は道路メッシュの角座標をそのまま使い、高さを一致させる
			if (k == 0)
			{
				pts << ei.leftCorner;
				continue;
			}
			if (k == DIV)
			{
				pts << en.rightCorner;
				continue;
			}
			const double t  = k / static_cast<double>(DIV);
			const double mt = 1.0 - t;
			const Vec2   ptXZ = p0xz * (mt*mt*mt)
			                  + p1xz * (3.0*mt*mt*t)
			                  + p2xz * (3.0*mt*t*t)
			                  + p3xz * (t*t*t);
			const double y = ei.leftCorner.y
			               + (en.rightCorner.y - ei.leftCorner.y) * k / DIV;
			pts << Vec3{ ptXZ.x, y, ptXZ.y };
		}
		fillets[i] = std::move(pts);
	}

	// ---- ③ BridgeTwoLine: fillet[i] 後半(逆順) ↔ fillet[next] 前半 ----
	// lineA: fa[DIV], fa[DIV-1], ..., fa[HALF]  (HALF+1 点)
	// lineB: fb[0],   fb[1],    ..., fb[HALF]   (HALF+1 点)
	// Pavecity BridgeTwoLine の三角形ワインディングに合わせる:
	//   {p0,p1,p2} + {p1,p3,p2}  → MergeTrianglesToMesh が反転 → {p2,p1,p0} + {p2,p3,p1}
	Array<Vertex3D>        vertices;
	Array<TriangleIndex32> indices;

	for (int i = 0; i < N; ++i)
	{
		const int          next = (i + 1) % N;
		const Array<Vec3>& fa   = fillets[i];
		const Array<Vec3>& fb   = fillets[next];

		for (int k = 0; k < HALF; ++k)
		{
			const Vec3& p0 = fa[DIV - k];      // lineA[k]   : rightCorner[next] 側
			const Vec3& p1 = fa[DIV - k - 1];  // lineA[k+1] : 中心側
			const Vec3& p2 = fb[k];             // lineB[k]   : leftCorner[next] 側
			const Vec3& p3 = fb[k + 1];         // lineB[k+1] : 中心側

			const uint32 base = static_cast<uint32>(vertices.size());
			vertices << makeVertWorldUV(p0)   // base+0
			         << makeVertWorldUV(p1)   // base+1
			         << makeVertWorldUV(p2)   // base+2
			         << makeVertWorldUV(p3);  // base+3

			indices << TriangleIndex32{ base + 0, base + 1, base + 2 };
			indices << TriangleIndex32{ base + 1, base + 3, base + 2 };
		}
	}

	// ---- ④ 中心ポリゴン（N≥3 のみ）----
	// Pavecity の mids ファン三角形化に対応
	// mids[i] = fillet[i][HALF]（各フィレットの中点）
	if (N >= 3)
	{
		Array<Vec3> mids;
		for (int i = 0; i < N; ++i)
			mids << fillets[i][HALF];

		while (static_cast<int>(mids.size()) >= 3)
		{
			const uint32 base = static_cast<uint32>(vertices.size());
			vertices << makeVertWorldUV(mids[0])   // base+0
			         << makeVertWorldUV(mids[1])   // base+1
			         << makeVertWorldUV(mids[2]);  // base+2
			indices << TriangleIndex32{ base + 2, base + 1, base + 0 };
			mids.erase(mids.begin() + 1);
		}
	}

	return MeshData{ vertices, indices };
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
		const bool entryOnRight = !isNodeA;
		float roadEdgeOffset = 0.0f;
		bool  foundRoadbed   = false;
		for (const auto& part : edge->parts)
		{
			if (part.type != RoadPartType::Roadbed) continue;
			const float edgePos = entryOnRight ? (part.offset + part.width) : part.offset;
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
			: static_cast<double>(world.computeHeight(
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
	const Texture& baseTex = TextureAsset(Asset::NationalRoadSign);
	if (not baseTex) return;

	const Font& fontNum = FontAsset(Asset::Arial24);
	constexpr int kTexSize = 256;

	for (const auto& route : network.routes())
	{
		if (!isDrawableNationalRoute(route)) continue;
		if (m_routeSignTexCache.contains(route.number)) continue;

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
		auto entries = buildPartMeshes(*edge, *bez, world, mA, mB);
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

