#include "../gen/GenerationSettings.hpp"
#include "VehicleManager.hpp"
#include "TrafficSpawn.hpp"

bool VehicleManager::tryAutomaticSpawn(int edgeId, const SimGraph& graph,
	VehicleType type, const RoadNetwork* network)
{
	const auto* edge = graph.getEdge(edgeId);
	if (!edge || edge->length < 24) { return false; }
	if (type == VehicleType::LargeTruck && edge->roadType == RoadType::LocalRoad) { return false; }
	const auto* road = network ? network->getEdge(edgeId) : nullptr;
	if (network && !road)
	{
		return false;
	}
	const double halfLength = TrafficSpawn::vehicleLength(type)*.5;
	const float start = static_cast<float>((road ? road->cutoffA : 8)+halfLength+4);
	const float finish = static_cast<float>(edge->length-(road ? road->cutoffB : 8)-halfLength-4);
	if (finish <= start) { return false; }
	Array<int> lanes;
	for (int lane = 0; lane < static_cast<int>(edge->lanes.size()); ++lane)
	{
		if (TrafficSpawn::laneOpen(*edge, lane)) { lanes << lane; }
	}
	if (lanes.isEmpty()) { return false; }
	const auto& access = m_buildingAccess.onEdge(edgeId);
	if (m_world && access.isEmpty()) { return false; }
	for (int attempt = 0; attempt < 5; ++attempt)
	{
		Optional<BuildingAccessPoint> origin;
		if (m_world) { origin = access[Random<size_t>(0, access.size() - 1)]; }
		const int lane = origin ? origin->lane : lanes[Random(0, static_cast<int>(lanes.size()) - 1)];
		const float arc = origin ? origin->arc : Random(start, finish);
		if (!TrafficSpawn::laneOpen(*edge, lane) || arc < start || arc > finish) { continue; }
		if (!m_laneTraffic.hasSpace(edgeId, lane, arc, type)) { continue; }
		Vehicle vehicle;
		vehicle.id = m_nextId++;
		vehicle.currentEdge = edgeId;
		vehicle.currentLane = lane;
		vehicle.arcPos = arc;
		vehicle.type = type;
		vehicle.originBuilding = origin ? origin->buildingKey : -1;
		vehicle.speed = origin ? 0.0f : Min(5.0f, edge->speedLimit/3.6f);
		vehicle.departedAt = m_lastGameNow;
		vehicle.populationManaged = true;
		if (!assignBuildingGoal(vehicle, graph)) { continue; }
		m_laneTraffic.reserve(vehicle, lane);
		m_vehicleIndices[vehicle.id] = m_vehicles.size();
		m_vehicles << std::move(vehicle);
		++m_populationStats.spawned;
		return true;
	}
	return false;
}


bool VehicleManager::assignBuildingGoal(Vehicle& vehicle, const SimGraph& graph)
{
	if (!m_world)
	{
		vehicle.goalEdgeId = TrafficSpawn::reachableGoal(graph, vehicle.currentEdge, vehicle.currentLane);
		return vehicle.goalEdgeId >= 0;
	}
	// Follow directed connections to select a reachable building, then request the shortest
	// route to its curb lane. Sparse mountain roads may require more steps than city blocks.
	int edgeId = vehicle.currentEdge, lane = vehicle.currentLane;
	const int desiredSteps = Random(GenerationSettings::get().traffic_minimumTripSteps, GenerationSettings::get().traffic_maximumTripSteps);
	Optional<BuildingAccessPoint> destination;
	const int kMaximumSteps = GenerationSettings::get().traffic_tripSearchSteps;
	for (int step = 0; step < kMaximumSteps; ++step)
	{
		const auto* edge = graph.getEdge(edgeId);
		if (!edge || !TrafficSpawn::laneOpen(*edge, lane)) { break; }
		const int exit = TrafficCommon::isForwardLane(*edge, lane) ? edge->nodeB : edge->nodeA;
		const auto* node = graph.getNode(exit);
		if (!node) { break; }
		Array<std::pair<int, int>> next;
		for (const auto& connection : node->laneConnections)
		{
			if (connection.fromEdgeId != edgeId || connection.fromLaneIndex != lane) { continue; }
			const auto* target = graph.getEdge(connection.toEdgeId);
			if (target && TrafficSpawn::laneOpen(*target, connection.toLaneIndex))
			{
				next << std::pair<int, int>{target->id, connection.toLaneIndex};
			}
		}
		if (next.isEmpty()) { break; }
		const auto selected = next[Random<size_t>(0, next.size() - 1)];
		edgeId = selected.first; lane = selected.second;
		if (edgeId == vehicle.currentEdge) { continue; }
		const auto& points = m_buildingAccess.onEdge(edgeId);
		Array<size_t> candidates;
		for (size_t i = 0; i < points.size(); ++i)
		{
			if (points[i].lane == lane && points[i].buildingKey != vehicle.originBuilding) { candidates << i; }
		}
		if (!candidates.isEmpty())
		{
			destination = points[candidates[Random<size_t>(0, candidates.size() - 1)]];
			if (step >= desiredSteps) { break; }
		}
	}
	if (!destination) { return false; }
	vehicle.goalEdgeId = destination->edgeId; vehicle.goalLane = destination->lane;
	vehicle.goalArc = destination->arc; vehicle.destinationBuilding = destination->buildingKey;
	return true;
}

void VehicleManager::replenishTraffic(double dt, const SimGraph& graph,
	const RoadNetwork& network, const HashSet<int>& visibleEdges)
{
	if (dt <= 0) { return; }
	const double kBurst = GenerationSettings::get().traffic_burst, kSpawnRate = GenerationSettings::get().traffic_spawnRate, kRefreshSeconds = GenerationSettings::get().traffic_refreshSeconds;
	const int kMaximumVehicles = GenerationSettings::get().traffic_maximumVehicles, kAttempts = GenerationSettings::get().traffic_attemptsPerUpdate, kLocalMaximum = GenerationSettings::get().traffic_localMaximum;
	m_spawnCredit = Min(kBurst, m_spawnCredit + dt * kSpawnRate);
	if (m_world) { m_buildingAccess.refresh(*m_world, network, graph); }
	m_spawnRefresh -= dt;
	if (m_spawnRefresh <= 0)
	{
		m_spawnRefresh = kRefreshSeconds;
		m_localSpawnEdges.clear(); m_globalSpawnEdges.clear(); m_localSpawnSet.clear();
		size_t nearbyBuildings = 0;
		for (const auto& [id, edge] : graph.edges)
		{
			if (!edge.isRoadbedBuilt() || edge.length < 24) { continue; }
			const auto& points = m_buildingAccess.onEdge(id);
			if (m_world && points.isEmpty()) { continue; }
			m_globalSpawnEdges << id;
			bool nearby = visibleEdges.contains(id);
			if (m_trafficFocus)
			{
				if (!points.isEmpty()) { nearby = points.front().position.distanceFromSq(*m_trafficFocus) < GenerationSettings::get().traffic_focusRadius * GenerationSettings::get().traffic_focusRadius; }
				else if (const auto curve = network.getBezier(id))
				{
					const auto point = curve->positionAt(edge.length * .5f);
					nearby = Vec2{point.x, point.z}.distanceFromSq(*m_trafficFocus) < GenerationSettings::get().traffic_focusRadius * GenerationSettings::get().traffic_focusRadius;
				}
			}
			if (nearby)
			{
				m_localSpawnEdges << id; m_localSpawnSet.insert(id);
				nearbyBuildings += m_world ? points.size() : 2;
			}
		}
		m_localTrafficTarget = Min(m_targetVehicleCount, Min(kLocalMaximum, static_cast<int>(nearbyBuildings * GenerationSettings::get().traffic_localCarsPerBuilding)));
	}
	int localCount = 0, totalCount = 0;
	for (const auto& vehicle : m_vehicles)
	{
		if (vehicle.currentEdge < 0 || vehicle.tripCompleted || vehicle.type == VehicleType::Bus || vehicle.type == VehicleType::Emergency) { continue; }
		++totalCount; localCount += m_localSpawnSet.contains(vehicle.currentEdge);
	}
	for (int attempt = 0; attempt < kAttempts && m_spawnCredit >= 1 && totalCount < Min(kMaximumVehicles, m_targetVehicleCount); ++attempt)
	{
		const bool local = localCount < m_localTrafficTarget && !m_localSpawnEdges.isEmpty();
		const auto& choices = local ? m_localSpawnEdges : m_globalSpawnEdges;
		if (choices.isEmpty()) { break; }
		const int id = choices[Random<size_t>(0, choices.size() - 1)];
		if (tryAutomaticSpawn(id, graph, selectDemandVehicleType(), &network))
		{
			m_spawnCredit -= 1; ++totalCount; localCount += m_localSpawnSet.contains(id);
		}
	}
	m_populationStats.localVehicles = localCount;
	m_populationStats.localTarget = m_localTrafficTarget;
}
