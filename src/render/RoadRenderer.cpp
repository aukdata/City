# include "../../stdafx.h"
#include "RoadRenderer.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// 内部ヘルパー
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	/// @brief 「右ベクトル」を接線ベクトルの xz 直角方向で計算する
	Vec3 calcRight(const Vec3& tan)
	{
		const float lenXZ = static_cast<float>(Math::Sqrt(tan.x * tan.x + tan.z * tan.z));
		if (lenXZ > 1e-6f)
			return Vec3{ tan.z / lenXZ, 0.0, -tan.x / lenXZ };
		return Vec3{ 1.0, 0.0, 0.0 };
	}

	/// @brief ベジェ曲線上の点 s における中心・右ベクトル・地形高さを返す
	struct SliceInfo { Vec3 center; Vec3 right; };
	SliceInfo makeSlice(const CubicBezier& bez, const World& world, float s, float terrainLift)
	{
		const Vec3  p       = bez.positionAt(s);
		const float gy      = world.sampleHeight(static_cast<float>(p.x), static_cast<float>(p.z));
		const Vec3  center  = Vec3{ p.x, gy + terrainLift, p.z };
		const Vec3  right   = calcRight(bez.tangentAt(s));
		return { center, right };
	}

	/// @brief Vertex3D を生成するユーティリティ
	Vertex3D makeVert(const Vec3& pos, float u, float v)
	{
		Vertex3D vt;
		vt.pos    = Float3{ static_cast<float>(pos.x), static_cast<float>(pos.y), static_cast<float>(pos.z) };
		vt.normal = Float3{ 0.0f, 1.0f, 0.0f };
		vt.tex    = Float2{ u, v };
		return vt;
	}

	/// @brief 四角形 (quad) を頂点配列・インデックス配列に追記する
	/// @param iL0/iR0  現在スライスの左右頂点インデックス
	/// @param iL1/iR1  次スライスの左右頂点インデックス
	void appendQuad(Array<TriangleIndex32>& indices,
	                uint32 iL0, uint32 iR0, uint32 iL1, uint32 iR1)
	{
		// CW ワインディング（Siv3D DirectX 上方向が表面）
		indices << TriangleIndex32{ iL0, iL1, iR0 };
		indices << TriangleIndex32{ iR0, iL1, iR1 };
	}

	/// @brief ベジェ曲線に沿って破線（または実線）の細いポリゴン帯を meshData に追記する
	/// @param offset      道路中心からの横方向オフセット [m]（正 = 右）
	/// @param halfLW      線幅の半値 [m]
	/// @param dashLength  破線の実部長 [m]（0 以下 = 実線）
	/// @param gapLength   破線の間隔長 [m]
	/// @param terrainLift 地形面からの浮き高さ [m]（路面より少し上にする）
	void appendDashedStrip(
		Array<Vertex3D>&        vertices,
		Array<TriangleIndex32>& indices,
		const CubicBezier&      bez,
		const World&            world,
		float offset, float halfLW,
		float dashLength, float gapLength,
		float terrainLift = 2.05f)
	{
		const float totalLen  = bez.totalLength;
		const bool  solid     = (dashLength <= 0.0f || gapLength <= 0.0f);
		const float cycleLen  = dashLength + gapLength;

		// 2m あたり 1 セグメントを目安にしつつ [20, 200] でクランプ
		const int N = Clamp(static_cast<int>(totalLen / 2.0f) + 1, 20, 200);

		for (int i = 0; i < N; ++i)
		{
			const float s0   = (i       / static_cast<float>(N)) * totalLen;
			const float s1   = ((i + 1) / static_cast<float>(N)) * totalLen;
			const float sMid = (s0 + s1) * 0.5f;

			// 破線: 中点がギャップ区間なら描画スキップ
			if (!solid && (Math::Fmod(sMid, cycleLen) >= dashLength))
				continue;

			// 独立 quad（4 頂点）として追記
			const uint32 base = static_cast<uint32>(vertices.size());

			for (int j = 0; j <= 1; ++j)
			{
				const float   s = (j == 0) ? s0 : s1;
				const auto    sl = makeSlice(bez, world, s, terrainLift);
				const Vec3 lc = sl.center + sl.right * static_cast<double>(offset);

				vertices << makeVert(lc - sl.right * static_cast<double>(halfLW), 0.0f, s / totalLen);
				vertices << makeVert(lc + sl.right * static_cast<double>(halfLW), 1.0f, s / totalLen);
			}

			// base=iL0, base+1=iR0, base+2=iL1, base+3=iR1
			appendQuad(indices, base, base + 1, base + 2, base + 3);
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// RoadRenderer 実装
// ─────────────────────────────────────────────────────────────────────────────

bool RoadRenderer::loadStyle(FilePathView tomlPath)
{
	return m_styleRegistry.load(tomlPath);
}

void RoadRenderer::render(const RoadNetwork& network, GameTime now, const World& world)
{
	Profiler::EnableAssetCreationWarning(false);

	// 地形が変更されていたら道路・車線区画線メッシュキャッシュを破棄して再構築する
	for (const Chunk* chunk : world.getActiveChunks())
	{
		if (chunk && chunk->dirty)
		{
			m_meshCache.clear();
			m_laneCache.clear();
			break;
		}
	}

	for (const RoadEdge& edge : network.edges())
	{
		if (edge.id == -1)
			continue;

		auto bez = network.getBezier(edge.id);
		if (!bez)
			continue;

		drawEdge(edge, *bez, world);
	}

	(void)now;  // 将来: 夜間照明・工事中点滅などに使用予定
}

void RoadRenderer::drawEdge(const RoadEdge& edge, const CubicBezier& bezier, const World& world)
{
	const RoadStyle& style = m_styleRegistry.get(edge.roadType);

	// ---- 路面メッシュ ----
	if (!m_meshCache.contains(edge.id))
	{
		const MeshData meshData = buildRoadMesh(edge, bezier, style, world);
		if (meshData.vertices.isEmpty())
			return;
		m_meshCache.emplace(edge.id, Mesh{ meshData });
	}

	if (style.surface.surfaceTexture)
		m_meshCache[edge.id].draw(*style.surface.surfaceTexture,
		                          style.surface.surfaceColor.removeSRGBCurve());
	else
		m_meshCache[edge.id].draw(style.surface.surfaceColor.removeSRGBCurve());

	// ---- 車線区画線 ----
	if (!m_laneCache.contains(edge.id))
		m_laneCache[edge.id] = buildLaneLineBatches(edge, bezier, style, world);

	for (const auto& b : m_laneCache[edge.id])
		b.mesh.draw(b.color);
}

void RoadRenderer::markDirty(int edgeId)
{
	m_meshCache.erase(edgeId);
	m_laneCache.erase(edgeId);
}

// ─────────────────────────────────────────────────────────────────────────────
// メッシュ生成
// ─────────────────────────────────────────────────────────────────────────────

MeshData RoadRenderer::buildRoadMesh(const RoadEdge& edge, const CubicBezier& bezier,
                                     const RoadStyle& style, const World& world) const
{
	if (bezier.totalLength <= 0.0f)
		return MeshData{};

	constexpr int N        = 20;
	// 路肩を両端に加えた全体幅
	const float   halfWidth = edge.totalWidth() / 2.0f + style.shoulderWidth;
	const float   totalLen  = bezier.totalLength;

	Array<Vertex3D> vertices;
	vertices.reserve((N + 1) * 2);

	for (int i = 0; i <= N; ++i)
	{
		const float s  = (i / static_cast<float>(N)) * totalLen;
		const auto  sl = makeSlice(bezier, world, s, 2.0f);

		// v は弧長ベースでタイリング（表面タイルが道路長に応じて繰り返す）
		const float v = s * style.surface.surfaceTileV;

		vertices << makeVert(sl.center - sl.right * static_cast<double>(halfWidth), 0.0f, v);
		vertices << makeVert(sl.center + sl.right * static_cast<double>(halfWidth), 1.0f, v);
	}

	// Quad 帯: セグメントごとに 2 三角形
	Array<TriangleIndex32> indices;
	indices.reserve(N * 2);
	for (int i = 0; i < N; ++i)
	{
		const uint32 iL0 = static_cast<uint32>(i * 2    );
		const uint32 iR0 = static_cast<uint32>(i * 2 + 1);
		const uint32 iL1 = static_cast<uint32>(i * 2 + 2);
		const uint32 iR1 = static_cast<uint32>(i * 2 + 3);
		appendQuad(indices, iL0, iR0, iL1, iR1);
	}

	return MeshData{ vertices, indices };
}

Array<RoadRenderer::LaneLineBatch> RoadRenderer::buildLaneLineBatches(
	const RoadEdge& edge, const CubicBezier& bezier,
	const RoadStyle& style, const World& world) const
{
	// 車線が 2 本以上ないと区画線を引く境界が存在しない
	if (bezier.totalLength <= 0.0f || edge.lanes.size() < 2)
		return {};

	// 対向車線境界（センターライン）の境界インデックスを探す
	// lanes は物理的左端 (index 0) から右端へ並ぶ
	int centerBoundaryIdx = -1;
	for (int i = 0; i + 1 < static_cast<int>(edge.lanes.size()); ++i)
	{
		if (edge.lanes[i].dir != edge.lanes[i + 1].dir)
		{
			centerBoundaryIdx = i;
			break;
		}
	}

	const float halfTotal = edge.totalWidth() / 2.0f;

	// 各境界の区画線データを種別ごとに集積する
	MeshData markingData;  // 同方向区画線（白破線など）
	MeshData centerData;   // センターライン（黄実線など）

	float cumFromLeft = 0.0f;
	for (int i = 0; i + 1 < static_cast<int>(edge.lanes.size()); ++i)
	{
		cumFromLeft += edge.lanes[i].width;
		const float offsetFromCenter = cumFromLeft - halfTotal;

		const bool isCenter = (i == centerBoundaryIdx);
		const LineMarkStyle& ms = isCenter ? style.centerLine : style.laneMarking;
		MeshData& target        = isCenter ? centerData : markingData;

		appendDashedStrip(
			target.vertices, target.indices,
			bezier, world,
			offsetFromCenter, ms.lineWidth * 0.5f,
			ms.dashLength, ms.gapLength);
	}

	Array<LaneLineBatch> batches;
	if (!markingData.vertices.isEmpty())
		batches << LaneLineBatch{ style.laneMarking.color.removeSRGBCurve(), Mesh{ markingData } };
	if (!centerData.vertices.isEmpty())
		batches << LaneLineBatch{ style.centerLine.color.removeSRGBCurve(), Mesh{ centerData } };

	return batches;
}
