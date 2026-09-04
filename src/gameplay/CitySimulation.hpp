#pragma once
#include "../world/World.hpp"
#include "../road/RoadNetwork.hpp"
#include "../traffic/Vehicle.hpp"

/// @brief 街全体の交通品質を表す集計値
struct TrafficKPI
{
	double averageSpeedKmh = 0.0;
	double averageSpeedRatio = 1.0;
	double averageCommuteMinutes = 0.0;
	double averageCongestion = 0.0;
	double maxCongestion = 0.0;
	int observedVehicleCount = 0;
	int slowVehicleCount = 0;
	int congestedEdgeCount = 0;
	int completedTripCount = 0;
};

/// @brief 全ワールドを対象にした都市統計のスナップショット
struct CitySnapshot
{
	int housingCapacity = 0;
	int residentialBuildings = 0;
	int commercialBuildings = 0;
	int industrialBuildings = 0;
	int parkBuildings = 0;
	int publicTransportStops = 0;
	double builtRoadLengthKm = 0.0;
	TrafficKPI traffic;
};

/// @brief 人口・用途・時間帯から算出する交通需要
struct TrafficDemand
{
	int targetVehicleCount = 0;
	double passengerShare = 0.70;
	double smallTruckShare = 0.20;
	double largeTruckShare = 0.10;
	double multiplier = 1.0;
};

/// @brief イベントが交通へ与える実効果
struct TrafficEventEffect
{
	double speedMultiplier = 1.0;
	double demandMultiplier = 1.0;
};

/// @brief 月次の人口・幸福度計算結果
struct MonthlyCityOutcome
{
	int populationDelta = 0;
	double happiness = 0.70;
	double commuteQuality = 1.0;
};

/// @brief 全チャンクの建物・道路・交通状態を集計する
CitySnapshot collectCitySnapshot(const World& world, const RoadNetwork& network,
	const Array<Vehicle>& vehicles, const Array<double>& completedTripMinutes = {});

/// @brief 人口と建物構成から交通需要を計算する
TrafficDemand calculateTrafficDemand(int population, const CitySnapshot& snapshot,
	double hour, double eventDemandMultiplier = 1.0);

/// @brief 交通品質などから次月の幸福度と人口変化を計算する
MonthlyCityOutcome calculateMonthlyCityOutcome(int population, double currentHappiness,
	const CitySnapshot& snapshot);

