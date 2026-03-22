#include "World.hpp"
#include <cmath>

void World::update(Vec3 cameraWorldPos)
{
	const Point newChunk = worldToChunkCoord(cameraWorldPos);

	if (newChunk == m_cameraChunk)
		return;

	m_cameraChunk = newChunk;
	bool activeChanged = false;

	// 範囲外チャンクを Sleeping に降格
	for (auto& [key, chunk] : m_chunks)
	{
		if (chunk.state == ChunkState::Active)
		{
			const int dx = chunk.coord.x - m_cameraChunk.x;
			const int dy = chunk.coord.y - m_cameraChunk.y;
			if (Abs(dx) > ACTIVE_RANGE || Abs(dy) > ACTIVE_RANGE)
			{
				chunk.state = ChunkState::Sleeping;
				activeChanged = true;
			}
		}
	}

	// アクティブ範囲内の installChunk 済みチャンクを Active に設定
	// （地形生成はバックグラウンドスレッドの buildChunk → installChunk で行う）
	for (int dy = -ACTIVE_RANGE; dy <= ACTIVE_RANGE; ++dy)
	{
		for (int dx = -ACTIVE_RANGE; dx <= ACTIVE_RANGE; ++dx)
		{
			const Point coord{ m_cameraChunk.x + dx, m_cameraChunk.y + dy };
			if (Chunk* chunk = getChunk(coord))
			{
				if (chunk->state != ChunkState::Active)
					activeChanged = true;
				chunk->state = ChunkState::Active;
			}
		}
	}

	if (activeChanged)
		rebuildActiveChunkCache();
}

Chunk& World::getOrCreateChunk(Point coord)
{
	const Key key = chunkCoordToKey(coord);

	if (not m_chunks.contains(key))
	{
		const auto oldBuckets = m_chunks.bucket_count();
		m_chunks.emplace(key, Chunk(coord));
		if (m_chunks.bucket_count() != oldBuckets)
			rebuildActiveChunkCache();
		generateChunk(m_chunks[key]);
		m_newChunks << coord;
	}

	return m_chunks[key];
}

void World::installChunk(Point coord, Grid<float>&& heightMap, float heightMin, float heightMax)
{
	const Key key = chunkCoordToKey(coord);
	if (m_chunks.contains(key))
		return;   // 既に生成済み

	Chunk chunk(coord);
	chunk.heightMap = std::move(heightMap);
	chunk.heightMin = heightMin;
	chunk.heightMax = heightMax;
	// カメラの ACTIVE_RANGE 内なら Active、それ以外は Sleeping
	const bool inRange = (Abs(coord.x - m_cameraChunk.x) <= ACTIVE_RANGE &&
	                      Abs(coord.y - m_cameraChunk.y) <= ACTIVE_RANGE);
	chunk.state              = inRange ? ChunkState::Active : ChunkState::Sleeping;
	chunk.isUrbanizationArea = true;
	// emplace がリハッシュを起こすとアクティブチャンクキャッシュ内の
	// ポインタが無効化されるため、挿入前のバケット数を記録する
	const auto oldBuckets = m_chunks.bucket_count();
	m_chunks.emplace(key, std::move(chunk));
	m_newChunks << coord;
	// リハッシュが発生した場合、または新チャンクがアクティブ範囲内の場合にキャッシュを再構築する
	if (m_chunks.bucket_count() != oldBuckets || inRange)
		rebuildActiveChunkCache();
}

const Chunk* World::getChunk(Point coord) const
{
	const Key key = chunkCoordToKey(coord);
	const auto it = m_chunks.find(key);
	if (it == m_chunks.end())
		return nullptr;
	return &it->second;
}

Chunk* World::getChunk(Point coord)
{
	const Key key = chunkCoordToKey(coord);
	const auto it = m_chunks.find(key);
	if (it == m_chunks.end())
		return nullptr;
	return &it->second;
}

void World::rebuildActiveChunkCache()
{
	m_activeChunks.clear();
	m_activeChunksConst.clear();
	for (auto& [key, chunk] : m_chunks)
	{
		if (chunk.state == ChunkState::Active)
		{
			m_activeChunks << &chunk;
			m_activeChunksConst << &chunk;
		}
	}
}

const Array<Chunk*>& World::getActiveChunks()
{
	return m_activeChunks;
}

const Array<const Chunk*>& World::getActiveChunks() const
{
	return m_activeChunksConst;
}

float World::sampleHeight(float wx, float wz) const
{
	const int cx = static_cast<int>(Math::Floor(wx / CHUNK_SIZE));
	const int cz = static_cast<int>(Math::Floor(wz / CHUNK_SIZE));
	const Chunk* chunk = getChunk({ cx, cz });
	if (!chunk)
		return 0.0f;
	return chunk->getHeight(wx, wz);
}

void World::setGenerationParams(uint64 seed, TerrainType terrainType, float mapWidth, float mapDepth)
{
	m_seed        = seed;
	m_terrainType = terrainType;
	m_mapWidth    = mapWidth;
	m_mapDepth    = mapDepth;
	m_perlin      = PerlinNoise{ seed };  // PerlinNoise はここで一度だけ構築する
}

float World::computeHeight(float wx, float wz) const
{
	constexpr double kFreq = 0.00035;

	const float p01 = static_cast<float>(
		m_perlin.octave2D0_1(wx * kFreq, wz * kFreq, 6, 0.5));

	switch (m_terrainType)
	{
	case TerrainType::Basin:
	{
		const float cnx    = (wx / m_mapWidth - 0.5f) * 2.0f;
		const float cnz    = (wz / m_mapDepth - 0.5f) * 2.0f;
		const float radial = Clamp(std::sqrt(cnx * cnx + cnz * cnz) / 1.414f, 0.0f, 1.0f);
		const float r2     = radial * radial;
		return r2 * 360.0f + p01 * (120.0f + r2 * 260.0f) - 80.0f;
	}
	case TerrainType::Coastal:
	{
		const float ramp = Clamp(wz / m_mapDepth, 0.0f, 1.0f);
		return Math::Lerp(-30.0f, 240.0f, ramp) + p01 * Math::Lerp(10.0f, 400.0f, ramp);
	}
	case TerrainType::RiverFan:
	{
		const float ramp = Clamp(wz / m_mapDepth, 0.0f, 1.0f);
		return Math::Lerp(440.0f, -20.0f, ramp) + p01 * Math::Lerp(160.0f, 70.0f, ramp) - 40.0f;
	}
	case TerrainType::Hills:
	default:
	{
		const float p01med = static_cast<float>(
			m_perlin.octave2D0_1(wx * kFreq * 3.0, wz * kFreq * 3.0, 4, 0.5));
		return p01 * 180.0f + p01med * 100.0f - 50.0f;
	}
	}
}

HeightMapResult World::buildHeightMap(Point chunkCoord) const
{
	constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / HEIGHT_CELLS;
	Grid<float> hm(HEIGHT_CELLS + 1, HEIGHT_CELLS + 1, 0.0f);
	float lo =  1e30f;
	float hi = -1e30f;
	for (int row = 0; row <= HEIGHT_CELLS; ++row)
	{
		for (int col = 0; col <= HEIGHT_CELLS; ++col)
		{
			const float wx = chunkCoord.x * CHUNK_SIZE + col * cellSize;
			const float wz = chunkCoord.y * CHUNK_SIZE + row * cellSize;
			const float h  = computeHeight(wx, wz);
			hm[{ col, row }] = h;
			if (h < lo) lo = h;
			if (h > hi) hi = h;
		}
	}
	return { std::move(hm), lo, hi };
}

void World::generateChunk(Chunk& chunk)
{
	const Stopwatch sw{ StartImmediately::Yes };

	auto hmr = buildHeightMap(chunk.coord);
	chunk.heightMap = std::move(hmr.heightMap);
	chunk.heightMin = hmr.heightMin;
	chunk.heightMax = hmr.heightMax;
	const bool inRange = (Abs(chunk.coord.x - m_cameraChunk.x) <= ACTIVE_RANGE &&
	                      Abs(chunk.coord.y - m_cameraChunk.y) <= ACTIVE_RANGE);
	chunk.state = inRange ? ChunkState::Active : ChunkState::Sleeping;
	chunk.isUrbanizationArea = true;

	Logger << U"[Chunk] ({}, {}) generated in {}ms"_fmt(
		chunk.coord.x, chunk.coord.y, sw.ms());
}
