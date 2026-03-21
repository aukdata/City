#pragma once
#include "TerrainType.hpp"
#include "PlaceNameGenerator.hpp"
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
	/// @param regionOffset  リージョン左下のワールド XZ 座標 [m]（kMapWidth / kMapDepth 単位でずらす）
	/// @param seed          ワールドシード（リージョン座標との XOR で独立した配置になる）
	void generateRegion(Vec2 regionOffset, uint64 seed,
	                    World& world, RoadNetwork& roads, ZoneManager& zones);

	const Array<Settlement>& settlements() const { return m_settlements; }

private:
	// ----- 定数 -----
	static constexpr int   kMapChunksX = 10;
	static constexpr int   kMapChunksZ = 10;
	static constexpr float kCellSize   = 40.0f;                        // 40 m/cell
	static constexpr int   kGridW      = 256;                          // kMapWidth / kCellSize = 10240/40
	static constexpr int   kGridH      = 256;                          // kMapDepth / kCellSize = 10240/40
	static constexpr float kMapWidth   = kMapChunksX * CHUNK_SIZE;    // 10240 m
	static constexpr float kMapDepth   = kMapChunksZ * CHUNK_SIZE;    // 10240 m

	// ----- Phase 1: 地形生成 -----
	/// @brief ハイトグリッドを構築する（40m セル中心の高さ、256×256）
	void buildHeightGrid(World& world);

	// ----- Phase 2: 地区配置 -----
	/// @brief Poisson ディスクサンプリングで地区核を配置する
	void placeSettlements(uint64 seed);

	/// @brief そのセルが地区に適しているか（平坦・陸地・適度な高さ）
	bool isSuitable(int gx, int gz) const;

	// ----- Phase 3: 旧道生成 -----
	/// @brief MST + A* で地区間を道路で繋ぐ
	void generateRoads(RoadNetwork& roads, uint64 seed);

	/// @brief Kruskal MST: 辺リスト (i,j) を返す
	Array<std::pair<int,int>> computeMST() const;

	/// @brief A* でグリッドセル間の最短経路を返す（セル座標リスト）
	/// @param forbiddenStartDirs 始点ノードで鋭角になる進行方向（outward 単位ベクトル）
	/// @param forbiddenGoalDirs  終点ノードで鋭角になる進行方向（outward 単位ベクトル）
	/// @param occupiedCells      既存パスが占有するグリッドセルの flat-index 集合
	Array<Point> findPath(Point start, Point goal,
	                      const Array<Vec2>& forbiddenStartDirs,
	                      const Array<Vec2>& forbiddenGoalDirs,
	                      const HashSet<int>& occupiedCells) const;

	/// @brief A* パスをサンプリングして Vec3 ウェイポイント列に変換する
	Array<Vec3> samplePath(const Array<Point>& path, int stepCells = 5) const;

	/// @brief ウェイポイント列をベジェ道路エッジとして RoadNetwork に追加する
	void pathToRoadEdges(const Array<Vec3>& wps,
	                     RoadNetwork& roads,
	                     RoadType rt, int lanes,
	                     int startNodeId, int endNodeId);

	// ----- Phase 6: 初期ゾーン -----
	/// @brief 地区周辺にゾーンを自動割当てする
	void assignZones(World& world, ZoneManager& zones);

	// ----- 鉄道初期設定 -----
	void setupTrain(TrainNetwork& trainNet, World& world);

	// ----- ユーティリティ -----
	/// @brief グリッド座標 → ワールド XZ 中心（リージョンオフセット込み）
	Vec2 gridToWorld(int gx, int gz) const
	{
		return Vec2{ m_regionOffset.x + (gx + 0.5f) * kCellSize,
		             m_regionOffset.y + (gz + 0.5f) * kCellSize };
	}
	/// @brief ワールド XZ → グリッド座標（クランプ済み、リージョンオフセット込み）
	Point worldToGrid(float wx, float wz) const
	{
		return Point{
			Clamp(static_cast<int>((wx - m_regionOffset.x) / kCellSize), 0, kGridW - 1),
			Clamp(static_cast<int>((wz - m_regionOffset.y) / kCellSize), 0, kGridH - 1)
		};
	}
	int gridIdx(int gx, int gz) const { return gz * kGridW + gx; }
	float gridHeight(int gx, int gz) const { return m_heightGrid[gridIdx(gx, gz)]; }

	// ----- 状態 -----
	Array<float>      m_heightGrid;      ///< 256×256 の高さキャッシュ [m]
	Array<Settlement> m_settlements;
	Vec2              m_regionOffset;    ///< 現在処理中のリージョン左下ワールド座標 [m]
};
