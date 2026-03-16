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

void World::generateChunk(Chunk& chunk)
{
	// Phase 1: フラット地形（heightMap は Chunk コンストラクタで 0.0f 初期化済み）
	chunk.state = ChunkState::Active;
	chunk.isUrbanizationArea = true;
}
