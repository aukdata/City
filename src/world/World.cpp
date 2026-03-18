#include "World.hpp"

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

void World::generateChunk(Chunk& chunk)
{
	// Phase 4-1: Perlin ノイズ（多重オクターブ）による地形生成
	// PerlinNoise は BasicPerlinNoise<double>。world 座標を直接渡すことでチャンク間でシームレスに繋がる。
	static const PerlinNoise s_perlin{ 20260316ULL };

	constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / HEIGHT_CELLS;
	// ノイズ周波数スケール（値が小さいほど地形がなだらか）
	constexpr double kFreq  = 0.00035;
	// 最大高さ [m]（道路は y=0 なので小さめに設定）
	constexpr float  kAmp   = 18.0f;

	for (int row = 0; row <= HEIGHT_CELLS; ++row)
	{
		for (int col = 0; col <= HEIGHT_CELLS; ++col)
		{
			const double wx = (chunk.coord.x * CHUNK_SIZE + col * cellSize) * kFreq;
			const double wz = (chunk.coord.y * CHUNK_SIZE + row * cellSize) * kFreq;
			const float h = static_cast<float>(s_perlin.octave2D0_1(wx, wz, 6, 0.5)) * kAmp - kAmp * 0.15f;
			chunk.heightMap[{ col, row }] = h;
		}
	}

	chunk.state = ChunkState::Active;
	chunk.isUrbanizationArea = true;
}
