#include "TrainRenderer.hpp"
#include "../railway/TrainConsist.hpp"
#include "BridgeStructure.hpp"
#include "RailStructure.hpp"
#include <Siv3D/Profiler.hpp>

namespace
{
	constexpr float kTrackWidth    = 1.067f;   // 軌間 [m]（狭軌）
	constexpr float kRailHalfW     = 0.07f;    // レール断面の半幅 [m]
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

Mesh TrainRenderer::buildTrackMesh(const TrackEdge& edge, const CubicBezier& bez, bool distant)
{
	// 線路 1 本をレール 2 本と枕木列へ分解し、静的メッシュとしてまとめて構築する。
	MeshData mesh;
	const int segments = distant ? Max(4, kSegments / 5) : kSegments;
	mesh.vertices.reserve((segments + 1) * 4 + (static_cast<int>(edge.length / .65f) + 2) * 4);

	for (const auto& lane : edge.lanes)
	{
		if (lane.type != LaneType::Rail) { continue; }
		// ---- レール（左右 2本のリボン） ----
		for (int side = 0; side < 2; ++side)
		{
			const float sideSign = (side == 0) ? -1.0f : 1.0f;

			Array<Float3> edgeL, edgeR;
			edgeL.reserve(segments + 1);
			edgeR.reserve(segments + 1);

			for (int i = 0; i <= segments; ++i)
			{
				const float fraction = static_cast<float>(i) / segments;
				const float t = bez.tFromArcLength(bez.totalLength * fraction);
				const Vec3  pos    = bez.evaluate(t) + tangentToRight(bez.tangent(t)) * lane.centerAt(fraction);
				const Vec3  tan    = bez.tangent(t).normalized();
				const Vec3  right  = tangentToRight(tan);

				// レール中心（左右どちらか）
				const Vec3  center = pos + right * (sideSign * kTrackWidth * 0.5f)
				                         + Vec3{ 0, TransportCrossSection::railTop(edge), 0 };

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
			for (int i = 0; i < segments; ++i)
			{
				// 上面（CW = Siv3D 表面）: edgeL[i], edgeL[i+1], edgeR[i], edgeR[i+1]
				addQuad(mesh, edgeL[i], edgeL[i + 1], edgeR[i], edgeR[i + 1], up);
			}
		}

		// ---- 枕木（約0.65m ごと） ----
		const int numSleepers = Max(1, static_cast<int>(edge.length / .65f));

		for (int si = 0; !distant && !edge.hasRoadLanes() && si <= numSleepers; ++si)
		{
			const float fraction = static_cast<float>(si) / numSleepers;
			const float t = bez.tFromArcLength(bez.totalLength * fraction);
			const Vec3  pos    = bez.evaluate(t) + tangentToRight(bez.tangent(t)) * lane.centerAt(fraction);
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

	}

	return Mesh{ mesh };
}

// ─────────────────────────────────────────────────────────────────────────────

void TrainRenderer::renderTracks(const TrainNetwork& network,const World& world,Vec3 eye,[[maybe_unused]] const RoadNetwork& roads)
{
	// 線路はエッジ単位でメッシュキャッシュし、未構築分だけ初回描画時に生成する。
	Profiler::EnableAssetCreationWarning(false);

	const ColorF railColor = ColorF{ 0.55, 0.55, 0.60 }.removeSRGBCurve();

	for (const auto& edge : network.edges())
	{
		if (!edge.isValid() || !edge.isRoadbedBuilt() || (edge.edgeState != EdgeState::Open && edge.edgeState != EdgeState::Existing)) continue;
		const Vec3 center=(network.getNode(edge.nodeA)->position+network.getNode(edge.nodeB)->position)*.5;
		if (Vec2{center.x-eye.x,center.z-eye.z}.length()>5000+edge.length*.5) { continue; }
		const auto bez = network.getBezier(edge.id);
		if (!bez) continue;

		// キャッシュになければ構築する
		const bool distant = center.distanceFromSq(eye) > Square(650.0 + edge.length * .5);
		auto& trackCache = distant ? m_distantTracks : m_trackMeshCache;
		if (!trackCache.contains(edge.id)) { trackCache.emplace(edge.id, buildTrackMesh(edge, *bez, distant)); }

		trackCache[edge.id].draw(railColor);
	}

	if (m_facilityEdgeCount != network.edges().size())
	{
		m_stations.clear(); m_depots.clear(); m_facilityEdgeCount = network.edges().size();
		m_parkedTrains = RailFacilities::parkedTrains(network);
	}
	for (const auto& node : network.nodes())
	{
		if (node.type != TrackNodeType::Station || node.position.distanceFromSq(eye) > Square(2500.0)) { continue; }
		if (!m_stations.contains(node.id)) { m_stations.emplace(node.id,buildFacility(RailFacilities::station(network,world,node.id))); }
		drawFacility(m_stations[node.id], node.position.distanceFromSq(eye) > 500 * 500);
	}
	for (const auto& depot : network.depots())
	{
		const auto* node = network.getNode(depot.throatNodeId);
		if (!node || node->position.distanceFromSq(eye) > Square(2500.0)) { continue; }
		if (!m_depots.contains(depot.stationNodeId)) { m_depots.emplace(depot.stationNodeId,buildFacility(RailFacilities::depot(network,world,depot))); }
		drawFacility(m_depots[depot.stationNodeId], node->position.distanceFromSq(eye) > 500 * 500);
	}
	renderTrains(m_parkedTrains,network,eye);
}

void TrainRenderer::renderTrains(const Array<Train>& trains,const TrainNetwork& network, Optional<Vec3> eye)
{
	for (const auto& train:trains)
	{
		if (train.currentEdge<0) { continue; }
		if (eye && train.position.distanceFromSq(*eye) > 6000 * 6000 && train.state != TrainState::OutOfService) { continue; }
		const auto profile=TrainConsist::profile(train.type);
		for (int car=0;car<profile.cars;++car)
		{
			const auto pose=TrainConsist::carPose(train,network,car);
			if (!pose) { continue; }
			const double distanceSq=eye ? pose->position.distanceFromSq(*eye) : 0;
			if (distanceSq>6000*6000) { continue; }
			const int level=distanceSq<180*180 ? 0 : (distanceSq<700*700 ? 1 : 2);
			Model& model=ensureModel(pose->model,level);
			const Transformer3D scope{pose->transform};
			for (const auto& object:model.objects()) { object.draw(model.materials()); }
		}
	}
}

void TrainRenderer::invalidateTrackCache(int edgeId)
{
	m_trackMeshCache.erase(edgeId);
	m_distantTracks.erase(edgeId);
}

Model& TrainRenderer::ensureModel(const String& stem, int level)
{
	auto [it, inserted] = m_models.try_emplace(stem, U"assets/railway/{}.obj"_fmt(stem));
	return it->second.at(level);
}

TrainRenderer::FacilityDraw TrainRenderer::buildFacility(const RailFacilities::Geometry& geometry)
{
	FacilityDraw result;
	for (size_t material = 0; material < RailFacilities::Count; ++material)
	{
		if (!geometry.parts[material].indices.isEmpty())
		{
			result.parts[material] = Mesh{geometry.parts[material]};
			const auto distant = simplifyMesh(geometry.parts[material], 1.2f);
			if (!distant.indices.isEmpty()) { result.distant[material] = Mesh{distant}; }
		}
	}
	for (const auto& sign : geometry.signs)
	{
		const Vec3 along = sign.along * (sign.width*.5), up{0,.4,0};
		MeshData board;
		const Vec3 offset=sign.along.cross(Vec3{0,-1,0})*.012;
		const Vec3 front=sign.center+offset, back=sign.center-offset;
		BridgeStructure::quad(board,front-along+up,front+along+up,front+along-up,front-along-up);
		BridgeStructure::quad(board,back+along+up,back-along+up,back-along-up,back+along-up);
		const std::array<Float2,4> uv={Float2{0,0},Float2{1,0},Float2{1,1},Float2{0,1}};
		for (size_t vertex = 0; vertex < board.vertices.size(); ++vertex) { board.vertices[vertex].tex=uv[vertex%4]; }
		result.signs << std::pair<Mesh,String>{Mesh{board},sign.text};
	}
	return result;
}
void TrainRenderer::drawFacility(const FacilityDraw& draw, bool distant)
{
	for (size_t material = 0; material < RailFacilities::Count; ++material)
	{
		const auto& mesh = distant ? draw.distant[material] : draw.parts[material];
		if (!mesh.isEmpty()) { mesh.draw(RailFacilities::color(material)); }
	}
	if (distant) { return; }
	const ScopedRenderStates3D twoSided{RasterizerState::SolidCullNone};
	for (const auto& [board,name] : draw.signs)
	{
		if (const auto texture=m_facilityTextures.find(name); texture!=m_facilityTextures.end()) { board.draw(Mat4x4::Identity(),texture->second); }
		else { m_pendingFacilityNames.insert(name); board.draw(ColorF{.92}); }
	}
}
void TrainRenderer::prepareFacilityTextures()
{
	if (m_pendingFacilityNames.empty()) { return; }
	Profiler::EnableAssetCreationWarning(false);
	const Font font{FontMethod::MSDF,36,Typeface::Bold};
	for (const auto& name : m_pendingFacilityNames)
	{
		RenderTexture texture{512,128,ColorF{.94,.95,.90},TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::No,HasMipMap::Yes};
		{
			const ScopedRenderTarget2D target{texture};
			const ScopedRenderStates2D blend{BlendState::Opaque};
			Rect{0,98,512,20}.draw(ColorF{.15,.46,.38});
			const double fontSize=Min(40.0,36.0*484/Max(1.0,font(name).region().w));
			font(name).draw(fontSize,Arg::center=Vec2{256,52},ColorF{.08,.13,.15});
			Graphics2D::Flush();
		}
		texture.generateMips(); m_facilityTextures.emplace(name,std::move(texture));
	}
	m_pendingFacilityNames.clear();
}

void TrainRenderer::drawTrainSilhouette(const Train& train,const TrainNetwork& network,Vec3 eye,const ColorF& color)
{
	for (int car=0;car<TrainConsist::profile(train.type).cars;++car)
	{
		const auto pose=TrainConsist::carPose(train,network,car);if (!pose) { continue; }
		const double distance=pose->position.distanceFromSq(eye);
		const int level=distance<180*180 ? 0 : (distance<700*700 ? 1 : 2);
		Model& model=ensureModel(pose->model,level);
		const Transformer3D scope{pose->transform};
		for (const auto& object:model.objects()) { for (const auto& part:object.parts) { part.mesh.draw(color); } }
	}
}
