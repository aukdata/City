
#include "Economy.hpp"

double Economy::monthlyGrant() const
{
	// 交付金は人口帯ごとの基本単価を土台にし、幸福度で軽く増減させる。
	double rate;
	if      (population <  5000) rate = 0.000080;
	else if (population < 20000) rate = 0.000100;
	else if (population < 50000) rate = 0.000120;
	else                         rate = 0.000130;

	double grant = static_cast<double>(population) * rate;

	if      (happiness >= 0.80) grant *= 1.1;
	else if (happiness <  0.40) grant *= 0.8;

	return grant;
}

double Economy::roadMaintenanceCost(const RoadNetwork& net) const
{
	// 維持費は道路種別ごとの km 単価を全エッジへ積み上げ、月次固定費として集計する。
	double total = 0.0;
	for (const auto& edge : net.edges())
	{
		if (edge.id == -1) continue;
		const double km = edge.length / 1000.0;
		double costPerKm = 0.0;
		switch (edge.roadType)
		{
		case RoadType::LocalRoad:  costPerKm = 0.01; break;
		case RoadType::Arterial:   costPerKm = 0.03; break;
		case RoadType::Expressway: costPerKm = 0.30; break;
		default:                   costPerKm = 0.01; break;
		}
		total += km * costPerKm;
	}
	return total;
}

void Economy::applyMonthly(const RoadNetwork& net)
{
	// 月初処理では収入と維持費をまとめて資金へ反映し、経済状態を一段進める。
	funds += monthlyGrant();
	funds -= roadMaintenanceCost(net);
}

MonthlyEconomyResult Economy::previewMonthly(const RoadNetwork& net,
	const CitySnapshot& snapshot, int activeBusRouteCount) const
{
	MonthlyEconomyResult result;
	const MonthlyCityOutcome cityOutcome = calculateMonthlyCityOutcome(
		population, happiness, snapshot);
	Economy projected = *this;
	projected.happiness = cityOutcome.happiness;
	result.grant = projected.monthlyGrant();
	result.roadMaintenance = roadMaintenanceCost(net);
	result.busOperation = Max(0, activeBusRouteCount) * 0.10;
	result.balance = result.grant - result.roadMaintenance - result.busOperation;
	result.populationDelta = cityOutcome.populationDelta;
	result.happiness = cityOutcome.happiness;
	return result;
}

MonthlyEconomyResult Economy::applyMonthly(const RoadNetwork& net,
	const CitySnapshot& snapshot, int activeBusRouteCount)
{
	const MonthlyEconomyResult result = previewMonthly(net, snapshot, activeBusRouteCount);
	happiness = result.happiness;
	population = Max(0, population + result.populationDelta);
	funds += result.balance;
	return result;
}
