#include "RoadRenderer.hpp"
#include "RoadRenderDetail.hpp"
#include "../asset/ModelLodPath.hpp"
#include "BridgeStructure.hpp"
#include "../debug/DebugLog.hpp"
#include "../road/JunctionGeometry.hpp"
#include "MeshGeometry.hpp"

using namespace RoadRenderDetail;

/// @file
/// @brief 道路描画とキャッシュの利用。

namespace
{
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

}

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
		m_pendingFallbackEdges.erase(edge.id);
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

	if(m_asyncDetails && (isClose || prepareDetail)) { startDetailJob(edge,network,world,marginA,marginB); }
	// ---- 道路沿い設備（近距離のみ） ----
	if (isClose || prepareDetail)
	{
		if (!m_asyncDetails && !m_streetFurnitureCache.contains(edge.id) && canBuildCache())
		{
			const auto bez = network.getBezier(edge.id);
			if (!bez) return;
			const Stopwatch cacheTimer{StartImmediately::Yes};
			m_streetFurnitureCache[edge.id] = buildStreetFurnitureBatches(edge, *bez, world, marginA, marginB);
			if(cacheTimer.msF()>20) { DBG_LOG(U"[RoadCacheCost] edge={} length={} stage=furniture ms={:.2f}"_fmt(edge.id,edge.length,cacheTimer.msF())); }
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
		if(!m_asyncDetails && !m_signCache.contains(edge.id) && canBuildCache())
		{
			const Stopwatch timer{StartImmediately::Yes};m_signCache[edge.id]=buildEdgeSignMeshes(network,edge.id,world);
			if(timer.msF()>20) { DBG_LOG(U"[RoadCacheCost] edge={} stage=sign ms={:.2f}"_fmt(edge.id,timer.msF())); }m_cacheBuildStats.furnitureMs+=timer.msF();
		}
		if(!m_asyncDetails && !m_guideSignCache.contains(edge.id) && canBuildCache())
		{
			const Stopwatch timer{StartImmediately::Yes};m_guideSignCache[edge.id]=buildEdgeGuideSignDraws(network,edge.id,world);
			if(timer.msF()>20) { DBG_LOG(U"[RoadCacheCost] edge={} stage=guideSign ms={:.2f}"_fmt(edge.id,timer.msF())); }m_cacheBuildStats.furnitureMs+=timer.msF();
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
		m_pendingFallbackNodes.erase(nodeId);
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
	if (entry.materialType == RoadPartType::Roadbed && entry.asphalt && m_asphaltPS)
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
	if (!part.asphalt()) { color = color.removeSRGBCurve(); }
	return { color, heightOff, tex };
}

// ─────────────────────────────────────────────────────────────────────────────
// メッシュ生成
// ─────────────────────────────────────────────────────────────────────────────

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
	synchronizeSpatialIndex(network);
	Array<int> edgeCandidates, nodeCandidates;
	if (m_spatialQuery)
	{
		edgeCandidates = querySpatial(cameraPos, frustum, false); nodeCandidates = querySpatial(cameraPos, frustum, true);
	}
	else
	{
		for (const auto& edge : network.edges()) { if (edge.id >= 0) { edgeCandidates << edge.id; } }
		for (const auto& node : network.nodes()) { if (node.id >= 0) { nodeCandidates << node.id; } }
	}
	const float camX = static_cast<float>(cameraPos.x);
	const float camZ = static_cast<float>(cameraPos.z);
	constexpr float kDrawMaxDistSqF = static_cast<float>(kDrawMaxDistSq);

	const ColorF wirePlanned        = ColorF{ 1.0, 1.0, 1.0, 1.0 };
	const ColorF wireUnderConstruct = ColorF{ 1.0, 0.8, 0.2, 1.0 };

	for (int id : edgeCandidates)
	{
		const auto* source = network.getEdge(id); if (!source) { continue; } const RoadEdge& edge = *source;
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
	for (int id : nodeCandidates)
	{
		const auto* source = network.getNode(id); if (!source) { continue; } const RoadNode& node = *source;
		if (node.id < 0) continue;
		const float ndx = static_cast<float>(node.position.x) - camX;
		const float ndz = static_cast<float>(node.position.z) - camZ;
		if (ndx * ndx + ndz * ndz > kDrawMaxDistSqF) continue;
		if (!frustum.intersects(Sphere{ node.position, 30.0 })) continue;
		drawNodeCapWireframe(network, node.id, world);
	}
}

/// @brief A continuous, inexpensive surface remains visible while exact roadside parts are built.
RoadRenderer::FallbackEdgeData RoadRenderer::buildFallbackEdgeData(const RoadEdge& edge, const RoadNetwork& network, const World& world) const
{
	FallbackEdgeData result;
	result.nodeA = edge.nodeA; result.nodeB = edge.nodeB;
	if (const auto bezier = network.getBezier(edge.id))
	{
		const Vec3 middle = bezier->positionAt(bezier->totalLength * .5f);
		result.chunk = chunkCoordToKey({static_cast<int>(Floor(middle.x / CHUNK_SIZE)), static_cast<int>(Floor(middle.z / CHUNK_SIZE))});
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
				result.parts << FallbackPartData{std::move(data), color, texture, part.type, part.asphalt()};
			}
		}
	}
	return result;
}

void RoadRenderer::commitFallbackEdge(int edgeId, FallbackEdgeData&& data, bool createMeshes)
{
	Array<PartMeshEntry> entries;
	Array<MeshData> pending;
	OverviewSource overview{};
	overview.nodeA = data.nodeA; overview.nodeB = data.nodeB; overview.chunk = data.chunk;
	for (auto& part : data.parts)
	{
		const Mesh mesh = createMeshes ? Mesh{part.data} : Mesh{};
		if (!createMeshes) { pending << part.data; }
		entries << PartMeshEntry{{mesh, mesh}, part.color, part.texture, part.type, part.asphalt};
		overview.parts << OverviewPart{std::move(part.data), entries.back()};
	}
	if (!pending.isEmpty()) { m_pendingFallbackEdges[edgeId] = std::move(pending); }
	m_overviewDirty.insert(overview.chunk);
	m_overviewSources[static_cast<int64>(edgeId) * 2] = std::move(overview);
	m_overviewOwners[m_overviewSources.at(static_cast<int64>(edgeId)*2).chunk].insert(static_cast<int64>(edgeId)*2);
	m_fallbackEdges.emplace(edgeId, std::move(entries));
	++m_geometryRevision;
}

void RoadRenderer::prepareFallbackEdge(const RoadEdge& edge, const RoadNetwork& network, const World& world)
{
	if (!m_fallbackEdges.contains(edge.id))
	{
		const Stopwatch timer{StartImmediately::Yes};
		commitFallbackEdge(edge.id, buildFallbackEdgeData(edge, network, world));
		m_cacheBuildStats.fallbackMs += timer.msF();
	}
}

void RoadRenderer::materializeFallbackEdge(int edgeId)
{
	const auto pending = m_pendingFallbackEdges.find(edgeId);
	if (pending == m_pendingFallbackEdges.end()) { return; }
	if (const auto entries = m_fallbackEdges.find(edgeId); entries != m_fallbackEdges.end())
	{
		for (size_t i = 0; i < entries->second.size() && i < pending->second.size(); ++i)
		{
			const Mesh mesh{pending->second[i]};
			entries->second[i].meshPair = {mesh, mesh};
		}
	}
	m_pendingFallbackEdges.erase(pending);
}

void RoadRenderer::materializeFallbackNode(int nodeId)
{
	const auto pending = m_pendingFallbackNodes.find(nodeId);
	if (pending == m_pendingFallbackNodes.end()) { return; }
	if (const auto entry = m_fallbackNodes.find(nodeId); entry != m_fallbackNodes.end())
	{
		const Mesh mesh{pending->second};
		entry->second.meshPair = {mesh, mesh};
	}
	m_pendingFallbackNodes.erase(pending);
}

void RoadRenderer::drawFallbackEdge(const RoadEdge& edge,const RoadNetwork& network,const World& world)
{
	prepareFallbackEdge(edge,network,world);
	materializeFallbackEdge(edge.id);
	m_drawnFallbackEdges.emplace(edge.id);
	for (const auto& entry : m_fallbackEdges.at(edge.id)) { drawSurface(entry, entry.meshPair.detail); }
}

RoadRenderer::FallbackNodeData RoadRenderer::buildFallbackNodeData(int nodeId, const RoadNetwork& network, const World& world) const
{
	FallbackNodeData result;
	const auto layout = JunctionGeometry::build(network, nodeId, true, &world);
	result.data = JunctionGeometry::terrainFootprint(layout);
	for (auto& vertex : result.data.vertices)
	{
		if (!layout.elevated) { vertex.pos.y = static_cast<float>(world.sampleHeight(vertex.pos.x, vertex.pos.z) + kRoadSurfaceLift); }
		vertex.tex = Float2{vertex.pos.x * kWorldUV, vertex.pos.z * kWorldUV};
	}
	orientRoadFaces(result.data);
	if (const auto* node = network.getNode(nodeId))
	{
		result.valid = true;
		result.chunk = chunkCoordToKey({static_cast<int>(Floor(node->position.x / CHUNK_SIZE)), static_cast<int>(Floor(node->position.z / CHUNK_SIZE))});
	}
	return result;
}

void RoadRenderer::commitFallbackNode(int nodeId, FallbackNodeData&& data, bool createMeshes)
{
	const auto& definition = m_partRegistry.get(U"roadbed_asphalt");
	const Mesh mesh = data.data.indices.isEmpty() || !createMeshes ? Mesh{} : Mesh{data.data};
	if (!createMeshes && !data.data.indices.isEmpty()) { m_pendingFallbackNodes[nodeId] = data.data; }
	m_fallbackNodes.emplace(nodeId, PartMeshEntry{{mesh, mesh}, ColorF{.29, .30, .32},
		definition.texture ? &*definition.texture : nullptr, RoadPartType::Roadbed});
	if (data.valid)
	{
		m_overviewDirty.insert(data.chunk);
		m_overviewSources[static_cast<int64>(nodeId) * 2 + 1] = OverviewSource{data.chunk, {{std::move(data.data), m_fallbackNodes.at(nodeId)}}};
		m_overviewOwners[data.chunk].insert(static_cast<int64>(nodeId)*2+1);
	}
}

void RoadRenderer::prepareFallbackNode(int nodeId, const RoadNetwork& network, const World& world)
{
	if (!m_fallbackNodes.contains(nodeId))
	{
		const Stopwatch timer{StartImmediately::Yes};
		commitFallbackNode(nodeId, buildFallbackNodeData(nodeId, network, world));
		m_cacheBuildStats.fallbackMs += timer.msF();
	}
}

void RoadRenderer::drawFallbackNode(int nodeId,const RoadNetwork& network,const World& world)
{
	prepareFallbackNode(nodeId,network,world);
	materializeFallbackNode(nodeId);
	const auto& entry = m_fallbackNodes.at(nodeId);
	if (entry.meshPair.detail) { drawSurface(entry, entry.meshPair.detail); }
}

void RoadRenderer::rebuildOverview(int64 chunk)
{
	Array<OverviewPart> groups;
	for (int64 owner : m_overviewOwners[chunk])
	{
		const auto found=m_overviewSources.find(owner);if(found==m_overviewSources.end()) { continue; }
		const auto& source=found->second;
		for (const auto& part : source.parts)
		{
			auto group = std::find_if(groups.begin(), groups.end(), [&](const OverviewPart& candidate)
			{
				return candidate.visual.materialType == part.visual.materialType && candidate.visual.asphalt == part.visual.asphalt
					&& candidate.visual.texture == part.visual.texture && candidate.visual.color == part.visual.color;
			});
			if (group == groups.end()) { groups << OverviewPart{{}, part.visual}; group = groups.end() - 1; }
			MeshGeometry::appendMeshData(group->data, part.data);
		}
	}
	Array<PartMeshEntry> batches;
	for (const auto& group : groups)
	{
		if (group.data.indices.isEmpty()) { continue; }
		auto entry = group.visual; const Mesh mesh{group.data}; entry.meshPair = {mesh, mesh}; batches << std::move(entry);
	}
	m_overviewBatches[chunk] = std::move(batches);
	m_overviewDirty.erase(chunk);
}

void RoadRenderer::invalidateOverview(int64 owner)
{
	if (const auto source = m_overviewSources.find(owner); source != m_overviewSources.end())
	{
		m_overviewOwners[source->second.chunk].erase(owner);
		m_overviewDirty.insert(source->second.chunk);
		m_overviewBatches.erase(source->second.chunk);
		m_overviewSources.erase(source);
	}
}

void RoadRenderer::drawOverview(const ViewFrustum& frustum, Vec3 cameraPos)
{
	m_overviewDrawn.clear();
	m_overviewDrawCalls = 0;
	if (!m_overviewEnabled) { return; }
	if (!m_overviewDirty.empty()) { rebuildOverview(*m_overviewDirty.begin()); }
	for (const auto& [key, batches] : m_overviewBatches)
	{
		if (m_overviewDirty.contains(key)) { continue; }
		bool distant = !batches.isEmpty();
		for (const auto& entry : batches)
		{
			const auto sphere = entry.meshPair.detail.boundingSphere();
			if (cameraPos.distanceFrom(sphere.center) - sphere.r < 1600) { distant = false; break; }
		}
		if (!distant) { continue; }
		m_overviewDrawn.insert(key);
		for (const auto& entry : batches)
		{
			const auto sphere = entry.meshPair.detail.boundingSphere();
			if (cameraPos.distanceFrom(sphere.center) - sphere.r <= kDrawMaxDist && frustum.intersects(sphere))
			{
				drawSurface(entry, entry.meshPair.detail); ++m_overviewDrawCalls;
			}
		}
	}
}
