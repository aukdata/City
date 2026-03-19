#include "World.hpp"
#include <cmath>

void World::update(Vec3 cameraWorldPos)
{
	const Point newChunk = worldToChunkCoord(cameraWorldPos);

	if (newChunk == m_cameraChunk)
		return;

	m_cameraChunk = newChunk;

	// 範囲外チャンクを Sleeping に降格
	for (auto& [key, chunk] : m_chunks)
	{
		if (chunk.state == ChunkState::Active)
		{
			const int dx = chunk.coord.x - m_cameraChunk.x;
			const int dy = chunk.coord.y - m_cameraChunk.y;
			if (Abs(dx) > ACTIVE_RANGE || Abs(dy) > ACTIVE_RANGE)
				chunk.state = ChunkState::Sleeping;
		}
	}

	// アクティブ範囲内のチャンクを確保・Active に設定
	for (int dy = -ACTIVE_RANGE; dy <= ACTIVE_RANGE; ++dy)
	{
		for (int dx = -ACTIVE_RANGE; dx <= ACTIVE_RANGE; ++dx)
		{
			const Point coord{ m_cameraChunk.x + dx, m_cameraChunk.y + dy };
			Chunk& chunk = getOrCreateChunk(coord);
			chunk.state = ChunkState::Active;
		}
	}
}

Chunk& World::getOrCreateChunk(Point coord)
{
	const Key key = makeKey(coord);

	if (not m_chunks.contains(key))
	{
		m_chunks.emplace(key, Chunk(coord));
		generateChunk(m_chunks[key]);
	}

	return m_chunks[key];
}

const Chunk* World::getChunk(Point coord) const
{
	const Key key = makeKey(coord);
	const auto it = m_chunks.find(key);
	if (it == m_chunks.end())
		return nullptr;
	return &it->second;
}

Chunk* World::getChunk(Point coord)
{
	const Key key = makeKey(coord);
	const auto it = m_chunks.find(key);
	if (it == m_chunks.end())
		return nullptr;
	return &it->second;
}

Array<Chunk*> World::getActiveChunks()
{
	Array<Chunk*> result;
	for (auto& [key, chunk] : m_chunks)
	{
		if (chunk.state == ChunkState::Active)
			result << &chunk;
	}
	return result;
}

Array<const Chunk*> World::getActiveChunks() const
{
	Array<const Chunk*> result;
	for (const auto& [key, chunk] : m_chunks)
	{
		if (chunk.state == ChunkState::Active)
			result << &chunk;
	}
	return result;
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

void World::generateChunk(Chunk& chunk)
{
	const Stopwatch sw{ StartImmediately::Yes };

	constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / HEIGHT_CELLS;

	for (int row = 0; row <= HEIGHT_CELLS; ++row)
	{
		for (int col = 0; col <= HEIGHT_CELLS; ++col)
		{
			const float wx = chunk.coord.x * CHUNK_SIZE + col * cellSize;
			const float wz = chunk.coord.y * CHUNK_SIZE + row * cellSize;
			chunk.heightMap[{ col, row }] = computeHeight(wx, wz);
		}
	}

	chunk.state = ChunkState::Active;
	chunk.isUrbanizationArea = true;

	Logger << U"[Chunk] ({}, {}) generated in {}ms"_fmt(
		chunk.coord.x, chunk.coord.y, sw.ms());
}
