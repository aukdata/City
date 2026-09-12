#pragma once
#include "TunnelGeometry.hpp"
#include "../world/World.hpp"
#include "../road/RoadNetwork.hpp"
#include <Siv3D/ViewFrustum.hpp>
#include <unordered_set>
#include <future>

/// @brief 地形メッシュの描画クラス
class WorldRenderer
{
public:
	/// @brief アクティブチャンクをカリングして描画する
	/// @brief Load building GPU assets during the loading phase, before camera travel.
	void preloadBuildingModels();
	void render(World& world, const RoadNetwork& network, const BasicCamera3D& camera);
	/// @brief Draw cached opaque city geometry into the sun's depth target.
	void renderShadowCasters(Vec3 focus, double radius) const;
	[[nodiscard]] uint64 geometryRevision() const { return m_geometryRevision; }
	[[nodiscard]] size_t buildingsConsidered() const { return m_buildingsConsidered; }
	[[nodiscard]] size_t buildingsSubmitted() const { return m_buildingsSubmitted; }
	/// @brief Build terrain booleans from immutable snapshots off the render thread.
	void setAsyncTerrain(bool enabled) { m_asyncTerrain = enabled; }
	[[nodiscard]] size_t pendingTerrainJobs() const { return m_terrainJobs.size(); }
	/// @brief The same terrain-following, road-clipped geometry is used for parcel selection.
	MeshData landPatchSurface(const World& world,const RoadNetwork& network,Point coord,const LandPatch& patch);
	void setTerrainShader(const PixelShader& shader) { m_terrainShader = shader; }
	void setLandscapeShaders(const PixelShader& field,const PixelShader& paddy,const PixelShader& foliage)
	{ m_fieldShader=field;m_paddyShader=paddy;m_foliageShader=foliage; }

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
	void setTunnelOpenings(const Array<TunnelGeometry::Opening>& openings) { m_tunnelOpenings=openings; invalidateAllTerrain(); }

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
	Array<TunnelGeometry::Opening> m_tunnelOpenings;

	/// @brief 建物種別ごとの描画バッチ（色 + マージ済みメッシュ）
	struct BuildingBatch
	{
		int    materialKey = 0;
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
		Model distantModel;
		float scale = 1.0f;
	};

	struct TerrainSubtractionQuad
	{
		Array<Vec2> footprint;
		RectF       bounds;
		float       bedBottomY = 0.0f;
	};

	struct TerrainMeshData
	{
		int      materialKey = 0;
		MeshData meshData;
	};

	struct TerrainMeshBatch
	{
		int         materialKey = 0;
		DynamicMesh mesh;
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
	static Array<TerrainMeshData> buildLandscapeMeshData(const Chunk& chunk, const Array<TerrainSubtractionQuad>& quads, const Array<Chunk>& heightSnapshots,bool detailedTrees);
	static Array<TerrainMeshData> buildTerrainMeshData(const Chunk& chunk, const Array<TerrainSubtractionQuad>& quads);

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

	struct TerrainJobResult
	{
		Array<TerrainMeshData> batches;
		Array<TerrainMeshData> landscape;
		double milliseconds = 0.0;
	};
	struct TerrainJob
	{
		Key key;
		uint64 epoch;
		uint64 revision;
		std::future<TerrainJobResult> future;
		Optional<TerrainJobResult> result;
		Array<TerrainMeshBatch> uploadedTerrain;
		Array<BuildingBatch> uploadedLandscape;
		size_t terrainIndex=0,landscapeIndex=0;
		double uploadTotalMilliseconds=0,maxUploadMilliseconds=0;
		int uploadFrames=0;
		bool detailedTrees=false;
	};
	Array<TerrainJob> m_terrainJobs;
	HashTable<Key, uint64> m_terrainRevisions;
	bool m_asyncTerrain = false;
	uint64 m_terrainEpoch = 0;

	HashTable<Key, Array<TerrainMeshBatch>>       m_meshCache;
	HashTable<Key, Array<TerrainSubtractionQuad>> m_chunkSubtractorCache;
	HashTable<int, TerrainBooleanSubtractor>      m_edgeSubtractorCache;
	HashTable<int, TerrainNodeSubtractor>         m_nodeSubtractorCache;
	std::unordered_set<Key>                       m_pendingTerrainRebuildKeys;
	HashTable<Key, Array<BuildingBatch>>          m_buildingMeshCache;
	HashTable<Key, Array<BuildingBatch>>          m_landscapeMeshCache;
	HashTable<Key, Array<BuildingModelInstance>>  m_buildingModelCache;
	HashTable<uint32, BuildingModelAsset>         m_buildingModels; ///< 建物 OBJ+TOML（遅延ロード）
	Array<Chunk*>                        m_sortedChunks;      ///< ソート済みチャンク（カメラ移動時のみ再ソート）
	Point                                m_lastSortChunk{ 0x7FFFFFFF, 0x7FFFFFFF };
	size_t                               m_lastActiveCount = 0;
	bool                                 m_chunkSubtractorPrimed = false;
	int                                  m_terrainRebuildBudget = 0;
	uint64 m_geometryRevision = 0;
	PixelShader m_terrainShader,m_fieldShader,m_paddyShader,m_foliageShader;
	bool drawLandscapeBatch(int materialKey,Key key) const;
	Optional<ViewFrustum> m_buildingFrustum;
	HashSet<Key> m_distantDetailChunks;
	HashSet<Key> m_detailedTreeChunks;
	Vec3 m_buildingEye{ 0, 0, 0 };
	mutable size_t m_buildingsConsidered = 0, m_buildingsSubmitted = 0;
};
