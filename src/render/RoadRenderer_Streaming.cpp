#include "RoadRenderer.hpp"
#include "RoadRenderDetail.hpp"
#include "../road/RoadSign.hpp"
#include "../debug/DebugLog.hpp"
#include <bit>

namespace
{
	uint64 terrainSignature(const Chunk& chunk)
	{
		uint64 value=1469598103934665603ULL;
		for(float height:chunk.heightMap) { value=(value^std::bit_cast<uint32>(height))*1099511628211ULL; }
		return value;
	}
}

void RoadRenderer::preloadFallbacks(const RoadNetwork& network,const World& world)
{
	const Stopwatch timer{StartImmediately::Yes};
	for(const auto& edge:network.edges()) { if(edge.id>=0 && edge.isRoadbedBuilt()) { prepareFallbackEdge(edge,network,world); m_boundsCache[edge.id]=edgeBounds(network,edge.id); } }
	for(const auto& node:network.nodes()) { if(node.id>=0 && !node.attachments.isEmpty()) { prepareFallbackNode(node.id,network,world); } }
	for(int z=0;z<WORLD_CHUNKS;++z) { for(int x=0;x<WORLD_CHUNKS;++x)
	{
		if(const auto* chunk=world.getChunk({x,z});chunk && !chunk->heightMap.isEmpty()) { m_preloadedHeights[chunkCoordToKey(chunk->coord)]=terrainSignature(*chunk); }
	} }
	DBG_LOG(U"[RoadFallback] preload edges={} nodes={} ms={:.2f}"_fmt(m_fallbackEdges.size(),m_fallbackNodes.size(),timer.msF()));
	HashSet<int64> overviewKeys;
	for (const auto& [owner, source] : m_overviewSources) { overviewKeys.insert(source.chunk); }
	for (int64 key : overviewKeys) { rebuildOverview(key); }
	DBG_LOG(U"[RoadOverview] batches={} sources={}"_fmt(m_overviewBatches.size(), m_overviewSources.size()));
	synchronizeSpatialIndex(network);
	m_cacheBuildStats={};
}

void RoadRenderer::render(const RoadNetwork& network, const World& world,
                          const ViewFrustum& frustum, Vec3 cameraPos)
{
	// 可視判定・地形起因のキャッシュ失効・エッジ/ノード描画を 1 フレーム内でまとめて回す。
	m_visibleEdges.clear();
	m_cacheBuildStats = {};
	uploadCompletedDetails();
	m_visibleRoads.clear();
	const Size viewport = Scene::Size();
	m_cableView->viewport = Float4{ static_cast<float>(viewport.x), static_cast<float>(viewport.y),
		1.0f / viewport.x, 1.0f / viewport.y };
	Profiler::EnableAssetCreationWarning(false);

	synchronizeTerrainChanges(world, network);
	synchronizeSpatialIndex(network);
	drawOverview(frustum, cameraPos);
	// ---- エッジ描画（端をノード半幅分カット）----
	const float camX = static_cast<float>(cameraPos.x);
	const float camZ = static_cast<float>(cameraPos.z);
	constexpr float kDrawMaxDistSqF = static_cast<float>(kDrawMaxDistSq);
	constexpr float kLodDistSqF     = static_cast<float>(kLodDistSq);


	Array<int> edgeCandidates,nodeCandidates;
	if(m_spatialQuery) { edgeCandidates=querySpatial(cameraPos,frustum,false);nodeCandidates=querySpatial(cameraPos,frustum,true); }
	else
	{
		for(const auto& edge:network.edges()) { if(edge.id>=0) { edgeCandidates << edge.id; } }
		for(const auto& node:network.nodes()) { if(node.id>=0) { nodeCandidates << node.id; } }
	}
	m_cacheBuildStats.candidateEdges=edgeCandidates.size();m_cacheBuildStats.candidateNodes=nodeCandidates.size();
	for (int id : edgeCandidates)
	{
		const auto* source=network.getEdge(id);if(!source) { continue; }const RoadEdge& edge=*source;
		if (edge.id == -1) continue;

		// バウンディング情報をキャッシュから取得（なければ計算してキャッシュ）
		auto boundsIt = m_boundsCache.find(edge.id);
		if (boundsIt == m_boundsCache.end())
		{
			boundsIt = m_boundsCache.emplace(edge.id, edgeBounds(network,edge.id)).first;
		}
		const auto& bounds = boundsIt->second;

		const double coarseDistance=Max(0.0,Vec3{bounds.center}.distanceFrom(cameraPos)-Sqrt(bounds.radiusSq));
		if (coarseDistance>kDrawMaxDist) { continue; }
		const auto overview = m_overviewSources.find(static_cast<int64>(edge.id) * 2);
		if (overview != m_overviewSources.end() && m_overviewDrawn.contains(overview->second.chunk))
		{
			if (frustum.intersects(Sphere{Vec3{bounds.center}, Sqrt(bounds.radiusSq)})) { m_visibleEdges.insert(edge.id); }
			continue;
		}
		double distSq=Math::Inf;
		for (size_t i=1;i<bounds.samples.size();++i)
		{
			const Vec3 delta=bounds.samples[i]-bounds.samples[i-1];
			const double t=Clamp((cameraPos-bounds.samples[i-1]).dot(delta)/Max(1e-9,delta.lengthSq()),0.0,1.0);
			distSq=Min(distSq,cameraPos.distanceFromSq(bounds.samples[i-1]+delta*t));
		}
		const bool isClose=distSq<kLodDistSq;

		// 視錐台カリング（距離チェックを通過した分のみ）
		const float radius = Math::Sqrt(bounds.radiusSq);
		if (!frustum.intersects(Sphere{ Vec3{ bounds.center }, static_cast<double>(radius) })) continue;

		m_visibleEdges.emplace(edge.id);

		m_visibleRoads << VisibleRoad{edge.id, static_cast<float>(distSq), false, isClose, distSq<kPrepareDist*kPrepareDist};
	}

	// ---- ノードキャップ描画（交差点フィル）----
	for (int id : nodeCandidates)
	{
		const auto* source=network.getNode(id);if(!source) { continue; }const RoadNode& node=*source;
		if (node.id < 0) continue;
		const auto overview = m_overviewSources.find(static_cast<int64>(node.id) * 2 + 1);
		if (overview != m_overviewSources.end() && m_overviewDrawn.contains(overview->second.chunk)) { continue; }

		// 距離チェック（float 演算のみ）
		const float nodeDx = static_cast<float>(node.position.x) - camX;
		const float nodeDz = static_cast<float>(node.position.z) - camZ;
		const float nodeDistSq = nodeDx * nodeDx + nodeDz * nodeDz;
		if (nodeDistSq > kDrawMaxDistSqF) continue;

		// 視錐台カリング
		if (!frustum.intersects(Sphere{ node.position, 30.0 })) continue;

		const bool isClose = nodeDistSq < kLodDistSqF;
		m_visibleRoads << VisibleRoad{node.id, nodeDistSq, true, isClose};
	}
	// Share one budget between edges and junctions, with the nearest geometry first.
	m_visibleRoads.sort_by([](const VisibleRoad& a, const VisibleRoad& b)
	{
		if (a.distanceSquared != b.distanceSquared) { return a.distanceSquared < b.distanceSquared; }
		if (a.node != b.node) { return a.node < b.node; }
		return a.id < b.id;
	});
	for (const auto& visible : m_visibleRoads)
	{
		if (visible.node) { drawNodeCap(network, visible.id, world, visible.close); }
		else if (const auto* edge = network.getEdge(visible.id))
		{
			drawEdge(*edge, network, edgeMargin(*edge, edge->nodeA), edgeMargin(*edge, edge->nodeB), world, visible.close, visible.prepare);
		}
	}
}

/// @brief Invalidate every road crossing an edited chunk, including roads whose endpoints are outside.
void RoadRenderer::synchronizeTerrainChanges(const World& world, const RoadNetwork& network)
{
	synchronizeSpatialIndex(network);
	for (const Chunk* chunk : world.getActiveChunks())
	{
		if (!chunk) { continue; }
		const int64 key = chunkCoordToKey(chunk->coord);
		if (!chunk->meshDirty) { m_dirtyTerrainObserved.erase(key); continue; }
		if (const auto observed = m_dirtyTerrainObserved.find(key); observed != m_dirtyTerrainObserved.end() && observed->second == chunk->contentRevision) { continue; }
		m_dirtyTerrainObserved[key] = chunk->contentRevision;
		const uint64 signature = terrainSignature(*chunk);
		const auto initial = m_preloadedHeights.find(key);
		if (initial != m_preloadedHeights.end() && initial->second == signature) { continue; }
		m_preloadedHeights[key] = signature;
		// Copy owners: invalidation marks their index entries dirty for the next synchronization.
		HashSet<int> edges, nodes;
		if (const auto found = m_spatialEdges.find(key); found != m_spatialEdges.end()) { edges = found->second; }
		if (const auto found = m_spatialNodes.find(key); found != m_spatialNodes.end()) { nodes = found->second; }
		for (int id : nodes)
		{
			eraseNodeCaches(id);
			if (const auto* node = network.getNode(id)) { for (const auto& attachment : node->attachments) { edges.insert(attachment.edgeId); } }
		}
		for (int id : edges) { eraseEdgeCaches(id); }
	}
}

void RoadRenderer::renderShadowCasters(Vec3 focus, double radius)
{
	for (const auto& [id,cache] : m_constructionCache)
	{
		const auto bounds = m_boundsCache.find(id);
		if (bounds == m_boundsCache.end()) continue;
		if (Vec3{bounds->second.center}.distanceFrom(focus) > radius + Sqrt(bounds->second.radiusSq)) continue;
		for (const auto& surface : cache.surfaces) surface.meshPair.detail.draw(ColorF{1});
		for (const auto& detail : cache.details) detail.mesh.draw(ColorF{1});
		for (const auto& [index,transform] : cache.machines) { m_constructionLodModels[index].draw(transform); }
	}
	for (const auto& [edgeId, entries] : m_partLodBatchCache)
	{
		const auto bounds = m_boundsCache.find(edgeId);
		if (bounds == m_boundsCache.end()) { continue; }
		const Vec3 delta = Vec3{ bounds->second.center } - focus;
		const double reach = radius + Sqrt(bounds->second.radiusSq);
		if (delta.x * delta.x + delta.z * delta.z > reach * reach) { continue; }
		for (const auto& entry : entries) { entry.mesh.draw(ColorF{ 1 }); }
		if (const auto furniture = m_streetFurnitureCache.find(edgeId); furniture != m_streetFurnitureCache.end())
		{
			for (const auto& batch : furniture->second)
			{
				if (!batch.cable) { batch.mesh.draw(ColorF{ 1 }); }
			}
		}
		if (const auto piers = m_pierMeshCache.find(edgeId); piers != m_pierMeshCache.end())
		{
			for (const auto& mesh : piers->second) { mesh.draw(ColorF{ 1 }); }
		}
		if (const auto signs = m_signCache.find(edgeId); signs != m_signCache.end()) { drawSigns(signs->second); }
		if (const auto signs = m_guideSignCache.find(edgeId); signs != m_guideSignCache.end()) { drawGuideSigns(signs->second); }
	}
	// 読み込み時の先行準備は表示ではない。未使用の全地域メッシュを影の描画へ送らない。
	for (const int id : m_drawnFallbackEdges)
	{
		const auto cached=m_fallbackEdges.find(id);
		if(cached==m_fallbackEdges.end()) { continue; }
		const auto& entries=cached->second;
		const auto bounds = m_boundsCache.find(id);
		if (bounds == m_boundsCache.end()) { continue; }
		const Vec3 delta = Vec3{bounds->second.center} - focus;
		const double reach = radius + Sqrt(bounds->second.radiusSq);
		if (delta.x * delta.x + delta.z * delta.z > reach * reach) { continue; }
		for (const auto& entry : entries) { entry.meshPair.detail.draw(ColorF{1}); }
	}
	for (const auto& [routeId, signs] : m_routeSignCache)
	{
		for (const auto& sign : signs)
		{
			if (sign.poleTop.distanceFrom(focus) > radius + 40) { continue; }
			if (m_signPoleMesh) { m_signPoleMesh->draw(sign.poleMat, ColorF{ 1 }); }
			if (const Mesh* board = getSignBoardMesh(sign.type)) { board->draw(sign.boardMat, ColorF{ 1 }); }
		}
	}
}

void RoadRenderer::eraseEdgeCaches(int edgeId)
{
	++m_detailRevisions[edgeId];
	m_dirtySpatialEdges.insert(edgeId);
	m_constructionCache.erase(edgeId);
	invalidateOverview(static_cast<int64>(edgeId) * 2);
	m_fallbackEdges.erase(edgeId);
	m_drawnFallbackEdges.erase(edgeId);
	if (m_partMeshCache.contains(edgeId) || m_streetFurnitureCache.contains(edgeId)
		|| m_pierMeshCache.contains(edgeId) || m_signCache.contains(edgeId) || m_guideSignCache.contains(edgeId))
	{
		++m_geometryRevision;
	}
	m_partMeshCache.erase(edgeId);
	m_partLodBatchCache.erase(edgeId);
	m_markingCacheByEdge.erase(edgeId);
	m_streetFurnitureCache.erase(edgeId);
	m_marginCache.erase(edgeId);
	m_boundsCache.erase(edgeId);
	m_pierMeshCache.erase(edgeId);
	m_signCache.erase(edgeId);
	m_guideSignCache.erase(edgeId);
	m_guideSignTexAllReady = false;  // エッジ変更時は案内標識テクスチャを再チェック
}

void RoadRenderer::eraseNodeCaches(int nodeId)
{
	m_dirtySpatialNodes.insert(nodeId);
	invalidateOverview(static_cast<int64>(nodeId) * 2 + 1);
	if (m_nodeCapCache.contains(nodeId) || m_signalAttachGeomCache.contains(nodeId)) { ++m_geometryRevision; }
	m_nodeCapCache.erase(nodeId);
	m_fallbackNodes.erase(nodeId);
	m_markingCacheByNode.erase(static_cast<int64>(nodeId) * 2);
	m_markingCacheByNode.erase(static_cast<int64>(nodeId) * 2 + 1);



	m_signalAttachGeomCache.erase(nodeId);
	m_nodeCapWireCache.erase(nodeId);
}

void RoadRenderer::invalidateEdgeCache(int edgeId, int nodeA, int nodeB)
{
	// A deleted edge no longer exposes its endpoints through RoadNetwork.
	if (const auto source = m_overviewSources.find(static_cast<int64>(edgeId) * 2); source != m_overviewSources.end())
	{
		if (nodeA < 0) { nodeA = source->second.nodeA; }
		if (nodeB < 0) { nodeB = source->second.nodeB; }
	}
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
		Array<int64> nodeOwners;
		for (const auto& [owner, source] : m_overviewSources) { if (owner % 2 != 0) { nodeOwners << owner; } }
		for (int64 owner : nodeOwners) { invalidateOverview(owner); }
		m_nodeCapCache.clear();
		m_fallbackNodes.clear();
		m_nodeCapWireCache.clear();
		m_markingCacheByNode.clear();


	}
}

void RoadRenderer::invalidateAllCaches()
{
	++m_detailEpoch;
	m_spatialNetwork=nullptr;m_indexedEdges=m_indexedNodes=0;
	m_spatialEdges.clear();m_spatialNodes.clear();m_edgeCells.clear();m_nodeCells.clear();m_spatialHeights.clear();
	m_dirtySpatialEdges.clear();m_dirtySpatialNodes.clear();
	m_overviewOwners.clear();
	m_overviewDirty.clear();
	m_overviewSources.clear();
	m_overviewBatches.clear();
	m_overviewDrawn.clear();
	m_preloadedHeights.clear();
	m_constructionCache.clear();
	m_fallbackEdges.clear();
	m_drawnFallbackEdges.clear();
	m_fallbackNodes.clear();
	m_pierMeshCache.clear();
	m_signalSummaryCache.clear();
	++m_geometryRevision;
	m_dirtyTerrainObserved.clear();
	m_partMeshCache.clear();
	m_partLodBatchCache.clear();
	m_markingCacheByEdge.clear();
	m_streetFurnitureCache.clear();
	m_marginCache.clear();
	m_nodeCapCache.clear();
	m_nodeCapWireCache.clear();
	m_markingCacheByNode.clear();



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
	m_signBoardBacks.clear();
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

bool RoadRenderer::canBuildCache()
{
	if (m_cacheBuildStats.buildMilliseconds() < m_cacheBuildBudgetMs) { return true; }
	++m_cacheBuildStats.deferred;
	return false;
}

RoadRenderer::EdgeBounds RoadRenderer::edgeBounds(const RoadNetwork& network, int edgeId) const
{
	EdgeBounds result{};
	if (const auto curve=network.getBezier(edgeId))
	{
		const Vec3 center=(curve->p0+curve->p1+curve->p2+curve->p3)*.25;
		result.center=Float3{center};
		for (const Vec3 point : {curve->p0,curve->p1,curve->p2,curve->p3})
		{
			result.radiusSq=Max(result.radiusSq,static_cast<float>(center.distanceFromSq(point)));
		}
		constexpr int kSamples=32;
		for (int i=0;i<=kSamples;++i) { result.samples << curve->evaluate(static_cast<float>(i)/kSamples); }
	}
	return result;
}

namespace
{
	/// @brief Preserve negative grid coordinates without shifting a negative signed value.
	int64 spatialKey(int x,int z) { return static_cast<int64>((static_cast<uint64>(static_cast<uint32>(x))<<32)|static_cast<uint32>(z)); }
}
void RoadRenderer::synchronizeSpatialIndex(const RoadNetwork& network)
{
	if(m_spatialNetwork!=&network || m_indexedEdges>network.edges().size() || m_indexedNodes>network.nodes().size())
	{
		if(m_spatialNetwork) { m_boundsCache.clear(); }
		m_spatialEdges.clear();m_spatialNodes.clear();m_edgeCells.clear();m_nodeCells.clear();m_spatialHeights.clear();
		m_indexedEdges=m_indexedNodes=0;m_spatialNetwork=&network;
	}
	auto index=[&](int id,bool node)
	{
		auto& cells=node ? m_spatialNodes : m_spatialEdges;auto& owners=node ? m_nodeCells : m_edgeCells;
		if(const auto previous=owners.find(id);previous!=owners.end()) { for(int64 key:previous->second) { cells[key].erase(id); } owners.erase(previous); }
		Vec3 center;double radius;
		if(node) { const auto* value=network.getNode(id);if(!value) { return; }center=value->position;radius=30; }
		else
		{
			if(!network.getEdge(id)) { m_boundsCache.erase(id);return; }
			if(!m_boundsCache.contains(id)) { m_boundsCache[id]=edgeBounds(network,id); }
			const auto& bounds=m_boundsCache.at(id);center=Vec3{bounds.center};radius=Sqrt(bounds.radiusSq) + RoadRenderDetail::equipmentPadding(*network.getEdge(id));
		}
		const int x0=static_cast<int>(Floor((center.x-radius)/CHUNK_SIZE)),x1=static_cast<int>(Floor((center.x+radius)/CHUNK_SIZE));
		const int z0=static_cast<int>(Floor((center.z-radius)/CHUNK_SIZE)),z1=static_cast<int>(Floor((center.z+radius)/CHUNK_SIZE));
		for(int z=z0;z<=z1;++z) for(int x=x0;x<=x1;++x)
		{
			const int64 key=spatialKey(x,z);cells[key].insert(id);owners[id] << key;
			const Vec2 range{center.y-radius,center.y+radius};
			if(const auto old=m_spatialHeights.find(key);old!=m_spatialHeights.end()) { old->second.x=Min(old->second.x,range.x);old->second.y=Max(old->second.y,range.y); }
			else { m_spatialHeights.emplace(key,range); }
		}
	};
	for(size_t i=m_indexedEdges;i<network.edges().size();++i) { if(network.edges()[i].id>=0) { m_dirtySpatialEdges.insert(network.edges()[i].id); } }
	for(size_t i=m_indexedNodes;i<network.nodes().size();++i) { if(network.nodes()[i].id>=0) { m_dirtySpatialNodes.insert(network.nodes()[i].id); } }
	for(int id:m_dirtySpatialEdges) { index(id,false); }for(int id:m_dirtySpatialNodes) { index(id,true); }
	m_dirtySpatialEdges.clear();m_dirtySpatialNodes.clear();m_indexedEdges=network.edges().size();m_indexedNodes=network.nodes().size();
}
Array<int> RoadRenderer::querySpatial(Vec3 eye,const ViewFrustum& frustum,bool nodes) const
{
	HashSet<int> selected;const auto& grid=nodes ? m_spatialNodes : m_spatialEdges;
	const int x0=static_cast<int>(Floor((eye.x-kDrawMaxDist)/CHUNK_SIZE)),x1=static_cast<int>(Floor((eye.x+kDrawMaxDist)/CHUNK_SIZE));
	const int z0=static_cast<int>(Floor((eye.z-kDrawMaxDist)/CHUNK_SIZE)),z1=static_cast<int>(Floor((eye.z+kDrawMaxDist)/CHUNK_SIZE));
	for(int z=z0;z<=z1;++z) for(int x=x0;x<=x1;++x)
	{
		const int64 key=spatialKey(x,z);const auto found=grid.find(key);if(found==grid.end() || found->second.empty()) { continue; }
		const auto height=m_spatialHeights.at(key);const double halfHeight=(height.y-height.x)*.5;
		const Sphere cell{Vec3{(x+.5)*CHUNK_SIZE,(height.x+height.y)*.5,(z+.5)*CHUNK_SIZE},Sqrt(CHUNK_SIZE*CHUNK_SIZE*.5+halfHeight*halfHeight)};
		if(!frustum.intersects(cell)) { continue; }
		for(int id:found->second) { selected.insert(id); }
	}
	Array<int> result;result.reserve(selected.size());for(int id:selected) { result << id; }result.sort();return result;
}

/// @brief Signal passes also use the index; shadow and scene passes may have different eye positions.
Array<int> RoadRenderer::queryNearbyNodes(const RoadNetwork& network, Vec3 eye, double radius)
{
	synchronizeSpatialIndex(network);
	Array<int> result;
	if (!m_spatialQuery)
	{
		for (const auto& node : network.nodes()) { if (node.id >= 0) { result << node.id; } }
		return result;
	}
	HashSet<int> selected;
	const int x0 = static_cast<int>(Floor((eye.x - radius) / CHUNK_SIZE)), x1 = static_cast<int>(Floor((eye.x + radius) / CHUNK_SIZE));
	const int z0 = static_cast<int>(Floor((eye.z - radius) / CHUNK_SIZE)), z1 = static_cast<int>(Floor((eye.z + radius) / CHUNK_SIZE));
	for (int z = z0; z <= z1; ++z) { for (int x = x0; x <= x1; ++x)
	{
		const auto found = m_spatialNodes.find(spatialKey(x,z));
		if (found != m_spatialNodes.end()) { for (int id : found->second) { selected.insert(id); } }
	} }
	result.reserve(selected.size()); for (int id : selected) { result << id; } result.sort(); return result;
}
