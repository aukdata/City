# include "../../stdafx.h"
#include "RoadRenderer.hpp"
#include <Siv3D/ViewFrustum.hpp>

// ─────────────────────────────────────────────────────────────────────────────
// 内部ヘルパー（無名名前空間）
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	/// @brief 接線の XZ 直角右ベクトルを返す（Y=0）
	Vec3 calcRight(const Vec3& tan)
	{
		const double lenXZ = Math::Sqrt(tan.x * tan.x + tan.z * tan.z);
		if (lenXZ > 1e-6)
			return Vec3{ tan.z / lenXZ, 0.0, -tan.x / lenXZ };
		return Vec3{ 1.0, 0.0, 0.0 };
	}

	/// @brief 弧長 s での中心・右ベクトルを返す（Y は地形 + リフト）
	struct SliceInfo { Vec3 center; Vec3 right; };
	SliceInfo makeSlice(const CubicBezier& bez, const World& world, float s, double terrainLift)
	{
		const Vec3  p  = bez.positionAt(s);
		// sampleHeight はチャンク未ロード時に 0 を返すため computeHeight を使う
		const float gy = world.computeHeight(static_cast<float>(p.x), static_cast<float>(p.z));
		return { Vec3{ p.x, gy + terrainLift, p.z }, calcRight(bez.tangentAt(s)) };
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
		float offset, float halfLW,
		float dashLength, float gapLength,
		float sStart, float sEnd,
		float terrainLift = 2.05f)
	{
		const float spanLen  = sEnd - sStart;
		if (spanLen <= 0.0f) return;

		const bool  solid    = (dashLength <= 0.0f || gapLength <= 0.0f);
		const float cycleLen = dashLength + gapLength;
		const int   N        = Clamp(static_cast<int>(spanLen / 2.0f) + 1, 5, 200);

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
				const auto    sl = makeSlice(bez, world, s, terrainLift);
				const Vec3    lc = sl.center + sl.right * offset;
				vertices << makeVert(lc - sl.right * halfLW, 0.0f, s / bez.totalLength);
				vertices << makeVert(lc + sl.right * halfLW, 1.0f, s / bez.totalLength);
			}

			appendQuad(indices, base, base + 1, base + 2, base + 3);
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// RoadRenderer 公開メソッド
// ─────────────────────────────────────────────────────────────────────────────

bool RoadRenderer::loadStyle(FilePathView tomlPath)
{
	const bool ok = m_styleRegistry.load(tomlPath);
	m_partRegistry.load(U"assets/road_parts");
	return ok;
}

void RoadRenderer::render(const RoadNetwork& network, const World& world,
                          const ViewFrustum& frustum, Vec3 cameraPos)
{
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
				m_nodeCapCache.erase(node.id);
				for (const auto& att : node.attachments)
				{
					const int eid = att.edgeId;
					m_partMeshCache.erase(eid);
					m_laneCache.erase(eid);
					m_marginCache.erase(eid);
					m_boundsCache.erase(eid);
				}
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

		const float mA = edgeMargin(edge, edge.nodeA);
		const float mB = edgeMargin(edge, edge.nodeB);

		drawEdge(edge, network, mA, mB, world, isClose);
	}

	// ---- ノードキャップ描画（交差点フィル）----
	for (const RoadNode& node : network.nodes())
	{
		if (node.id < 0) continue;

		// 距離チェック（float 演算のみ）
		const float ndx = static_cast<float>(node.position.x) - camX;
		const float ndz = static_cast<float>(node.position.z) - camZ;
		const float nodeDistSq = ndx * ndx + ndz * ndz;
		if (nodeDistSq > kDrawMaxDistSqF) continue;

		// 視錐台カリング
		if (!frustum.intersects(Sphere{ node.position, 30.0 })) continue;

		const bool isClose = nodeDistSq < kLodDistSqF;
		drawNodeCap(network, node.id, world, isClose);
	}
}

void RoadRenderer::invalidateEdgeCache(int edgeId, int nodeA, int nodeB)
{
	m_partMeshCache.erase(edgeId);
	m_laneCache.erase(edgeId);
	m_marginCache.erase(edgeId);
	m_boundsCache.erase(edgeId);
	if (nodeA >= 0 || nodeB >= 0)
	{
		if (nodeA >= 0) m_nodeCapCache.erase(nodeA);
		if (nodeB >= 0) m_nodeCapCache.erase(nodeB);
	}
	else
	{
		m_nodeCapCache.clear();
	}
}

void RoadRenderer::invalidateAllCaches()
{
	m_partMeshCache.clear();
	m_laneCache.clear();
	m_marginCache.clear();
	m_nodeCapCache.clear();
	m_boundsCache.clear();
}

void RoadRenderer::invalidateCachesAroundNode(int nodeId, const RoadNetwork& network)
{
	m_nodeCapCache.erase(nodeId);
	const RoadNode* node = network.getNode(nodeId);
	if (node)
	{
		for (const auto& att : node->attachments)
		{
			const int eid = att.edgeId;
			m_partMeshCache.erase(eid);
			m_laneCache.erase(eid);
			m_marginCache.erase(eid);
			m_boundsCache.erase(eid);
		}
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
			m_partMeshCache.erase(edge.id);
			m_laneCache.erase(edge.id);
			m_marginCache.erase(edge.id);
			m_nodeCapCache.erase(edge.nodeA);
			m_nodeCapCache.erase(edge.nodeB);
		}
	}

	// ---- 部品ごとのメッシュを構築・キャッシュ ----
	if (!m_partMeshCache.contains(edge.id))
	{
		const auto bez = network.getBezier(edge.id);
		if (!bez) return;
		auto entries = buildPartMeshes(edge, *bez, world, marginA, marginB);
		if (entries.isEmpty()) return;
		m_partMeshCache[edge.id] = std::move(entries);
		m_marginCache[edge.id] = { marginA, marginB };
	}

	// ---- 部品ごとに描画 ----
	for (const auto& entry : m_partMeshCache[edge.id])
	{
		const Mesh& mesh = isClose ? entry.meshPair.detail : entry.meshPair.lod;
		if (entry.texture)
			mesh.draw(*entry.texture, entry.color.removeSRGBCurve());
		else
			mesh.draw(entry.color.removeSRGBCurve());
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
		auto entries = buildNodeCapParts(network, nodeId, world, 6);
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
}

// ─────────────────────────────────────────────────────────────────────────────
// ヘルパー
// ─────────────────────────────────────────────────────────────────────────────

float RoadRenderer::edgeMargin(const RoadEdge& edge, int nodeId)
{
	return (nodeId == edge.nodeA) ? edge.cutoffA : edge.cutoffB;
}

// ─────────────────────────────────────────────────────────────────────────────
// メッシュ生成
// ─────────────────────────────────────────────────────────────────────────────

MeshData RoadRenderer::buildStripMesh(const CubicBezier& bezier, const World& world,
                                      float offsetL, float offsetR, float heightOffset,
                                      float sStart, float sEnd, float lodFactor) const
{
	const float spanLen = sEnd - sStart;
	if (spanLen <= 0.1f) return MeshData{};

	const int N = Clamp(static_cast<int>(spanLen / 2.0f * lodFactor) + 1,
	                    3, static_cast<int>(100 * lodFactor));

	constexpr float kTileV = 0.05f;  // UV タイリング [1/m]

	Array<Vertex3D> vertices;
	vertices.reserve((N + 1) * 2);

	for (int i = 0; i <= N; ++i)
	{
		const float s  = sStart + (i / static_cast<float>(N)) * spanLen;
		const auto  sl = makeSlice(bezier, world, s, 2.0 + static_cast<double>(heightOffset));
		const float v  = (s - sStart) * kTileV;

		const float uL = 0.0f;
		const float uR = 1.0f;
		vertices << makeVert(sl.center + sl.right * static_cast<double>(offsetL), uL, v);
		vertices << makeVert(sl.center + sl.right * static_cast<double>(offsetR), uR, v);
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
	constexpr float kOverlap = 0.1f;
	const float totalLen = bezier.totalLength;
	const float sStart   = Max(marginA - kOverlap, 0.0f);
	const float sEnd     = Min(totalLen - marginB + kOverlap, totalLen);
	if (sStart >= sEnd - 0.1f) return {};

	Array<PartMeshEntry> entries;

	for (const auto& part : edge.parts)
	{
		if (part.build != BuildState::Built) continue;

		const float oL = part.offset;
		const float oR = part.offset + part.width;

		// heightOffset を RoadPartDef から取得
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
			// defId 未設定のフォールバック色
			switch (part.type)
			{
			case RoadPartType::Roadbed:   color = ColorF{ 0.36, 0.36, 0.36 }; break;
			case RoadPartType::Sidewalk:  color = ColorF{ 0.72, 0.70, 0.68 }; heightOff = 0.15f; break;
			case RoadPartType::Curb:      color = ColorF{ 0.78, 0.76, 0.72 }; heightOff = 0.15f; break;
			case RoadPartType::Median:    color = ColorF{ 0.75, 0.73, 0.70 }; break;
			case RoadPartType::Shoulder:  color = ColorF{ 0.40, 0.40, 0.40 }; break;
			case RoadPartType::Slope:     color = ColorF{ 0.45, 0.58, 0.35 }; break;
			case RoadPartType::Guardrail: color = ColorF{ 0.82, 0.82, 0.82 }; break;
			default: break;
			}
		}

		const MeshData mdDetail = buildStripMesh(bezier, world, oL, oR, heightOff, sStart, sEnd, 1.0f);
		if (mdDetail.vertices.isEmpty()) continue;
		const MeshData mdLod = buildStripMesh(bezier, world, oL, oR, heightOff, sStart, sEnd, 0.25f);

		PartMeshEntry entry;
		entry.meshPair.detail = Mesh{ mdDetail };
		entry.meshPair.lod    = mdLod.vertices.isEmpty() ? Mesh{ mdDetail } : Mesh{ mdLod };
		entry.color   = color;
		entry.texture = tex;
		entries << std::move(entry);
	}

	return entries;
}

Array<RoadRenderer::LaneLineBatch> RoadRenderer::buildLaneLineBatches(
	const RoadEdge& edge, const CubicBezier& bezier,
	const World& world,
	float marginA, float marginB) const
{
	constexpr float kOverlap = 0.1f;
	const float sStart = Max(marginA - kOverlap, 0.0f);
	const float sEnd   = Min(bezier.totalLength - marginB + kOverlap, bezier.totalLength);
	if (sStart >= sEnd - 0.1f || edge.lanes.size() < 2) return {};

	// LineType ごとの色・破線パターン
	struct LineStyle
	{
		ColorF color;
		float  lineWidth;
		float  dashLength;
		float  gapLength;
	};

	auto styleForType = [](LineType lt) -> LineStyle
	{
		switch (lt)
		{
		case LineType::SolidWhite:  return { ColorF{ 1.0, 1.0, 1.0 }, 0.15f, 0.0f, 0.0f };
		case LineType::DashedWhite: return { ColorF{ 1.0, 1.0, 1.0 }, 0.15f, 8.0f, 12.0f };
		case LineType::SolidYellow: return { ColorF{ 1.0, 0.9, 0.0 }, 0.20f, 0.0f, 0.0f };
		case LineType::DoubleYellow:return { ColorF{ 1.0, 0.9, 0.0 }, 0.20f, 0.0f, 0.0f };
		default: return {};
		}
	};

	// 各車線の右境界に lineRight の線を描画（Lane の offsetA_R を使用）
	Array<LaneLineBatch> batches;

	for (int i = 0; i < static_cast<int>(edge.lanes.size()); ++i)
	{
		const auto& lane = edge.lanes[i];

		// 右境界の線（最右端車線の外側は描画しない: 路肩線は別途）
		if (lane.lineRight != LineType::None && i + 1 < static_cast<int>(edge.lanes.size()))
		{
			// 車線の右端位置 = 道路中心からのオフセット
			const float offset = lane.offsetA_R;
			const auto ls = styleForType(lane.lineRight);

			MeshData md;
			appendDashedStrip(md.vertices, md.indices,
			                  bezier, world,
			                  offset, ls.lineWidth * 0.5f,
			                  ls.dashLength, ls.gapLength,
			                  sStart, sEnd);
			if (!md.vertices.isEmpty())
				batches << LaneLineBatch{ ls.color.removeSRGBCurve(), Mesh{ md } };
		}
	}

	return batches;
}

Array<PartMeshEntry> RoadRenderer::buildNodeCapParts(const RoadNetwork& network, int nodeId,
                                                     const World& world, int div)
{
	const RoadNode* node = network.getNode(nodeId);
	if (!node || node->attachments.size() < 2) return {};

	// 接続エッジから部品の種類を収集（共通部品のみ）
	// 全エッジに共通する部品種別の合併セットを作り、各部品ごとにフィレットを生成
	// 簡略化: 最初のエッジの parts を基準にする
	const RoadEdge* refEdge = nullptr;
	for (const auto& att : node->attachments)
	{
		refEdge = network.getEdge(att.edgeId);
		if (refEdge && !refEdge->parts.isEmpty()) break;
	}
	if (!refEdge || refEdge->parts.isEmpty()) return {};

	Array<PartMeshEntry> entries;

	for (const auto& refPart : refEdge->parts)
	{
		if (refPart.build != BuildState::Built) continue;

		float heightOff = 0.0f;
		ColorF color{ 0.35 };
		const Texture* tex = nullptr;

		if (!refPart.defId.isEmpty())
		{
			const auto& def = m_partRegistry.get(refPart.defId);
			heightOff = def.heightOffset;
			color     = def.color;
			if (def.texture) tex = &(*def.texture);
		}
		else
		{
			switch (refPart.type)
			{
			case RoadPartType::Roadbed:   color = ColorF{ 0.36, 0.36, 0.36 }; break;
			case RoadPartType::Sidewalk:  color = ColorF{ 0.72, 0.70, 0.68 }; heightOff = 0.15f; break;
			case RoadPartType::Curb:      color = ColorF{ 0.78, 0.76, 0.72 }; heightOff = 0.15f; break;
			case RoadPartType::Median:    color = ColorF{ 0.75, 0.73, 0.70 }; break;
			case RoadPartType::Shoulder:  color = ColorF{ 0.40, 0.40, 0.40 }; break;
			case RoadPartType::Slope:     color = ColorF{ 0.45, 0.58, 0.35 }; break;
			case RoadPartType::Guardrail: color = ColorF{ 0.82, 0.82, 0.82 }; break;
			default: break;
			}
		}

		const int lodDiv = Max(div / 3, 2);
		const MeshData mdDetail = buildNodeCapMeshForRange(network, nodeId, world, div,
			refPart.offset, refPart.offset + refPart.width, heightOff);
		if (mdDetail.vertices.isEmpty()) continue;
		const MeshData mdLod = buildNodeCapMeshForRange(network, nodeId, world, lodDiv,
			refPart.offset, refPart.offset + refPart.width, heightOff);

		PartMeshEntry entry;
		entry.meshPair.detail = Mesh{ mdDetail };
		entry.meshPair.lod    = mdLod.vertices.isEmpty() ? Mesh{ mdDetail } : Mesh{ mdLod };
		entry.color   = color;
		entry.texture = tex;
		entries << std::move(entry);
	}

	return entries;
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
		if (!edge) continue;
		const auto bezOpt = network.getBezier(eid);
		if (!bezOpt) continue;
		const CubicBezier& bez = *bezOpt;

		// 引数の部品幅範囲を使用
		const double oL = static_cast<double>(partOffsetL);
		const double oR = static_cast<double>(partOffsetR);
		const float  capRad = (edge->nodeA == nodeId) ? edge->cutoffA : edge->cutoffB;

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
		if (tanLen < 1e-6) continue;
		const Vec2 tanNorm = tanXZ / tanLen;
		capTan = Vec3{ tanNorm.x, 0.0, tanNorm.y };

		const Vec3  right    = calcRight(capTan);
		const float gy       = world.computeHeight(static_cast<float>(capPos.x),
		                                           static_cast<float>(capPos.z));
		const Vec3  capCenter{ capPos.x, gy + 2.0 + static_cast<double>(heightOffset), capPos.z };

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
			vertices << makeVert(p0, 0.0f, 0.0f)   // base+0
			         << makeVert(p1, 0.0f, 1.0f)   // base+1
			         << makeVert(p2, 1.0f, 0.0f)   // base+2
			         << makeVert(p3, 1.0f, 1.0f);  // base+3

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
			vertices << makeVert(mids[0], 0.5f, 0.5f)   // base+0
			         << makeVert(mids[1], 0.5f, 0.5f)   // base+1
			         << makeVert(mids[2], 0.5f, 0.5f);  // base+2
			indices << TriangleIndex32{ base + 2, base + 1, base + 0 };
			mids.erase(mids.begin() + 1);
		}
	}

	return MeshData{ vertices, indices };
}
