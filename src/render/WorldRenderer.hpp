#pragma once
#include "../world/World.hpp"
#include <Siv3D/ViewFrustum.hpp>

/// @brief 地形メッシュの描画クラス
class WorldRenderer
{
public:
	/// @brief アクティブチャンクをカリングして描画する
	void render(World& world, const BasicCamera3D& camera);

private:
	using Key = int64;

	/// @brief 建物種別ごとの描画バッチ（色 + マージ済みメッシュ）
	struct BuildingBatch
	{
		ColorF color;
		Mesh   mesh;
	};

	/// @brief OBJ モデルで描画する住宅建物1棟分のインスタンスデータ
	struct BuildingModelInstance
	{
		uint8  modelIdx;  ///< m_buildingModels 内のインデックス
		Float3 pos;       ///< 設置位置（Y は地表高さ）
		float  angle;     ///< Y 軸回転 [rad]
	};

	/// @brief チャンクの地形メッシュデータを生成する
	MeshData buildTerrainMeshData(const Chunk& chunk);

	/// @brief チャンクを描画する（DynamicMesh キャッシュを利用）
	void drawChunk(Chunk& chunk, const World& world);

	/// @brief 建物メッシュキャッシュを再構築する
	void rebuildBuildingMeshes(Key key, const Chunk& chunk, const World& world);

	/// @brief キャッシュ済み建物バッチを描画する
	void drawCachedBuildings(Key key) const;

	/// @brief 住宅建物 OBJ（001〜010）を必要時にロードして返す
	Model& getBuildingModel(uint8 idx);

	HashTable<Key, DynamicMesh>                   m_meshCache;
	HashTable<Key, Array<BuildingBatch>>          m_buildingMeshCache;
	HashTable<Key, Array<BuildingModelInstance>>  m_buildingModelCache;
	Array<Model>                                  m_buildingModels; ///< 住宅 OBJ（遅延ロード）
	Array<Chunk*>                        m_sortedChunks;      ///< ソート済みチャンク（カメラ移動時のみ再ソート）
	Point                                m_lastSortChunk{ 0x7FFFFFFF, 0x7FFFFFFF };
	size_t                               m_lastActiveCount = 0;
};
