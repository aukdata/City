#include "RoadRenderer.hpp"

void RoadRenderer::render(const RoadNetwork& network, GameTime now)
{
	for (const RoadEdge& edge : network.edges())
	{
		if (edge.id == -1)
			continue;

		auto bez = network.getBezier(edge.id);
		if (!bez)
			continue;

		drawEdge(edge, *bez, now);
	}
}

void RoadRenderer::drawEdge(const RoadEdge& edge, const CubicBezier& bezier, GameTime now)
{
	// キャッシュになければ生成して登録する
	if (!m_meshCache.contains(edge.id))
	{
		const MeshData meshData = buildRoadMesh(edge, bezier);
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

MeshData RoadRenderer::buildRoadMesh(const RoadEdge& edge, const CubicBezier& bezier) const
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
		const Vec3 center = bezier.positionAt(s) + Vec3{ 0.0, 0.05, 0.0 };
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
		return ColorF{ 0.35, 0.35, 0.35 };
	case RoadType::Arterial:
		return ColorF{ 0.30, 0.30, 0.30 };
	case RoadType::Expressway:
	case RoadType::Highway:
		return ColorF{ 0.25, 0.25, 0.25 };
	}
	return ColorF{ 0.35, 0.35, 0.35 };
}
