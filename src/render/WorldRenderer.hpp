#pragma once
#include "../world/World.hpp"
#include <Siv3D/ViewFrustum.hpp>

/// @brief 地形メッシュの描画クラス
class WorldRenderer
{
public:
	/// @brief アクティブチャンクをカリングして描画する
	void render(World& world, const BasicCamera3D& camera);

	/// @brief 選択アウトライン用: 指定建物 1 棟のシルエットを描画する
	/// @details OBJ 建物はモデル形状、それ以外は Box 形状で描画する。
	///          色は呼び出し側の RT に白塗りされる想定（アルファ 1.0）。
	void drawBuildingSilhouette(const Chunk& chunk, const World& world,
	                            int col, int row, const ColorF& color);

	/// @brief 建物の当たり判定用 OrientedBox を返す（OBJ は外接直方体、それ以外は規定サイズ）
	Optional<OrientedBox> buildingHitBox(const Chunk& chunk, const World& world,
	                                     int col, int row);

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
		BuildingType type;     ///< 建物種別（モデル解決に利用）
		uint8  modelVariant;   ///< 種別内バリアント（住宅: 0..9, 商業: 固定 0）
		Float3 pos;       ///< 設置位置（Y は地表高さ）
		float  angle;     ///< Y 軸回転 [rad]
		float  scale;     ///< TOML 指定スケール
	};

	struct BuildingModelAsset
	{
		Model model;
		float scale = 1.0f;
	};

	/// @brief チャンクの地形メッシュデータを生成する
	MeshData buildTerrainMeshData(const Chunk& chunk);

	/// @brief チャンクを描画する（DynamicMesh キャッシュを利用）
	void drawChunk(Chunk& chunk, const World& world);

	/// @brief 建物メッシュキャッシュを再構築する
	void rebuildBuildingMeshes(Key key, const Chunk& chunk, const World& world);

	/// @brief キャッシュ済み建物バッチを描画する
	void drawCachedBuildings(Key key) const;

	/// @brief 建物 OBJ（種別+バリアント）を必要時にロードして返す
	BuildingModelAsset& getBuildingModelAsset(BuildingType type, uint8 variant);

	HashTable<Key, DynamicMesh>                   m_meshCache;
	HashTable<Key, Array<BuildingBatch>>          m_buildingMeshCache;
	HashTable<Key, Array<BuildingModelInstance>>  m_buildingModelCache;
	HashTable<uint32, BuildingModelAsset>         m_buildingModels; ///< 建物 OBJ+TOML（遅延ロード）
	Array<Chunk*>                        m_sortedChunks;      ///< ソート済みチャンク（カメラ移動時のみ再ソート）
	Point                                m_lastSortChunk{ 0x7FFFFFFF, 0x7FFFFFFF };
	size_t                               m_lastActiveCount = 0;
};
