#pragma once
#include <functional>
#include "TerrainType.hpp"
#include "PlaceNameGenerator.hpp"
#include "RoadPathfinder.hpp"
#include "../world/World.hpp"
#include "../road/RoadNetwork.hpp"
#include "../railway/TrainNetwork.hpp"

/// @brief プロシージャルマップ生成（03_procedural_generation_spec.md）
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
		float          score;    ///< 地形適性スコア (0.0〜1.0)
		String         name;     ///< 地区地名（PlaceNameGenerator が設定）
		String         reading;  ///< ローマ字読み（PlaceNameGenerator が設定）
	};

	/// @brief initWorld() の結果
	struct InitResult
	{
		PlaceNameDB  placeNames;
	};

	/// @brief ワールドパラメータ設定 + 地名生成（メインスレッドで即座に完了）
	static InitResult initWorld(uint64 seed, World& world);

	/// @brief 全ワールドの地区を一括配置する（地形スコアベース）
	static Array<Settlement> placeAllSettlements(uint64 seed, const World& world);

	/// @brief 進捗コールバック (0.0〜1.0)
	using ProgressCallback = std::function<void(float)>;

	/// @brief 3層分岐構造で道路を一括生成する
	static void generateGlobalRoads(
		uint64 seed,
		const Array<Settlement>& settlements,
		const World& world,
		RoadNetwork& network,
		ProgressCallback onProgress = {});

	/// @brief 各地区の内部生活道路を格子ベースで生成する（03_procedural_generation_spec.md §4）
	static void generateDistrictRoads(
		uint64 seed,
		const Array<Settlement>& settlements,
		const World& world,
		RoadNetwork& network,
		ProgressCallback onProgress = {});

	/// @brief 鉄道の初期路線を構築する
	static void setupTrain(TrainNetwork& trainNet, const World& world,
	                        const Array<Settlement>& districts);

private:
	static constexpr float kCellSize = 40.0f;
	static constexpr int   kGridW    = 26;
	static constexpr int   kGridH    = 26;

	/// @brief 地形適性スコアを計算する (0.0=不適, 1.0=最適)
	static float scoreSuitability(const RoadPathfinder& pf, int gx, int gz);

	/// @brief 指定インデックスの地区サブセットで Prim's MST を計算する
	static Array<std::pair<int,int>> computeMSTSubset(
		const Array<Settlement>& settlements, const Array<int>& indices);

	/// @brief MST 上の最長パス(diameter)を求める
	/// @return diameter パス上のインデックス列（indices 配列内のインデックス）
	static Array<int> findMSTDiameter(
		const Array<std::pair<int,int>>& mst, int nodeCount);

	/// @brief 2点間の A* 道路を生成する共通ヘルパー
	/// @param globalOccupied  既存道路が通過するワールド座標セル (cellSize=120m) のセット。
	///                        生成後、新道路の通過セルが追加される。
	static void buildRoadSegment(
		const World& world, RoadNetwork& network,
		int startNodeId, Vec3 startPos, int endNodeId, Vec3 endPos,
		RoadType roadType, int lanes,
		HashSet<int64>& globalOccupied,
		const Array<Vec2>& forbiddenStartDirs = {},
		const Array<Vec2>& forbiddenGoalDirs = {});
};
