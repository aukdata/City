#pragma once
#include "TerrainType.hpp"
#include "PlaceNameGenerator.hpp"
#include "RoadPathfinder.hpp"
#include "../world/World.hpp"
#include "../road/RoadNetwork.hpp"
#include "../railway/TrainNetwork.hpp"

/// @brief プロシージャルマップ生成（03_procedural_generation_spec.md）
/// Phase 1: 地形生成 / Phase 2: 地区配置 / Phase 3: 旧道生成 / Phase 6: 初期ゾーン
class MapGenerator
{
public:
	/// @brief 地区種別
	enum class SettlementType { Urban, Suburbs, Rural };

	/// @brief 地区データ
	struct Settlement
	{
		Vec2           center;   ///< ワールド XZ 座標 [m]
		SettlementType type;
		float          radius;   ///< 影響半径 [m]
		String         name;     ///< 地区地名（PlaceNameGenerator が設定）
		String         reading;  ///< ローマ字読み（PlaceNameGenerator が設定）
	};

	/// @brief initWorld() の結果
	struct InitResult
	{
		PlaceNameDB  placeNames;    ///< 生成された地名データベース
	};

	/// @brief ワールドパラメータ設定 + 地名生成（メインスレッドで即座に完了）
	static InitResult initWorld(uint64 seed, TerrainType terrainType, World& world);

	/// @brief 既存ノードの位置スナップショット（バックグラウンドスレッドでの最近傍検索用）
	struct NodeSnapshot { int id; Vec3 position; };

	/// @brief チャンク事前構築結果
	/// @details ローカル RoadNetwork で生成した道路データ + ゾーン割当用の高さグリッドキャッシュ。
	///   ノード/エッジの ID はローカル (0-based) であり、メインスレッドでリマップが必要。
	///   connectionNodeId のノードは既存ネットワークに存在するため addNode 不要。
	struct ChunkBuildResult
	{
		Point             chunkCoord;
		Array<RoadNode>   localNodes;
		Array<RoadEdge>   localEdges;
		Array<Settlement> settlements;
		int               connectionNodeId = -1; ///< 接続先の既存ノード ID (-1 = 接続なし)
		Vec2              gridOffset;
		int               gridW    = 0;
		int               gridH    = 0;
		float             cellSize = 0.0f;
		Array<float>      heightGrid;
		Grid<float>       terrainHeightMap;       ///< 事前計算済み地形 heightMap
	};

	/// @brief チャンクを構築する（スレッド安全: World は const 読み取りのみ）
	/// @param skipPostProcess  true にするとローカルポスト処理をスキップ（初期生成時用）
	static ChunkBuildResult buildChunk(
		Vec2 regionOffset, uint64 seed,
		const World& world,
		const Array<Vec2>& existingUrbanCenters,
		const Array<NodeSnapshot>& existingNodes,
		bool skipPostProcess = false);

	/// @brief 鉄道の初期路線を構築する
	static void setupTrain(TrainNetwork& trainNet, const World& world,
	                        const Array<Settlement>& districts);

private:
	// ----- 定数 -----
	static constexpr int   kMapChunksX = 1;
	static constexpr int   kMapChunksZ = 1;
	static constexpr float kCellSize   = 40.0f;
	static constexpr int   kGridW      = 26;                           // ceil(1024/40)
	static constexpr int   kGridH      = 26;
	static constexpr float kMapWidth   = kMapChunksX * CHUNK_SIZE;    // 1024 m
	static constexpr float kMapDepth   = kMapChunksZ * CHUNK_SIZE;

	// ----- Phase 1: 地形生成 -----
	/// @brief ハイトグリッドを構築する（m_pf に委譲）
	void buildHeightGrid(const World& world);

	// ----- Phase 2: 地区配置 -----
	/// @brief Poisson ディスクサンプリングで地区核を配置する
	void placeSettlements(uint64 seed, const Array<Vec2>& existingUrbanCenters = {});

	/// @brief そのセルが地区に適しているか（平坦・陸地・適度な高さ）
	bool isSuitable(int gx, int gz) const;

	// ----- Phase 3: 旧道生成 -----
	/// @brief MST + A* で地区間を道路で繋ぐ
	void generateRoads(RoadNetwork& roads, uint64 seed);

	/// @brief Kruskal MST: 辺リスト (i,j) を返す
	Array<std::pair<int,int>> computeMST() const;

	// ----- ユーティリティ（RoadPathfinder へ委譲）-----
	Vec2  gridToWorld(int gx, int gz) const { return m_pf.gridToWorld(gx, gz); }
	Point worldToGrid(float wx, float wz) const { return m_pf.worldToGrid(wx, wz); }
	float gridHeight(int gx, int gz)  const { return m_pf.height(gx, gz); }

	// ----- 状態 -----
	RoadPathfinder    m_pf;           ///< 地形グリッドと A* を保持する共用パスファインダー
	Array<Settlement> m_settlements;
	Vec2              m_regionOffset; ///< 現在処理中のリージョン左下ワールド座標 [m]
};
