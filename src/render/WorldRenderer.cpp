#include "WorldRenderer.hpp"
#include "TreeGeometry.hpp"
#include "RegionalTerrain.hpp"
#include "TerrainMaterials.hpp"
#include "../asset/AssetRegistrar.hpp"
#include "../debug/DebugLog.hpp"
#include <Siv3D/Profiler.hpp>
#include <algorithm>

bool WorldRenderer::setRenderDistance(double meters)
{
	if (!RenderDistance::valid(meters)) { return false; }
	if (meters != m_renderDistance) { m_renderDistance = meters; ++m_visibilityRevision; }
	return true;
}

void WorldRenderer::prepareView(Vec3 eye)
{
	if (m_renderDistance != RenderDistance::kDefault && eye != m_buildingEye) { ++m_visibilityRevision; }
	m_buildingEye = eye;
}

void WorldRenderer::render(World& world, const RoadNetwork& network, const BasicCamera3D& camera)
{
	prepareView(camera.getEyePosition());
	m_treeRenderer.clear();
	publishCompletedTrees();
	uploadCompletedTerrain();
	// アクティブチャンクを近傍優先で回し、地形本体と水面を分けて描画する。
	// 地形メッシュを動的更新するため W100 警告を抑制する
	Profiler::EnableAssetCreationWarning(false);
	m_terrainRebuildBudget = Clamp(static_cast<int>(m_pendingTerrainRebuildKeys.size()), 1, 16);

	const auto regionalEye=camera.getEyePosition();
	if(regionalEye.y-world.sampleHeight(static_cast<float>(regionalEye.x),static_cast<float>(regionalEye.z))>RegionalTerrain::kMinimumClearance)
	{
		m_buildingsConsidered=m_buildingsSubmitted=m_buildingDrawCalls=m_buildingTriangles=0;
		if(m_regionalTerrain.isEmpty())
		{
			const Stopwatch timer{StartImmediately::Yes};
			for(const auto& batch:RegionalTerrain::batches(RegionalTerrain::build(world)))
			{
				if(!batch.data.indices.isEmpty()) { m_regionalTerrain<<TerrainMeshBatch{batch.materialKey,DynamicMesh{batch.data}}; }
			}
			DBG_LOG(U"[RegionalTerrain] buildMs={:.2f}"_fmt(timer.msF()));
		}
		{
			const ScopedCustomShader3D shader{m_terrainShader};const ScopedRenderStates3D state{RasterizerState::SolidCullNone};
			Graphics3D::SetPSTexture(2,TextureAsset(Asset::CoastSand));
			for(const auto& batch:m_regionalTerrain) { batch.mesh.draw(TextureAsset(TerrainMaterials::texture(batch.materialKey)),TerrainMaterials::color(batch.materialKey)); }
		}
		{const ScopedCustomShader3D shader{m_waterShader};Box{Vec3{WORLD_SIZE*.5,0,WORLD_SIZE*.5},Vec3{WORLD_SIZE,.04,WORLD_SIZE}}.draw(ColorF{1});}
		// すでに用意済みの建物は地域LODでも残す。地形全域を同期で詳細化しない。
		m_buildingFrustum=ViewFrustum{camera,160000};
		for(const auto& [key,far]:m_farBuildings) { if(m_buildingFrustum->intersects(far.bounds)) { drawCachedBuildings(key); } }
		m_treeRenderer.draw(m_foliageShader);
		return;
	}
	m_buildingFrustum = ViewFrustum{ camera, 9000.0 };
	m_buildingsConsidered = m_buildingsSubmitted = 0;
	m_buildingDrawCalls = m_buildingTriangles = 0;
	const Vec3 eye = camera.getEyePosition();
	m_buildingEye = eye;
	const int camCx = static_cast<int>(Math::Floor(eye.x / CHUNK_SIZE));
	const int camCz = static_cast<int>(Math::Floor(eye.z / CHUNK_SIZE));
	const Point camChunk{ camCx, camCz };

	const auto& activeChunks = world.getActiveChunks();
	const bool activeChunkOrderChanged = (m_lastActiveOrder.size() != activeChunks.size())
		|| !std::equal(activeChunks.begin(), activeChunks.end(), m_lastActiveOrder.begin(), m_lastActiveOrder.end());

	// ソート済み順序を維持しつつ、カメラ位置やアクティブ集合が変わった時だけ並べ直す
	if (camChunk != m_lastSortChunk || eye.distanceFromSq(m_lastSortEye)>128*128 || activeChunks.size() != m_lastActiveCount || activeChunkOrderChanged)
	{
		m_sortedChunks = activeChunks;
		m_lastActiveOrder=activeChunks;
		m_sortedChunks.sort_by([&](const Chunk* a, const Chunk* b)
		{
			return TreeGeometry::distanceSquared(a->coord,eye,{a->heightMin,a->heightMax}) < TreeGeometry::distanceSquared(b->coord,eye,{b->heightMin,b->heightMax});
		});
		m_lastSortChunk  = camChunk;
		m_lastSortEye = eye;
		m_lastActiveCount = activeChunks.size();
	}

	const auto sceneSize = Scene::Size();
	constexpr float kMargin = 512.0f;

	for (Chunk* chunk : m_sortedChunks)
	{
		if (!chunk) continue;

		// カメラチャンクと周囲8チャンク（計9チャンク）は常に描画
		const int dx = Math::Abs(chunk->coord.x - camCx);
		const int dz = Math::Abs(chunk->coord.y - camCz);
		const bool isNearCamera = (dx <= 1 && dz <= 1);

		// BoundingBox の 8 頂点がすべてスクリーン外なら描画スキップ。
		// 頂点単位で判定するため、連続地形の境界付近が過剰カリングされにくい。
		const Vec3 o = chunk->worldOrigin();
		const double yLo = static_cast<double>(chunk->heightMin);
		const double yHi = static_cast<double>(chunk->heightMax);
		const double cs  = static_cast<double>(CHUNK_SIZE);
		const Float3 corners[8] = {
			Float3{ static_cast<float>(o.x),      static_cast<float>(yLo), static_cast<float>(o.z)      },
			Float3{ static_cast<float>(o.x + cs), static_cast<float>(yLo), static_cast<float>(o.z)      },
			Float3{ static_cast<float>(o.x),      static_cast<float>(yLo), static_cast<float>(o.z + cs) },
			Float3{ static_cast<float>(o.x + cs), static_cast<float>(yLo), static_cast<float>(o.z + cs) },
			Float3{ static_cast<float>(o.x),      static_cast<float>(yHi), static_cast<float>(o.z)      },
			Float3{ static_cast<float>(o.x + cs), static_cast<float>(yHi), static_cast<float>(o.z)      },
			Float3{ static_cast<float>(o.x),      static_cast<float>(yHi), static_cast<float>(o.z + cs) },
			Float3{ static_cast<float>(o.x + cs), static_cast<float>(yHi), static_cast<float>(o.z + cs) },
		};
		bool anyVisible = false;
		for (const auto& c : corners)
		{
			const Float3 sp = camera.worldToScreenPoint(c);
			if (sp.z > 0.0f &&
			    sp.x >= -kMargin && sp.x <= sceneSize.x + kMargin &&
			    sp.y >= -kMargin && sp.y <= sceneSize.y + kMargin)
			{
				anyVisible = true;
				break;
			}
		}
		if (!anyVisible && !isNearCamera) continue;

		drawChunk(*chunk, world, network);
	}

	m_treeRenderer.draw(m_foliageShader);

	// ---- 水面（y=0 の半透明平面）----
	{
		const ScopedCustomShader3D waterShader{m_waterShader};
		const ColorF waterColor{1};
		constexpr double cs = static_cast<double>(CHUNK_SIZE);
		bool hasWater = false;
		int minChunkX = 0;
		int maxChunkX = 0;
		int minChunkZ = 0;
		int maxChunkZ = 0;

		for (const Chunk* chunk : m_sortedChunks)
		{
			if (!chunk || chunk->heightMin > 0.0f) continue;
			if (!hasWater)
			{
				minChunkX = maxChunkX = chunk->coord.x;
				minChunkZ = maxChunkZ = chunk->coord.y;
				hasWater = true;
			}
			else
			{
				minChunkX = Min(minChunkX, chunk->coord.x);
				maxChunkX = Max(maxChunkX, chunk->coord.x);
				minChunkZ = Min(minChunkZ, chunk->coord.y);
				maxChunkZ = Max(maxChunkZ, chunk->coord.y);
			}
		}

		if (hasWater)
		{
			const double minX = static_cast<double>(minChunkX) * cs;
			const double maxX = static_cast<double>(maxChunkX + 1) * cs;
			const double minZ = static_cast<double>(minChunkZ) * cs;
			const double maxZ = static_cast<double>(maxChunkZ + 1) * cs;
			Box{ (minX + maxX) * 0.5, 0.04, (minZ + maxZ) * 0.5,
				maxX - minX, 0.04, maxZ - minZ }.draw(waterColor);
		}
	}
}
