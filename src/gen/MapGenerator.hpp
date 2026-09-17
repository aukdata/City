#pragma once
#include <functional>
#include "SettlementPlan.hpp"
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
	/// @brief 集落の規模。歴史的起源は Settlement::plan.origin が保持する
	enum class SettlementKind : uint8
	{
		RegionalCity = 0,  ///< 地域中心都市
		LocalTown = 1,  ///< 地方町
		RuralSettlement = 2,  ///< 農村集落
	};

	/// @brief 地区データ
	struct Settlement
	{
		Vec2           center;                       ///< ワールド XZ 座標 [m]
		SettlementKind kind   = SettlementKind::RuralSettlement;
		float          radius = 0.0f;                ///< 影響半径 [m]
		float          score  = 0.0f;                ///< 地形適性スコア (0.0〜1.0)
		String         name;     ///< 地区地名（PlaceNameGenerator が設定）
		String         reading;  ///< ローマ字読み（PlaceNameGenerator が設定）
		/// @brief 街路・地割の共有軸方向（地形・街道に基づいて生成時に設定）
		Vec2           gridAxisX{ 1.0f, 0.0f };
		Vec2           gridAxisZ{ 0.0f, 1.0f };
		UrbanMorphology::Plan plan; ///< 成立史・駅・公共用地・街区を共有する計画
		Optional<size_t> serviceCenter; ///< 往来費用で選んだ中心町の配列index。行政界・成立順序ではない
		double accessCost = 0; ///< 中心町までの地形を考慮した距離相当費用 [m]
		Optional<Vec2> accessDirection; ///< 中心町へ向かう地形回廊の初期方向
	};

	/// @brief initWorld() の結果
	struct InitResult
	{
		PlaceNameDB  placeNames;
	};

	/// @brief ワールドパラメータ設定 + 地名生成（メインスレッドで即座に完了）
	static InitResult initWorld(uint64 seed, World& world);

	/// @brief 全ワールドの中心町と周辺農村を地形・農業立地・往来費用で配置する
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
	/// @param settlements 集落配列（城下町の gridAxisX/Z を更新する）
	static void generateDistrictRoads(
		uint64 seed,
		Array<Settlement>& settlements,
		const World& world,
		RoadNetwork& network,
		ProgressCallback onProgress = {});

	/// @brief 鉄道の初期路線を構築する
	static void setupTrain(TrainNetwork& trainNet, World& world,
	                        const Array<Settlement>& districts, RoadNetwork* roads=nullptr);

private:

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
	/// @param outEdgeIds (optional) 生成されたエッジ ID を起点→終点順で追記
	static void buildRoadSegment(
		const World& world, RoadNetwork& network,
		int startNodeId, Vec3 startPos, int endNodeId, Vec3 endPos,
		RoadType roadType, int lanes,
		HashSet<int64>& globalOccupied,
		const Array<Vec2>& forbiddenStartDirs = {},
		const Array<Vec2>& forbiddenGoalDirs = {},
		Array<int>* outEdgeIds = nullptr);
};
