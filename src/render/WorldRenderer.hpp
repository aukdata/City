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

	/// @brief 建物種別ごとの描画バッチ（色 + マージ済みメッシュ）
	struct BuildingBatch
	{
		ColorF color;
		Mesh   mesh;
	};

	static Key chunkKey(Point p)
	{
		return (static_cast<int64>(p.x) << 32) | static_cast<uint32>(p.y);
	}

	/// @brief チャンクの地形メッシュデータを生成する
	MeshData buildTerrainMeshData(const Chunk& chunk);

	/// @brief チャンクを描画する（DynamicMesh キャッシュを利用）
	void drawChunk(Chunk& chunk, const World& world);

	/// @brief 建物メッシュキャッシュを再構築する
	void rebuildBuildingMeshes(Key key, const Chunk& chunk, const World& world);

	/// @brief キャッシュ済み建物バッチを描画する
	void drawCachedBuildings(Key key) const;

	/// @brief 建物種別の高さを返す
	static float buildingHeight(BuildingType type, uint8 stage);

	/// @brief 建物種別の色を返す
	static ColorF buildingColor(BuildingType type);

	HashTable<Key, DynamicMesh>          m_meshCache;
	HashTable<Key, Array<BuildingBatch>> m_buildingMeshCache;
	Texture                              m_grassTexture;
};
