#include "WorldRenderer.hpp"
#include "LandscapeMaterials.hpp"
#include "TerrainMaterials.hpp"
#include "TreeGeometry.hpp"
#include "../asset/AssetRegistrar.hpp"
#include "../debug/DebugLog.hpp"
#include <algorithm>

using LandscapeMaterials::detailColorForKey;

size_t WorldRenderer::treeInstanceCount() const
{
	size_t count = 0;
	for (const auto& entry : m_treeCache) { count += entry.second.instances.size(); }
	for (const auto& entry : m_lotTreeCache) { count += entry.second.size(); }
	return count;
}

void WorldRenderer::submitCachedTrees(Key key) const
{
	const Point coord{static_cast<int>(key >> 32), static_cast<int>(static_cast<uint32>(key))};
	const auto range = m_treeHeightRanges.find(key);
	const Vec2 heights = range == m_treeHeightRanges.end() ? Vec2{} : range->second;
	if (const auto found = m_treeCache.find(key); found != m_treeCache.end())
	{
		m_treeRenderer.append(found->second.instances, coord, heights, m_buildingEye, m_renderDistance);
	}
	if (const auto found = m_lotTreeCache.find(key); found != m_lotTreeCache.end())
	{
		m_treeRenderer.append(found->second, coord, heights, m_buildingEye, m_renderDistance);
	}
}

void WorldRenderer::publishCompletedTrees()
{
	for (size_t index = 0; index < m_treeJobs.size();)
	{
		auto& job = m_treeJobs[index];
		if (job.future.wait_for(std::chrono::seconds{0}) != std::future_status::ready) { ++index; continue; }
		auto result = job.future.get();
		if (job.epoch == m_terrainEpoch && job.revision == m_terrainRevisions[job.key])
		{
			DBG_LOG(U"[TreeStreaming] key={} revision={} placementMs={:.2f} masksMs={:.2f} patchesMs={:.2f} woodlandMs={:.2f} instances={} excludedTrees={}"_fmt(
				job.key, job.revision, result.milliseconds, result.stats.masksMilliseconds,
				result.stats.patchesMilliseconds, result.stats.woodlandMilliseconds, result.instances.size(), result.stats.excludedTrees));
			m_treeCache[job.key] = TreeChunk{std::move(result.instances), job.revision};
			++m_geometryRevision;
		}
		else
		{
			DBG_LOG(U"[TreeStreaming] discard key={} revision={} currentRevision={} reason=staleSnapshot"_fmt(
				job.key, job.revision, m_terrainRevisions[job.key]));
		}
		m_treeJobs.erase(m_treeJobs.begin() + index);
	}
}

void WorldRenderer::startStreamingJobs(Key key, const Chunk& chunk, const World& world, const RoadNetwork& network, bool terrainDirty)
{
	const auto trees = m_treeCache.find(key);
	const bool treesDirty = trees == m_treeCache.end() || trees->second.revision != m_terrainRevisions[key];
	const bool startTrees = treesDirty && m_treeJobs.size() < 2
		&& std::none_of(m_treeJobs.begin(), m_treeJobs.end(), [key](const TreeJob& job) { return job.key == key; });
	const bool startTerrain = terrainDirty && m_terrainJobs.size() < 2
		&& std::none_of(m_terrainJobs.begin(), m_terrainJobs.end(), [key](const TerrainJob& job) { return job.key == key; });
	if (!startTrees && !startTerrain) { return; }
	auto snapshot = std::make_shared<LandscapeSnapshot>();
	snapshot->chunk.coord = chunk.coord;
	snapshot->chunk.heightMap = chunk.heightMap;
	snapshot->chunk.buildingGrid = chunk.buildingGrid;
	snapshot->chunk.zoneMap = chunk.zoneMap;
	snapshot->chunk.isUrbanizationArea = chunk.isUrbanizationArea;
	snapshot->chunk.landPatches = chunk.landPatches;
	for (int dz = -1; dz <= 1; ++dz) for (int dx = -1; dx <= 1; ++dx)
	{
		if (const auto* neighbor = world.getChunk(chunk.coord + Point{dx, dz}))
		{
			Chunk heights;
			heights.coord = neighbor->coord;
			heights.heightMap = neighbor->heightMap;
			heights.zoneMap = neighbor->zoneMap;
			snapshot->heights << std::move(heights);
		}
	}
	snapshot->quads = getChunkSubtractionQuads(network, chunk.coord);
	if (const auto sites = m_transportSites.find(key); sites != m_transportSites.end()) { snapshot->sites = sites->second; }
	snapshot->rivers = world.rivers().subset(RectF{chunk.coord.x * CHUNK_SIZE - 64, chunk.coord.y * CHUNK_SIZE - 64, CHUNK_SIZE + 128, CHUNK_SIZE + 128});
	snapshot->woodland = m_woodlandEnabled;
	// Placement has its own slots and can become visible during terrain work.
	const std::shared_ptr<const LandscapeSnapshot> input = std::move(snapshot);
	if (startTrees)
	{
		TreeJob job{key, m_terrainEpoch, m_terrainRevisions[key], {}};
		job.future = std::async(std::launch::async, [input]
		{
			const Stopwatch timer{StartImmediately::Yes};
			TreeJobResult result;
			(void)buildLandscapeMeshData(input->chunk, input->quads, input->heights, input->rivers,
				input->sites, result.stats, result.instances, true, input->woodland);
			result.milliseconds = timer.msF();
			return result;
		});
		m_treeJobs << std::move(job);
		++m_treePlacementBuilds;
	}
	if (startTerrain)
	{
		TerrainJob job{key, m_terrainEpoch, m_terrainRevisions[key], {}};
		job.future = std::async(std::launch::async, [input]
		{
			const Stopwatch timer{StartImmediately::Yes};
			TerrainJobResult result;
			result.batches = buildTerrainMeshData(input->chunk, input->quads);
			result.terrainMilliseconds = timer.msF();
			Array<TreeInstance> unusedTrees;
			result.landscape = buildLandscapeMeshData(input->chunk, input->quads, input->heights, input->rivers,
				input->sites, result.landscapeStats, unusedTrees, false, false);
			result.milliseconds = timer.msF();
			return result;
		});
		m_terrainJobs << std::move(job);
		++m_terrainBuilds;
	}
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
			m_pendingTerrainRebuildKeys.erase(job.key);
			++m_geometryRevision;
			DBG_LOG(U"[Streaming] terrain workerMs={:.2f} uploadTotalMs={:.2f} maxFrameUploadMs={:.2f} uploadFrames={}"_fmt(
				result.milliseconds,job.uploadTotalMilliseconds,job.maxUploadMilliseconds,job.uploadFrames));
			const auto& stats = result.landscapeStats;
			DBG_LOG(U"[LandscapeStreaming] key={} terrainMs={:.2f} masksMs={:.2f} patchesMs={:.2f} woodlandMs={:.2f} landscapeVertices={} landscapeTriangles={}"_fmt(
				job.key,result.terrainMilliseconds,stats.masksMilliseconds,
				stats.patchesMilliseconds,stats.woodlandMilliseconds,stats.vertices,stats.triangles));
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
	const bool terrainDirty = chunk.meshDirty || m_pendingTerrainRebuildKeys.contains(key);

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
				rebuildBuildingMeshes(key, chunk, world, network);
				DBG_LOG(U"[Streaming] buildings chunk=({}, {}) ms={:.2f}"_fmt(chunk.coord.x,chunk.coord.y,timer.msF()));
				++m_terrainRevisions[key];
				chunk.meshDirty = false;
			}
		}
		startStreamingJobs(key, chunk, world, network, absent || terrainDirty);
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
			rebuildBuildingMeshes(key, chunk, world, network);
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
			rebuildBuildingMeshes(key, chunk, world, network);
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

