#pragma once
#include "../world/World.hpp"

/// @brief 地形メッシュの描画クラス
class WorldRenderer
{
public:
	/// @brief アクティブチャンクをすべて描画する
	void render(World& world);

	/// @brief チャンクのメッシュキャッシュを無効化する
	void markDirty(Point chunkCoord);


private:
	using Key = int64;

	static Key chunkKey(Point p)
	{
		return (static_cast<int64>(p.x) << 32) | static_cast<uint32>(p.y);
	}

	/// @brief チャンクの地形メッシュデータを生成する
	MeshData buildTerrainMeshData(const Chunk& chunk);

	/// @brief チャンクを描画する（DynamicMesh キャッシュを利用）
	void drawChunk(Chunk& chunk);

	/// @brief チャンク内の建物を Box で描画する
	void drawBuildings(const Chunk& chunk);

	HashTable<Key, Mesh> m_meshCache;
};
