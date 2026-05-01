#pragma once
#include "../world/World.hpp"
#include "../road/RoadNetwork.hpp"
#include <Siv3D/ViewFrustum.hpp>
#include <unordered_set>

/// @brief 地形メッシュの描画クラス
class WorldRenderer
{
public:
	/// @brief アクティブチャンクをカリングして描画する
	void render(World& world, const RoadNetwork& network, const BasicCamera3D& camera);

	/// @brief 指定エッジ群に関係する地形 subtraction キャッシュを無効化する
	void invalidateTerrainForEdges(const RoadNetwork& network, const Array<int>& edgeIds);

	/// @brief 指定ノード群に関係する地形 subtraction キャッシュを無効化する
	void invalidateTerrainForNodes(const RoadNetwork& network, const Array<int>& nodeIds);

	/// @brief ノード移動時に、交差点と接続エッジ両端を含むチャンクだけを無効化する
	void invalidateTerrainNearDirtyNodes(const RoadNetwork& network, const Array<int>& nodeIds);

	/// @brief ノード移動時に、移動前ノード位置も含めて局所チャンクだけを無効化する
	void invalidateTerrainNearMovedNode(const RoadNetwork& network, int nodeId, Vec3 oldNodePos);

	/// @brief 既に削除されたエッジの旧 subtraction キャッシュだけを無効化する
	void invalidateTerrainForEdge(int edgeId);

	/// @brief 既に削除されたノードの旧 subtraction キャッシュだけを無効化する
	void invalidateTerrainForNode(int nodeId);

	/// @brief 地形 subtraction / 地形メッシュの全キャッシュを無効化する
	void invalidateAllTerrain();

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

	struct TerrainSubtractionQuad
	{
		Array<Vec2> footprint;
		RectF       bounds;
		float       bedBottomY = 0.0f;
	};

	struct TerrainBooleanSubtractor
	{
		int         edgeId = -1;
		Array<TerrainSubtractionQuad> quads;
		Array<Key> touchedChunkKeys;
	};

	struct TerrainNodeSubtractor
	{
		int         nodeId = -1;
		Array<TerrainSubtractionQuad> quads;
		Array<Key> touchedChunkKeys;
	};

	/// @brief チャンクの地形メッシュデータを生成する
	MeshData buildTerrainMeshData(const Chunk& chunk, const RoadNetwork& network);

	/// @brief チャンクを描画する（DynamicMesh キャッシュを利用）
	void drawChunk(Chunk& chunk, const World& world, const RoadNetwork& network);

	/// @brief エッジ単位の subtraction 形状を必要時に構築して返す
	const TerrainBooleanSubtractor* getTerrainSubtractor(const RoadNetwork& network, int edgeId);

	/// @brief ノード単位の subtraction 形状を必要時に構築して返す
	const TerrainNodeSubtractor* getTerrainNodeSubtractor(const RoadNetwork& network, int nodeId);

	/// @brief 指定チャンクへ重なる subtraction 形状を組み立てて返す
	const Array<TerrainSubtractionQuad>& getChunkSubtractionQuads(const RoadNetwork& network, Point chunkCoord);

	/// @brief 指定チャンク群の地形メッシュ / subtraction キャッシュを無効化する
	void invalidateTerrainChunkKeys(const std::unordered_set<Key>& chunkKeys, bool rebuildImmediately = false);

	/// @brief edge/node subtractor から chunk subtractor cache を一括構築する
	void primeAllChunkSubtractorCaches(const RoadNetwork& network);

	/// @brief 建物メッシュキャッシュを再構築する
	void rebuildBuildingMeshes(Key key, const Chunk& chunk, const World& world);

	/// @brief キャッシュ済み建物バッチを描画する
	void drawCachedBuildings(Key key) const;

	/// @brief 建物 OBJ（種別+バリアント）を必要時にロードして返す
	BuildingModelAsset& getBuildingModelAsset(BuildingType type, uint8 variant);

	HashTable<Key, DynamicMesh>                   m_meshCache;
	HashTable<Key, Array<TerrainSubtractionQuad>> m_chunkSubtractorCache;
	HashTable<int, TerrainBooleanSubtractor>      m_edgeSubtractorCache;
	HashTable<int, TerrainNodeSubtractor>         m_nodeSubtractorCache;
	std::unordered_set<Key>                       m_pendingTerrainRebuildKeys;
	HashTable<Key, Array<BuildingBatch>>          m_buildingMeshCache;
	HashTable<Key, Array<BuildingModelInstance>>  m_buildingModelCache;
	HashTable<uint32, BuildingModelAsset>         m_buildingModels; ///< 建物 OBJ+TOML（遅延ロード）
	Array<Chunk*>                        m_sortedChunks;      ///< ソート済みチャンク（カメラ移動時のみ再ソート）
	Point                                m_lastSortChunk{ 0x7FFFFFFF, 0x7FFFFFFF };
	size_t                               m_lastActiveCount = 0;
	bool                                 m_chunkSubtractorPrimed = false;
	int                                  m_terrainRebuildBudget = 0;
};
