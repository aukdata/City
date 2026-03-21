#pragma once
#include "TerrainType.hpp"
#include "PlaceNameGenerator.hpp"
#include "RoadPathfinder.hpp"
#include "../world/World.hpp"
#include "../road/RoadNetwork.hpp"
#include "../zone/ZoneManager.hpp"
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

	/// @brief 生成結果
	struct Result
	{
		Vec3         cameraFocus;   ///< 初期カメラ注視点（都市核の位置）
		PlaceNameDB  placeNames;    ///< 生成された地名データベース
	};

	/// @brief マップを生成してワールド・道路・ゾーン・鉄道を初期化する
	/// @param seed         乱数シード（同じシード→同じマップ）
	/// @param terrainType  地形タイプ
	Result generate(uint64 seed, TerrainType terrainType,
	                World& world, RoadNetwork& roads,
	                ZoneManager& zones, TrainNetwork& trainNet);

	/// @brief 既存ワールドの任意のリージョンに地区・道路・ゾーンを追加生成する
	/// @param regionOffset          リージョン左下のワールド XZ 座標 [m]
	/// @param seed                  ワールドシード
	/// @param existingUrbanCenters  他チャンクで既に配置済みの Urban 座標（Urban 10km 排他に使用）
	void generateRegion(Vec2 regionOffset, uint64 seed,
	                    World& world, RoadNetwork& roads, ZoneManager& zones,
	                    const Array<Vec2>& existingUrbanCenters = {});

	const Array<Settlement>& settlements() const { return m_settlements; }

	/// @brief 既存ノードの位置スナップショット（バックグラウンドスレッドでの最近傍検索用）
	struct NodeSnapshot { int id; Vec3 position; };

	/// @brief バックグラウンドスレッドでのチャンク事前構築結果
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
	};

	/// @brief ローカル RoadNetwork でチャンクを事前構築する（バックグラウンドスレッド用）
	/// @details World は const 読み取りのみ。共有状態を変更しないためスレッド安全。
	///   A* 接続 + ポスト処理もバックグラウンドで実行し、ポスト処理済みの結果を返す。
	static ChunkBuildResult buildChunkOffthread(
		Vec2 regionOffset, uint64 seed,
		const World& world,
		const Array<Vec2>& existingUrbanCenters,
		const Array<NodeSnapshot>& existingNodes);

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

	// ----- Phase 6: 初期ゾーン -----
	/// @brief 地区周辺にゾーンを自動割当てする
	void assignZones(World& world, ZoneManager& zones);

	// ----- 鉄道初期設定 -----
	void setupTrain(TrainNetwork& trainNet, World& world);

	// ----- ユーティリティ（RoadPathfinder へ委譲）-----
	Vec2  gridToWorld(int gx, int gz) const { return m_pf.gridToWorld(gx, gz); }
	Point worldToGrid(float wx, float wz) const { return m_pf.worldToGrid(wx, wz); }
	float gridHeight(int gx, int gz)  const { return m_pf.height(gx, gz); }

	// ----- 状態 -----
	RoadPathfinder    m_pf;           ///< 地形グリッドと A* を保持する共用パスファインダー
	Array<Settlement> m_settlements;
	Vec2              m_regionOffset; ///< 現在処理中のリージョン左下ワールド座標 [m]
};
