#include "../gen/GenerationSettings.hpp"
#include "CitySimulation.hpp"

namespace
{
	constexpr double kTrafficVehicleHeadwayMeters = 25.0;
	constexpr double kCongestedEdgeThreshold = 0.80;

	double timeOfDayMultiplier(double hour)
	{
		if (hour >= 6.0 && hour < 9.0) return GenerationSettings::get().traffic_morningMultiplier;
		if (hour >= 9.0 && hour < 17.0) return GenerationSettings::get().traffic_dayMultiplier;
		if (hour >= 17.0 && hour < 20.0) return GenerationSettings::get().traffic_eveningMultiplier;
		if (hour >= 20.0 && hour < 23.0) return GenerationSettings::get().traffic_lateMultiplier;
		return GenerationSettings::get().traffic_nightMultiplier;
	}

	double commuteGrowthMultiplier(double commuteMinutes)
	{
		if (commuteMinutes < 15.0) return 1.20;
		if (commuteMinutes < 30.0) return 1.00;
		if (commuteMinutes < 45.0) return 0.50;
		if (commuteMinutes < 60.0) return 0.0;
		return -0.30;
	}

	double commuteQuality(double commuteMinutes)
	{
		if (commuteMinutes <= 15.0) return 1.0;
		if (commuteMinutes >= 60.0) return 0.0;
		return 1.0 - (commuteMinutes - 15.0) / 45.0;
	}
}

CitySnapshot collectCitySnapshot(const World& world, const RoadNetwork& network,
	const Array<Vehicle>& vehicles, const Array<double>& completedTripMinutes)
{
	CitySnapshot snapshot;
	for (int chunkY = 0; chunkY < WORLD_CHUNKS; ++chunkY)
	{
		for (int chunkX = 0; chunkX < WORLD_CHUNKS; ++chunkX)
		{
			const Chunk* chunk = world.getChunk(Point{ chunkX, chunkY });
			if (!chunk) continue;
			for (int row = 0; row < ZONE_CELLS; ++row)
			{
				for (int col = 0; col < ZONE_CELLS; ++col)
				{
					const BuildingType type = chunk->buildingGrid[{ col, row }].type;
					snapshot.housingCapacity += buildingCapacity(type);
					if (isResidentialBuildingType(type)) ++snapshot.residentialBuildings;
					else if (type == BuildingType::Shop || type == BuildingType::Office || type == BuildingType::OfficeTower || type == BuildingType::ShoppingMall || isRoadsideServiceBuilding(type)) ++snapshot.commercialBuildings;
					else if (type == BuildingType::Factory) ++snapshot.industrialBuildings;
					else if (type == BuildingType::ParkBuilding) ++snapshot.parkBuildings;
				}
			}
		}
	}

	HashTable<int, int> vehiclesByEdge;
	double speedTotal = 0.0;
	double speedRatioTotal = 0.0;
	for (const auto& vehicle : vehicles)
	{
		if (vehicle.currentEdge >= 0) ++vehiclesByEdge[vehicle.currentEdge];
		if (vehicle.mode != VehicleMode::Active) continue;
		const RoadEdge* edge = network.getEdge(vehicle.currentEdge);
		if (!edge || edge->speedLimit <= 0.0f) continue;
		const double speedKmh = Max(0.0, static_cast<double>(vehicle.speed) * 3.6);
		const double ratio = Clamp(speedKmh / edge->speedLimit, 0.0, 1.5);
		speedTotal += speedKmh;
		speedRatioTotal += ratio;
		++snapshot.traffic.observedVehicleCount;
		if (ratio < 0.35) ++snapshot.traffic.slowVehicleCount;
	}
	if (snapshot.traffic.observedVehicleCount > 0)
	{
		snapshot.traffic.averageSpeedKmh = speedTotal / snapshot.traffic.observedVehicleCount;
		snapshot.traffic.averageSpeedRatio = speedRatioTotal / snapshot.traffic.observedVehicleCount;
	}

	int roadCount = 0;
	double congestionTotal = 0.0;
	for (const auto& edge : network.edges())
	{
		if (edge.id < 0 || !edge.isRoadbedBuilt()) continue;
		snapshot.builtRoadLengthKm += edge.length / 1000.0;
		const int laneCount = Max(1, static_cast<int>(edge.lanes.size()));
		const double capacity = Max(1.0, laneCount * Max(1.0, static_cast<double>(edge.length))
			/ kTrafficVehicleHeadwayMeters);
		const int count = vehiclesByEdge.contains(edge.id) ? vehiclesByEdge[edge.id] : 0;
		const double congestion = Max(
			Clamp(static_cast<double>(count) / capacity, 0.0, 1.0),
			Clamp(static_cast<double>(edge.congestion), 0.0, 1.0));
		congestionTotal += congestion;
		snapshot.traffic.maxCongestion = Max(snapshot.traffic.maxCongestion, congestion);
		if (congestion >= kCongestedEdgeThreshold) ++snapshot.traffic.congestedEdgeCount;
		++roadCount;
	}
	if (roadCount > 0) snapshot.traffic.averageCongestion = congestionTotal / roadCount;

	if (!completedTripMinutes.isEmpty())
	{
		double total = 0.0;
		for (const double minutes : completedTripMinutes) total += minutes;
		snapshot.traffic.completedTripCount = static_cast<int>(completedTripMinutes.size());
		snapshot.traffic.averageCommuteMinutes = total / completedTripMinutes.size();
	}
	else if (snapshot.traffic.observedVehicleCount > 0)
	{
		const double speedRatio = Clamp(snapshot.traffic.averageSpeedRatio, 0.10, 1.0);
		snapshot.traffic.averageCommuteMinutes = 20.0 / speedRatio;
	}

	return snapshot;
}

TrafficDemand calculateTrafficDemand(int population, const CitySnapshot& snapshot,
	double hour, double eventDemandMultiplier)
{
	TrafficDemand demand;
	const double populationVehicles = Max(0, population) / GenerationSettings::get().traffic_peoplePerCar;
	const double activityVehicles = snapshot.commercialBuildings * GenerationSettings::get().traffic_commercialCars
		+ snapshot.industrialBuildings * GenerationSettings::get().traffic_industrialCars;
	demand.multiplier = timeOfDayMultiplier(hour) * Max(0.0, eventDemandMultiplier);
	demand.targetVehicleCount = Clamp(static_cast<int>(Math::Round(
		(populationVehicles + activityVehicles) * demand.multiplier)), 0, GenerationSettings::get().traffic_maximumVehicles);

	const double freightWeight = snapshot.industrialBuildings * GenerationSettings::get().traffic_industrialFreightWeight + snapshot.commercialBuildings;
	const double residentWeight = Max(1, snapshot.residentialBuildings);
	const double freightShare = Clamp(freightWeight / (freightWeight + residentWeight * GenerationSettings::get().traffic_residentFreightWeight), GenerationSettings::get().traffic_minimumFreightShare, GenerationSettings::get().traffic_maximumFreightShare);
	demand.largeTruckShare = freightShare * GenerationSettings::get().traffic_largeTruckFreightShare;
	demand.smallTruckShare = freightShare * GenerationSettings::get().traffic_smallTruckFreightShare;
	demand.passengerShare = 1.0 - freightShare;
	return demand;
}

MonthlyCityOutcome calculateMonthlyCityOutcome(int population, double currentHappiness,
	const CitySnapshot& snapshot)
{
	MonthlyCityOutcome outcome;
	const double commuteMinutes = (snapshot.traffic.averageCommuteMinutes > 0.0)
		? snapshot.traffic.averageCommuteMinutes : 20.0;
	outcome.commuteQuality = commuteQuality(commuteMinutes);

	const double parkCoverage = Clamp(
		static_cast<double>(snapshot.parkBuildings) / Max(1, snapshot.residentialBuildings / 20), 0.0, 1.0);
	const double commerceCoverage = Clamp(
		static_cast<double>(snapshot.commercialBuildings) / Max(1, snapshot.residentialBuildings / 8), 0.0, 1.0);
	const double transportCoverage = Clamp(
		static_cast<double>(snapshot.publicTransportStops) / Max(1, snapshot.residentialBuildings / 30), 0.0, 1.0);
	const double targetHappiness = Clamp(0.35 + outcome.commuteQuality * 0.30
		+ parkCoverage * 0.10 + commerceCoverage * 0.10 + transportCoverage * 0.10
		- snapshot.traffic.averageCongestion * 0.15, 0.0, 1.0);
	outcome.happiness = Clamp(currentHappiness * 0.75 + targetHappiness * 0.25, 0.0, 1.0);

	const double occupancy = (snapshot.housingCapacity > 0)
		? static_cast<double>(population) / snapshot.housingCapacity : 2.0;
	double housingMultiplier = 1.0;
	if (occupancy > 0.80) housingMultiplier = 0.50;
	else if (occupancy < 0.50) housingMultiplier = 1.30;

	double happinessMultiplier = Clamp(0.8 + outcome.happiness * 0.4, 0.8, 1.2);
	if (outcome.happiness < 0.30) happinessMultiplier = -0.30;
	else if (outcome.happiness < 0.50) happinessMultiplier = 0.50;

	const double delta = Max(0, population) * 0.003
		* commuteGrowthMultiplier(commuteMinutes) * housingMultiplier * happinessMultiplier;
	outcome.populationDelta = static_cast<int>(Math::Round(delta));
	return outcome;
}

void HousingCapacityCache::update(const World& world, int chunkBudget)
{
	for (int index = 0; index < Min(chunkBudget,WORLD_CHUNKS*WORLD_CHUNKS); ++index)
	{
		const Point coord{m_cursor%WORLD_CHUNKS,m_cursor/WORLD_CHUNKS};
		int64 count = 0;
		if (const auto* chunk = world.getChunk(coord))
		{
			for (const auto& building : chunk->buildingGrid) { count += buildingCapacity(building.type); }
		}
		m_capacity += count-m_chunks[m_cursor];
		m_chunks[m_cursor] = count;
		m_cursor = (m_cursor+1)%(WORLD_CHUNKS*WORLD_CHUNKS);
		if (m_cursor == 0) { m_initialized = true; }
	}
}
