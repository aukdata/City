#include "RoadRenderer.hpp"

void RoadRenderer::render(const RoadNetwork& network, GameTime now, const World& world)
{
	Profiler::EnableAssetCreationWarning(false);

	// 地形が変更されていたら道路メッシュキャッシュを破棄して再構築する
	for (const Chunk* chunk : world.getActiveChunks())
	{
		if (chunk && chunk->dirty)
		{
			m_meshCache.clear();
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

		drawEdge(edge, *bez, now, world);
	}
}

void RoadRenderer::drawEdge(const RoadEdge& edge, const CubicBezier& bezier, GameTime now, const World& world)
{
	// キャッシュになければ生成して登録する
	if (!m_meshCache.contains(edge.id))
	{
		const MeshData meshData = buildRoadMesh(edge, bezier, world);
		if (meshData.vertices.isEmpty())
			return;
		m_meshCache.emplace(edge.id, Mesh{ meshData });
	}

	m_meshCache[edge.id].draw(roadSurfaceColor(edge.roadType));
	drawLaneLines(edge, bezier, now);
}

void RoadRenderer::markDirty(int edgeId)
{
	m_meshCache.erase(edgeId);
}

MeshData RoadRenderer::buildRoadMesh(const RoadEdge& edge, const CubicBezier& bezier, const World& world) const
{
	if (bezier.totalLength <= 0.0f)
		return MeshData{};

	constexpr int N = 20;
	const float halfWidth = edge.totalWidth() / 2.0f;

	Array<Vertex3D> vertices;
	vertices.reserve((N + 1) * 2);

	for (int i = 0; i <= N; ++i)
	{
		const float s = (i / static_cast<float>(N)) * bezier.totalLength;
		const Vec3 p = bezier.positionAt(s);
		const float terrainY = world.sampleHeight(static_cast<float>(p.x), static_cast<float>(p.z));
		// カメラ距離 600m 時のデプス精度不足による Z-fighting を防ぐため 2m 浮かせる
		const Vec3 center{ p.x, terrainY + 2.0, p.z };
		const Vec3 tan    = bezier.tangentAt(s);

		// 水平方向の右ベクトル（tan の xz 平面での直角）
		const float lenXZ = static_cast<float>(Math::Sqrt(tan.x * tan.x + tan.z * tan.z));

		Vec3 right;
		if (lenXZ > 1e-6f)
		{
			right = Vec3{ tan.z / lenXZ, 0.0, -tan.x / lenXZ };
		}
		else
		{
			right = Vec3{ 1.0, 0.0, 0.0 };
		}

		const Vec3 leftPos  = center - right * halfWidth;
		const Vec3 rightPos = center + right * halfWidth;

		const float v = i / static_cast<float>(N);

		Vertex3D vL;
		vL.pos    = Float3{ static_cast<float>(leftPos.x),  static_cast<float>(leftPos.y),  static_cast<float>(leftPos.z) };
		vL.normal = Float3{ 0.0f, 1.0f, 0.0f };
		vL.tex    = Float2{ 0.0f, v };

		Vertex3D vR;
		vR.pos    = Float3{ static_cast<float>(rightPos.x), static_cast<float>(rightPos.y), static_cast<float>(rightPos.z) };
		vR.normal = Float3{ 0.0f, 1.0f, 0.0f };
		vR.tex    = Float2{ 1.0f, v };

		vertices << vL;
		vertices << vR;
	}

	// Quad 帯: セグメントごとに2三角形
	// 頂点配置: i番目のセグメントの左=2i, 右=2i+1
	Array<TriangleIndex32> indices;
	indices.reserve(N * 2);

	for (int i = 0; i < N; ++i)
	{
		const uint32 iL0 = static_cast<uint32>(i * 2    );  // 左  (現在)
		const uint32 iR0 = static_cast<uint32>(i * 2 + 1);  // 右  (現在)
		const uint32 iL1 = static_cast<uint32>(i * 2 + 2);  // 左  (次)
		const uint32 iR1 = static_cast<uint32>(i * 2 + 3);  // 右  (次)

		// 反時計回り（表面 = カメラ上方から可視）
		indices << TriangleIndex32{ iL0, iL1, iR0 };
		indices << TriangleIndex32{ iR0, iL1, iR1 };
	}

	return MeshData{ vertices, indices };
}

void RoadRenderer::drawLaneLines(const RoadEdge& /*edge*/, const CubicBezier& /*bezier*/, GameTime /*now*/) const
{
	// Phase 1: 未実装
}

ColorF RoadRenderer::roadSurfaceColor(RoadType rt)
{
	switch (rt)
	{
	case RoadType::LocalRoad:
		return ColorF{ 0.35, 0.35, 0.35 }.removeSRGBCurve();
	case RoadType::Arterial:
		return ColorF{ 0.30, 0.30, 0.30 }.removeSRGBCurve();
	case RoadType::Expressway:
	case RoadType::Highway:
		return ColorF{ 0.25, 0.25, 0.25 }.removeSRGBCurve();
	}
	return ColorF{ 0.35, 0.35, 0.35 }.removeSRGBCurve();
}
