
#pragma once
#include "../road/RoadNetwork.hpp"
#include "../gameplay/CitySimulation.hpp"

/// @brief 交通・人口を含む月次経済処理の結果
struct MonthlyEconomyResult
{
	double grant = 0.0;
	double roadMaintenance = 0.0;
	double busOperation = 0.0;
	double balance = 0.0;
	int populationDelta = 0;
	double happiness = 0.70;
};

/// @brief 月次経済シミュレーション（04_gameplay_detail_spec.md §2）
struct Economy
{
	double funds      = 30.0;    ///< 現在資金 [億円]（開始: 30億円）
	int    population = 15000;   ///< 人口
	double happiness  = 0.70;    ///< 幸福度 0.0〜1.0

	/// @brief 人口規模に応じた交付金を返す [億円/月]
	/// @details 交付単価は人口規模ステップで変化（04_gameplay_detail_spec.md §2）
	double monthlyGrant() const;

	/// @brief 道路維持費を返す [億円/月]
	/// @details 道路種別ごとの単価 × 総延長（04_gameplay_detail_spec.md §2）
	double roadMaintenanceCost(const RoadNetwork& net) const;

	/// @brief 月初に収支を適用する（交付金入金・維持費引き落とし）
	void applyMonthly(const RoadNetwork& net);

	/// @brief 交通品質と公共交通費を含む月次結果を副作用なしで計算する
	MonthlyEconomyResult previewMonthly(const RoadNetwork& net, const CitySnapshot& snapshot,
		int activeBusRouteCount) const;

	/// @brief 交通品質・人口成長・バス費を含む月次処理を適用する
	MonthlyEconomyResult applyMonthly(const RoadNetwork& net, const CitySnapshot& snapshot,
		int activeBusRouteCount);
};
