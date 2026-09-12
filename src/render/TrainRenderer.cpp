#include "TrainRenderer.hpp"
#include "BridgeStructure.hpp"
#include "RailStructure.hpp"
#include <Siv3D/Profiler.hpp>

namespace
{
	constexpr float kTrackWidth    = 1.067f;   // 軌間 [m]（狭軌）
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
	// 線路 1 本をレール 2 本と枕木列へ分解し、静的メッシュとしてまとめて構築する。
	MeshData mesh;
	mesh.vertices.reserve((kSegments + 1) * 4 + (static_cast<int>(edge.length / .65f) + 2) * 4);

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
			const Vec3  right  = tangentToRight(tan);

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

	// ---- 枕木（約0.65m ごと） ----
	const int numSleepers = Max(1, static_cast<int>(edge.length / .65f));

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

void TrainRenderer::renderTracks(const TrainNetwork& network,const World& world,Vec3 eye,const RoadNetwork& roads)
{
	if (!m_roadClearance || m_roadEdgeCount!=roads.edges().size())
	{
		m_roadClearance=std::make_unique<ParcelRoadIndex>(roads,true); m_roadEdgeCount=roads.edges().size(); m_bedMeshCache.clear();
	}
	// 線路はエッジ単位でメッシュキャッシュし、未構築分だけ初回描画時に生成する。
	Profiler::EnableAssetCreationWarning(false);

	const ColorF railColor = ColorF{ 0.55, 0.55, 0.60 }.removeSRGBCurve();

	for (const auto& edge : network.edges())
	{
		if (!edge.isValid()) continue;
		const Vec3 center=(network.getNode(edge.nodeA)->position+network.getNode(edge.nodeB)->position)*.5;
		if (Vec2{center.x-eye.x,center.z-eye.z}.length()>5000+edge.length*.5) { continue; }
		const auto bez = network.getBezier(edge.id);
		if (!bez) continue;

		// キャッシュになければ構築する
		if (!m_trackMeshCache.contains(edge.id))
		{
			m_trackMeshCache.emplace(edge.id, buildTrackMesh(edge, *bez));
		}

		if (!m_bedMeshCache.contains(edge.id))
		{
			MeshData bed;
			for (float arc=0;arc<bez->totalLength;arc+=5)
			{
				const Vec3 a=bez->positionAt(arc),b=bez->positionAt(Min(arc+5,bez->totalLength));
				const Vec3 rightA=tangentToRight(bez->tangentAt(arc)),rightB=tangentToRight(bez->tangentAt(Min(arc+5,bez->totalLength)));
				const double ground=world.sampleHeight(static_cast<float>(a.x),static_cast<float>(a.z));
				const bool viaduct=a.y-ground>3;
				if (viaduct) { BridgeStructure::append(bed,RailStructure::deck(a,b,rightA,rightB)); }
				else
				{
					RailStructure::prism(bed,a,b,rightA,rightB,0,2.4,0,Min(-.5,ground-a.y-.25));
				}

				if (viaduct && static_cast<int>(arc)%30==0 && !m_roadClearance->overlaps(ParcelGeometry::footprint({a.x,a.z},3,0)))
				{
					BridgeStructure::append(bed,BridgeStructure::pier(a,rightA,ground,a.y-1.5,4.8));
				}
			}
			m_bedMeshCache.emplace(edge.id,Mesh{bed});
		}
		m_bedMeshCache[edge.id].draw(ColorF{.38,.39,.37}.removeSRGBCurve());
		m_trackMeshCache[edge.id].draw(railColor);
	}

	// 接続線路の接線にホームの長手方向を合わせる。
	for (const auto& node : network.nodes())
	{
		if (!node.isValid() || node.type != TrackNodeType::Station || node.position.distanceFromSq(eye)>Square(5000.0))
		{
			continue;
		}
		float heading = 0.0f;
		for (const int edgeId : node.edgeIds)
		{
			const auto* edge = network.getEdge(edgeId);
			const auto bez = network.getBezier(edgeId);
			if (!edge || !bez)
			{
				continue;
			}
			Vec3 tangent = bez->tangent(edge->nodeA == node.id ? 0.0f : 1.0f);
			if (edge->nodeB == node.id)
			{
				tangent = -tangent;
			}
			heading = static_cast<float>(Math::Atan2(tangent.x, tangent.z));
			break;
		}
		Model& model = ensureModel((node.id & 1) == 0 ? U"station_001" : U"station_002");
		const Transformer3D transform{ Mat4x4::RotateY(heading).translated(node.position) };
		for (const auto& object : model.objects())
		{
			object.draw(model.materials());
		}
	}
}

void TrainRenderer::renderTrains(const Array<Train>& trains)
{
	// 普通・急行は専用モデルを使用し、それ以外は従来の簡易表示を維持する。
	for (const auto& train : trains)
	{
		if (train.currentEdge < 0) continue;
		if (train.type == TrainType::Local || train.type == TrainType::Express)
		{
			Model& model = ensureModel(train.type == TrainType::Local ? U"commuter_001" : U"commuter_002");
			if (!model.isEmpty())
			{
				const Transformer3D transform{ Mat4x4::RotateY(train.heading)
					.translated(train.position + Vec3{ 0, kRailTopY, 0 }) };
				for (const auto& object : model.objects())
				{
					object.draw(model.materials());
				}
				continue;
			}
		}

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

void TrainRenderer::invalidateTrackCache(int edgeId)
{
	m_trackMeshCache.erase(edgeId);
	m_bedMeshCache.erase(edgeId);
}

Model& TrainRenderer::ensureModel(const String& stem)
{
	auto [it, inserted] = m_models.try_emplace(stem);
	if (inserted)
	{
		it->second = Model{ U"assets/railway/{}.obj"_fmt(stem) };
		Model::RegisterDiffuseTextures(it->second, TextureDesc::MippedSRGB);
	}
	return it->second;
}
