#include "WorldRenderer.hpp"
#include "LandscapeMaterials.hpp"
#include "TerrainMaterials.hpp"
#include "TreeGeometry.hpp"
#include "../asset/AssetRegistrar.hpp"
#include "../debug/DebugLog.hpp"
#include <algorithm>

using LandscapeMaterials::detailColorForKey;

void WorldRenderer::trimDistantTreeDetails()
{
	Array<Key> distantTrees;
	for (const Key key : m_detailedTreeChunks)
	{
		const Point coord{static_cast<int>(key>>32),static_cast<int>(static_cast<uint32>(key))};
		const auto range=m_treeHeightRanges.find(key);
		if (range!=m_treeHeightRanges.end() && TreeGeometry::nearChunk(coord,m_buildingEye,range->second,TreeGeometry::kRetainDistance)) { continue; }
		if (auto cache=m_landscapeMeshCache.find(key);cache!=m_landscapeMeshCache.end())
		{
			cache->second.remove_if([](const BuildingBatch& batch){ return batch.materialKey>=1000; });
		}
		distantTrees << key;
	}
	for (const Key key : distantTrees) { m_detailedTreeChunks.erase(key);++m_geometryRevision; }
}

void WorldRenderer::uploadCompletedTerrain()
{
	// Upload a bounded portion of a completed worker snapshot per frame. Keep the
	// previous complete chunk visible until both terrain and landscape are ready.
	for (size_t index=0;index<m_terrainJobs.size();++index)
	{
		auto& job=m_terrainJobs[index];
		if (!job.result)
		{
			if (job.future.wait_for(std::chrono::seconds{0})!=std::future_status::ready) { continue; }
			job.result=job.future.get();
		}
		if (job.epoch!=m_terrainEpoch || job.revision!=m_terrainRevisions[job.key])
		{
			m_terrainJobs.erase(m_terrainJobs.begin()+index);
			break;
		}
		auto& result=*job.result;
		const Stopwatch upload{StartImmediately::Yes};
		constexpr double kUploadBudgetMilliseconds=2.0;
		while (job.terrainIndex<result.batches.size() && upload.msF()<kUploadBudgetMilliseconds)
		{
			auto& data=result.batches[job.terrainIndex++];
			if (!data.meshData.indices.isEmpty()) { job.uploadedTerrain << TerrainMeshBatch{data.materialKey,DynamicMesh{data.meshData}}; }
			data.meshData=MeshData{};
		}
		while (job.terrainIndex==result.batches.size() && job.landscapeIndex<result.landscape.size() && upload.msF()<kUploadBudgetMilliseconds)
		{
			auto& data=result.landscape[job.landscapeIndex++];
			if (!data.meshData.indices.isEmpty()) { job.uploadedLandscape << BuildingBatch{data.materialKey,detailColorForKey(data.materialKey),Mesh{data.meshData}}; }
			data.meshData=MeshData{};
		}
		const double elapsed=upload.msF();
		job.uploadTotalMilliseconds+=elapsed;job.maxUploadMilliseconds=Max(job.maxUploadMilliseconds,elapsed);++job.uploadFrames;
		if (job.terrainIndex==result.batches.size() && job.landscapeIndex==result.landscape.size())
		{
			m_meshCache[job.key]=std::move(job.uploadedTerrain);
			m_landscapeMeshCache[job.key]=std::move(job.uploadedLandscape);
			if (job.detailedTrees) { m_detailedTreeChunks.insert(job.key); }
			else { m_detailedTreeChunks.erase(job.key); }
			m_pendingTerrainRebuildKeys.erase(job.key);
			++m_geometryRevision;
			DBG_LOG(U"[Streaming] terrain workerMs={:.2f} uploadTotalMs={:.2f} maxFrameUploadMs={:.2f} uploadFrames={} detailedTrees={}"_fmt(
				result.milliseconds,job.uploadTotalMilliseconds,job.maxUploadMilliseconds,job.uploadFrames,job.detailedTrees));
			m_terrainJobs.erase(m_terrainJobs.begin()+index);
		}
		break;
	}
}

void WorldRenderer::drawChunk(Chunk& chunk, const World& world, const RoadNetwork& network)
{
	// チャンク単位で地形と建物の GPU キャッシュを更新し、そのまま描画まで完結させる。
	const Key key = chunkCoordToKey(chunk.coord);
	const double nearestDistance = m_buildingEye.distanceFrom(Vec3{
		Clamp(m_buildingEye.x, chunk.coord.x * static_cast<double>(CHUNK_SIZE), (chunk.coord.x + 1) * static_cast<double>(CHUNK_SIZE)),
		Clamp(m_buildingEye.y, static_cast<double>(chunk.heightMin), static_cast<double>(chunk.heightMax) + 8),
		Clamp(m_buildingEye.z, chunk.coord.y * static_cast<double>(CHUNK_SIZE), (chunk.coord.y + 1) * static_cast<double>(CHUNK_SIZE)) });
	if (nearestDistance > 600) { m_distantDetailChunks.insert(key); }
	else { m_distantDetailChunks.erase(key); }
	const Vec2 treeHeights{chunk.heightMin,chunk.heightMax};
	m_treeHeightRanges[key]=treeHeights;
	const bool prepareTrees=TreeGeometry::nearChunk(chunk.coord,m_buildingEye,treeHeights,TreeGeometry::kPrepareDistance);
	const bool needsTreeDetails=m_asyncTerrain && prepareTrees && !m_detailedTreeChunks.contains(key);
	const bool terrainDirty = chunk.meshDirty || m_pendingTerrainRebuildKeys.contains(key) || needsTreeDetails;

	auto rebuildTerrainBatches = [&]
	{
		Array<TerrainMeshBatch> batches;
		for (auto& data : buildTerrainMeshData(chunk, getChunkSubtractionQuads(network, chunk.coord)))
		{
			if (data.meshData.vertices.isEmpty()) continue;
			TerrainMeshBatch batch;
			batch.materialKey = data.materialKey;
			batch.mesh = DynamicMesh{ data.meshData };
			batches << std::move(batch);
		}
		m_meshCache[key] = std::move(batches);
		++m_geometryRevision;
	};

	if (m_asyncTerrain)
	{
		const bool absent = !m_meshCache.contains(key);
		if (absent || terrainDirty)
		{
			// A cheap uncut terrain remains visible until the exact road cut is ready.
			if (absent)
			{
				Array<TerrainMeshBatch> batches;
				for (auto& data : buildTerrainMeshData(chunk, {}))
				{
					batches << TerrainMeshBatch{data.materialKey, DynamicMesh{data.meshData}};
				}
				m_meshCache[key] = std::move(batches);
				m_pendingTerrainRebuildKeys.insert(key);
			}
			if (chunk.meshDirty || !m_buildingMeshCache.contains(key))
			{
				const Stopwatch timer{ StartImmediately::Yes };
				rebuildBuildingMeshes(key, chunk, world);
				DBG_LOG(U"[Streaming] buildings chunk=({}, {}) ms={:.2f}"_fmt(chunk.coord.x,chunk.coord.y,timer.msF()));
				++m_terrainRevisions[key];
				chunk.meshDirty = false;
			}
			const bool running = std::any_of(m_terrainJobs.begin(), m_terrainJobs.end(), [key](const TerrainJob& job) { return job.key == key; });
			if (!running && m_terrainJobs.size() < 2)
			{
				Chunk snapshot;
				snapshot.coord = chunk.coord;
				snapshot.heightMap = chunk.heightMap;
				snapshot.buildingGrid = chunk.buildingGrid;
				snapshot.zoneMap = chunk.zoneMap;
				snapshot.isUrbanizationArea = chunk.isUrbanizationArea;
				snapshot.landPatches = chunk.landPatches;
				Array<Chunk> heightSnapshots;
				for (int dz = -1; dz <= 1; ++dz) for (int dx = -1; dx <= 1; ++dx)
				{
					if (const auto* neighbor = world.getChunk(chunk.coord+Point{dx,dz}))
					{
						Chunk heights;
						heights.coord = neighbor->coord;
						heights.heightMap = neighbor->heightMap;
						heights.zoneMap = neighbor->zoneMap;
						heightSnapshots << std::move(heights);
					}
				}
				auto quads = getChunkSubtractionQuads(network, chunk.coord);
				const auto foundSites=m_transportSites.find(key);
				auto sites=foundSites==m_transportSites.end() ? Array<Polygon>{} : foundSites->second;
				auto rivers=world.rivers().subset(RectF{chunk.coord.x*CHUNK_SIZE-64,chunk.coord.y*CHUNK_SIZE-64,CHUNK_SIZE+128,CHUNK_SIZE+128});
				TerrainJob job{ key, m_terrainEpoch, m_terrainRevisions[key], {} };
				job.detailedTrees=prepareTrees || m_detailedTreeChunks.contains(key);
				job.future = std::async(std::launch::async,
					[snapshot = std::move(snapshot), quads = std::move(quads),
						heightSnapshots = std::move(heightSnapshots), detailedTrees = job.detailedTrees,
						rivers = std::move(rivers), sites = std::move(sites), woodland = m_woodlandEnabled]()
				{
					const Stopwatch timer{ StartImmediately::Yes };
					TerrainJobResult result;
					result.batches = buildTerrainMeshData(snapshot, quads);
					result.landscape = buildLandscapeMeshData(
						snapshot, quads, heightSnapshots, detailedTrees, rivers, sites, woodland);
					result.milliseconds = timer.msF();
					return result;
				});
				m_terrainJobs << std::move(job);
			}
		}
	}
	else
	{
		if (!m_meshCache.contains(key))
		{
			DBG_LOG(U"[TerrainBool] drawChunk build chunk=({}, {}) dirty={} pending={}"_fmt(
				chunk.coord.x, chunk.coord.y, chunk.meshDirty ? 1 : 0,
				m_pendingTerrainRebuildKeys.contains(key) ? 1 : 0));
			rebuildTerrainBatches();
			m_pendingTerrainRebuildKeys.erase(key);
			chunk.meshDirty = false;
			rebuildBuildingMeshes(key, chunk, world);
		}
		else if (terrainDirty && m_terrainRebuildBudget > 0)
		{
			DBG_LOG(U"[TerrainBool] drawChunk refill chunk=({}, {}) dirty={} pending={} budget={}"_fmt(
				chunk.coord.x, chunk.coord.y, chunk.meshDirty ? 1 : 0,
				m_pendingTerrainRebuildKeys.contains(key) ? 1 : 0, m_terrainRebuildBudget));
			rebuildTerrainBatches();
			m_pendingTerrainRebuildKeys.erase(key);
			chunk.meshDirty = false;
			--m_terrainRebuildBudget;
			rebuildBuildingMeshes(key, chunk, world);
		}

	}

	// 急斜面では地形メッシュの薄い断面が見えるため両面描画にする
	const ScopedRenderStates3D cullNone{ RasterizerState::SolidCullNone };
	if (const auto it = m_meshCache.find(key); it != m_meshCache.end())
	{
		const ScopedCustomShader3D shader{ m_terrainShader };
		Graphics3D::SetPSTexture(2, TextureAsset(Asset::CoastSand));
		for (const TerrainMeshBatch& batch : it->second)
		{
			batch.mesh.draw(TextureAsset(TerrainMaterials::texture(batch.materialKey)), TerrainMaterials::color(batch.materialKey));
		}
	}
	drawCachedBuildings(key);
}

