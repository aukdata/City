#include "TrainRenderer.hpp"
#include <Siv3D/Profiler.hpp>

namespace
{
	constexpr float kTrackWidth    = 1.435f;   // 軌間 [m]（標準軌）
	constexpr float kRailHalfW     = 0.07f;    // レール断面の半幅 [m]
	constexpr float kRailTopY      = 0.17f;    // レール天面の高さ [m]（地盤からの offset）
	constexpr float kSleeperY      = 0.10f;    // 枕木天面の高さ [m]
	constexpr float kSleeperHalfL  = kTrackWidth * 0.5f + 0.2f;  // 枕木の半長
	constexpr float kSleeperHalfW  = 0.065f;  // 枕木の半幅（進行方向）
	constexpr int   kSegments      = 50;

	/// 4点から四角形（2三角形）の頂点・インデックスを MeshData に追加する
	void addQuad(MeshData& dst,
	             const Float3& v0, const Float3& v1,
	             const Float3& v2, const Float3& v3,
	             const Float3& normal)
	{
		const uint32 base = static_cast<uint32>(dst.vertices.size());
		for (const auto& p : { v0, v1, v2, v3 })
		{
			Vertex3D v;
			v.pos    = p;
			v.normal = normal;
			v.tex    = Float2{ 0, 0 };
			dst.vertices << v;
		}
		dst.indices << TriangleIndex32{ base,     base + 1, base + 2 };
		dst.indices << TriangleIndex32{ base + 2, base + 1, base + 3 };
	}
}

// ─────────────────────────────────────────────────────────────────────────────

Mesh TrainRenderer::buildTrackMesh(const TrackEdge& edge, const CubicBezier& bez)
{
	MeshData mesh;
	mesh.vertices.reserve((kSegments + 1) * 4 + (static_cast<int>(edge.length / 5.0f) + 2) * 4);

	// ---- レール（左右 2本のリボン） ----
	for (int side = 0; side < 2; ++side)
	{
		const float sideSign = (side == 0) ? -1.0f : 1.0f;

		Array<Float3> edgeL, edgeR;
		edgeL.reserve(kSegments + 1);
		edgeR.reserve(kSegments + 1);

		for (int i = 0; i <= kSegments; ++i)
		{
			const float t      = static_cast<float>(i) / kSegments;
			const Vec3  pos    = bez.evaluate(t);
			const Vec3  tan    = bez.tangent(t).normalized();
			const Vec3  right  = Vec3{ tan.z, 0, -tan.x };

			// レール中心（左右どちらか）
			const Vec3  center = pos + right * (sideSign * kTrackWidth * 0.5f)
			                         + Vec3{ 0, kRailTopY, 0 };

			edgeL << Float3{
				static_cast<float>((center - right * kRailHalfW).x),
				static_cast<float>((center - right * kRailHalfW).y),
				static_cast<float>((center - right * kRailHalfW).z) };
			edgeR << Float3{
				static_cast<float>((center + right * kRailHalfW).x),
				static_cast<float>((center + right * kRailHalfW).y),
				static_cast<float>((center + right * kRailHalfW).z) };
		}

		const Float3 up{ 0, 1, 0 };
		for (int i = 0; i < kSegments; ++i)
		{
			// 上面（CW = Siv3D 表面）: edgeL[i], edgeL[i+1], edgeR[i], edgeR[i+1]
			addQuad(mesh, edgeL[i], edgeL[i + 1], edgeR[i], edgeR[i + 1], up);
		}
	}

	// ---- 枕木（5m ごと） ----
	const int numSleepers = Max(1, static_cast<int>(edge.length / 5.0f));

	for (int si = 0; si <= numSleepers; ++si)
	{
		const float t      = static_cast<float>(si) / numSleepers;
		const Vec3  pos    = bez.evaluate(t);
		const Vec3  tan    = bez.tangent(t).normalized();
		const Vec3  right  = Vec3{ tan.z, 0, -tan.x };

		// 枕木の四隅（進行方向 = tan、幅方向 = right）
		const auto toF3 = [](const Vec3& v) -> Float3 {
			return Float3{
				static_cast<float>(v.x),
				static_cast<float>(v.y),
				static_cast<float>(v.z) };
		};

		const Vec3 base = pos + Vec3{ 0, kSleeperY, 0 };
		const Float3 v0 = toF3(base - right * kSleeperHalfL + tan * kSleeperHalfW);
		const Float3 v1 = toF3(base + right * kSleeperHalfL + tan * kSleeperHalfW);
		const Float3 v2 = toF3(base - right * kSleeperHalfL - tan * kSleeperHalfW);
		const Float3 v3 = toF3(base + right * kSleeperHalfL - tan * kSleeperHalfW);

		addQuad(mesh, v0, v1, v2, v3, Float3{ 0, 1, 0 });
	}

	return Mesh{ mesh };
}

// ─────────────────────────────────────────────────────────────────────────────

void TrainRenderer::renderTracks(const TrainNetwork& network)
{
	Profiler::EnableAssetCreationWarning(false);

	const ColorF railColor = ColorF{ 0.55, 0.55, 0.60 }.removeSRGBCurve();

	for (const auto& edge : network.edges())
	{
		if (!edge.isValid()) continue;
		const auto bez = network.getBezier(edge.id);
		if (!bez) continue;

		// キャッシュになければ構築する
		if (!m_trackMeshCache.contains(edge.id))
		{
			m_trackMeshCache.emplace(edge.id, buildTrackMesh(edge, *bez));
		}

		m_trackMeshCache[edge.id].draw(railColor);
	}

	// 駅マーカー（キャッシュ対象外・低頻度）
	for (const auto& node : network.nodes())
	{
		if (node.type == TrackNodeType::Station && !node.name.isEmpty())
		{
			Cylinder{ node.position, node.position + Vec3{ 0, 8, 0 }, 3.0 }
				.draw(ColorF{ 0.9, 0.85, 0.2 }.removeSRGBCurve());
		}
	}
}

void TrainRenderer::renderTrains(const Array<Train>& trains)
{
	for (const auto& train : trains)
	{
		if (train.currentEdge < 0) continue;

		const ColorF bodyColor = ((train.type == TrainType::Shinkansen)
			? ColorF{ 0.9, 0.95, 1.0 }
			: (train.type == TrainType::Express || train.type == TrainType::LimitedExpress)
			? ColorF{ 0.2, 0.4, 0.8 }
			: ColorF{ 0.3, 0.6, 0.35 }).removeSRGBCurve();

		const Quaternion rot = Quaternion::RotationAxis(Float3{ 0, 1, 0 }, -train.heading);

		Box{ train.position + Vec3{ 0, 1.75, 0 }, 3.0, 3.5, 20.0 }
			.draw(rot, bodyColor);

		Box{ train.position + Vec3{ 0, 2.5, 0 }, 3.1, 0.3, 0.4 }
			.draw(rot, ColorF{ 0.1, 0.1, 0.1 }.removeSRGBCurve());
	}
}

void TrainRenderer::markDirty(int edgeId)
{
	m_trackMeshCache.erase(edgeId);
}
