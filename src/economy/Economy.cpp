
#include "Economy.hpp"

double Economy::monthlyGrant() const
{
	// 交付単価 [億円/人/月]（04_gameplay_detail_spec.md §2、仕様値×0.1 で初期値1.5億に合わせる）
	double rate;
	if      (population <  5000) rate = 0.000080;
	else if (population < 20000) rate = 0.000100;
	else if (population < 50000) rate = 0.000120;
	else                         rate = 0.000130;

	double grant = static_cast<double>(population) * rate;

	// 幸福度ボーナス・ペナルティ
	if      (happiness >= 0.80) grant *= 1.1;
	else if (happiness <  0.40) grant *= 0.8;

	return grant;
}

double Economy::roadMaintenanceCost(const RoadNetwork& net) const
{
	// 道路維持費 [億円/km/月]（04_gameplay_detail_spec.md §3）
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
	funds += monthlyGrant();
	funds -= roadMaintenanceCost(net);
}
