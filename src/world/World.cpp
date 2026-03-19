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
}

void World::generateChunk(Chunk& chunk)
{
	// Perlin ノイズ（多重オクターブ）による地形生成
	// PerlinNoise は BasicPerlinNoise<double>。world 座標を直接渡すことでチャンク間でシームレスに繋がる。
	const PerlinNoise perlin{ m_seed };

	constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / HEIGHT_CELLS;
	// ノイズ周波数スケール（値が小さいほど地形がなだらか）
	constexpr double kFreq = 0.00035;

	for (int row = 0; row <= HEIGHT_CELLS; ++row)
	{
		for (int col = 0; col <= HEIGHT_CELLS; ++col)
		{
			// ワールド座標 [m]
			const float wx = chunk.coord.x * CHUNK_SIZE + col * cellSize;
			const float wz = chunk.coord.y * CHUNK_SIZE + row * cellSize;

			// 基本 Perlin ノイズ [0, 1]（低周波・大地形）
			const float p01 = static_cast<float>(
				perlin.octave2D0_1(wx * kFreq, wz * kFreq, 6, 0.5));

			float h = 0.0f;

			switch (m_terrainType)
			{
			case TerrainType::Basin:
			{
				// 山間盆地: 中央を凹ませるラジアルグラデーション
				// マップ中央が平野・低地、外周が山地
				const float cnx    = (wx / m_mapWidth - 0.5f) * 2.0f;  // [-1, 1]
				const float cnz    = (wz / m_mapDepth - 0.5f) * 2.0f;  // [-1, 1]
				const float radial = Clamp(std::sqrt(cnx * cnx + cnz * cnz) / 1.414f, 0.0f, 1.0f);
				// 中央(radial=0): 平坦・低高度  外周(radial=1): 山岳
				const float r2     = radial * radial;
				const float base   = r2 * 360.0f;
				const float var    = 120.0f + r2 * 260.0f;
				h = base + p01 * var - 80.0f;
				break;
			}
			case TerrainType::Coastal:
			{
				// 沿岸平野: z=0 が海岸、z=kMapDepth が山地
				const float ramp = Clamp(wz / m_mapDepth, 0.0f, 1.0f);
				const float base = Math::Lerp(-30.0f, 240.0f, ramp);
				const float var  = Math::Lerp(10.0f, 400.0f, ramp);
				h = base + p01 * var;
				break;
			}
			case TerrainType::RiverFan:
			{
				// 河川扇状地: z=0 が山（上流）、z=kMapDepth が扇端の低地
				const float ramp = Clamp(wz / m_mapDepth, 0.0f, 1.0f);
				const float base = Math::Lerp(440.0f, -20.0f, ramp);
				const float var  = Math::Lerp(160.0f, 70.0f, ramp);
				h = base + p01 * var - 40.0f;
				break;
			}
			case TerrainType::Hills:
			default:
			{
				// 丘陵台地: 中周波ノイズを強調して緩やかな丘を形成
				const float p01med = static_cast<float>(
					perlin.octave2D0_1(wx * kFreq * 3.0, wz * kFreq * 3.0, 4, 0.5));
				h = p01 * 180.0f + p01med * 100.0f - 50.0f;
				break;
			}
			}

			chunk.heightMap[{ col, row }] = h;
		}
	}

	chunk.state = ChunkState::Active;
	chunk.isUrbanizationArea = true;
}
