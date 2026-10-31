#include "../gen/GenerationSettings.hpp"
#include "VehicleManager.hpp"
#include "TrafficSpawn.hpp"
#include "../debug/DebugLog.hpp"

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


bool VehicleManager::trySpawnThroughTraffic(int edgeId, const SimGraph& graph, VehicleType type, const RoadNetwork& network)
{
	const auto* edge = graph.getEdge(edgeId);
	const auto* road = network.getEdge(edgeId);
	if (!edge || !road || edge->roadType != RoadType::Expressway) { return false; }
	const double halfLength = TrafficSpawn::vehicleLength(type) * .5;
	const float start = static_cast<float>(road->cutoffA + halfLength + 4), finish = static_cast<float>(edge->length - road->cutoffB - halfLength - 4);
	if (finish <= start) { return false; }
	const int lane = Random(0, static_cast<int>(edge->lanes.size()) - 1);
	const float arc = Random(start, finish);
	if (!TrafficSpawn::laneOpen(*edge, lane) || !m_laneTraffic.hasSpace(edgeId, lane, arc, type)) { return false; }
	// 本線と IC・JCT の連絡路だけを接続順にたどり、数 km 先の本線区間を目的地にする。
	int current = edgeId, currentLane = lane, goal = -1, goalLane = -1;
	const int steps = Random(GenerationSettings::get().expressway_throughTripSteps / 2, GenerationSettings::get().expressway_throughTripSteps);
	for (int step = 0; step < steps; ++step)
	{
		const auto* from = graph.getEdge(current);
		const auto* node = from ? graph.getNode(TrafficCommon::isForwardLane(*from, currentLane) ? from->nodeB : from->nodeA) : nullptr;
		if (!node) { break; }
		Array<std::pair<int, int>> next;
		for (const auto& connection : node->laneConnections)
		{
			if (connection.fromEdgeId != current || connection.fromLaneIndex != currentLane) { continue; }
			const auto* target = graph.getEdge(connection.toEdgeId);
			if (target && target->roadType == RoadType::Expressway && TrafficSpawn::laneOpen(*target, connection.toLaneIndex)) { next << std::pair{ target->id, connection.toLaneIndex }; }
		}
		if (next.isEmpty()) { break; }
		const auto selected = next[Random<size_t>(0, next.size() - 1)];
		current = selected.first; currentLane = selected.second;
		if (graph.getEdge(current)->lanes.size() >= 2) { goal = current; goalLane = currentLane; }
	}
	if (goal < 0 || goal == edgeId) { return false; }
	Vehicle vehicle;
	vehicle.id = m_nextId++;
	vehicle.currentEdge = edgeId; vehicle.currentLane = lane; vehicle.arcPos = arc; vehicle.type = type;
	vehicle.speed = static_cast<float>(edge->speedLimit / 3.6 * .85);
	vehicle.departedAt = m_lastGameNow;
	vehicle.populationManaged = true;
	vehicle.goalEdgeId = goal; vehicle.goalLane = goalLane; vehicle.goalArc = static_cast<float>(graph.getEdge(goal)->length * .5);
	m_laneTraffic.reserve(vehicle, lane);
	m_vehicleIndices[vehicle.id] = m_vehicles.size();
	m_vehicles << std::move(vehicle);
	++m_populationStats.spawned;
	return true;
}

namespace
{
	/// @brief 地図端の節点から、車が出入りする位置までの弧長。
	constexpr float kOutsideEndClearance = 12.0f;
}

bool VehicleManager::trySpawnInbound(const OutsideConnections::Connection& connection, const SimGraph& graph, VehicleType type)
{
	const auto* edge = graph.getEdge(connection.edgeId);
	if (!edge || connection.inboundLanes.isEmpty() || m_globalSpawnEdges.isEmpty()) { return false; }
	if (type == VehicleType::LargeTruck && edge->roadType == RoadType::LocalRoad) { type = VehicleType::SmallTruck; }
	const int lane = connection.inboundLanes[Random<size_t>(0, connection.inboundLanes.size() - 1)];
	const float offset = kOutsideEndClearance + static_cast<float>(TrafficSpawn::vehicleLength(type) * .5);
	if (edge->length < offset * 2 + 8) { return false; }
	const float arc = connection.atNodeA ? offset : edge->length - offset;
	if (!TrafficSpawn::laneOpen(*edge, lane) || !m_laneTraffic.hasSpace(edge->id, lane, arc, type)) { return false; }
	Vehicle vehicle;
	vehicle.id = m_nextId++;
	vehicle.currentEdge = edge->id; vehicle.currentLane = lane; vehicle.arcPos = arc; vehicle.type = type;
	vehicle.speed = edge->speedLimit / 3.6f * .7f;
	vehicle.departedAt = m_lastGameNow;
	vehicle.outsideTrip = 1;
	// 地図外から来た車は街のどこかの建物へ向かう。経路は走行グラフ全体の最短経路で求める。
	if (m_world)
	{
		const int target = m_globalSpawnEdges[Random<size_t>(0, m_globalSpawnEdges.size() - 1)];
		const auto& points = m_buildingAccess.onEdge(target);
		if (points.isEmpty()) { return false; }
		const auto& destination = points[Random<size_t>(0, points.size() - 1)];
		vehicle.goalEdgeId = destination.edgeId; vehicle.goalLane = destination.lane;
		vehicle.goalArc = destination.arc; vehicle.destinationBuilding = destination.buildingKey;
	}
	else
	{
		vehicle.goalEdgeId = TrafficSpawn::reachableGoal(graph, vehicle.currentEdge, vehicle.currentLane);
		if (vehicle.goalEdgeId < 0) { return false; }
	}
	m_laneTraffic.reserve(vehicle, lane);
	m_vehicleIndices[vehicle.id] = m_vehicles.size();
	m_vehicles << std::move(vehicle);
	++m_populationStats.spawned;
	return true;
}

bool VehicleManager::trySpawnOutbound(const OutsideConnections::Connection& connection, const SimGraph& graph, VehicleType type)
{
	const auto* exit = graph.getEdge(connection.edgeId);
	if (!exit || connection.outboundLanes.isEmpty() || m_globalSpawnEdges.isEmpty() || exit->length < kOutsideEndClearance * 2 + 8) { return false; }
	const int originEdge = m_globalSpawnEdges[Random<size_t>(0, m_globalSpawnEdges.size() - 1)];
	const auto* edge = graph.getEdge(originEdge);
	if (!edge) { return false; }
	if (type == VehicleType::LargeTruck && edge->roadType == RoadType::LocalRoad) { type = VehicleType::SmallTruck; }
	// 建物の前から出発する（建物の索引がない単体試験では区間の中ほどから）。
	Optional<BuildingAccessPoint> origin;
	int lane = -1; float arc = edge->length * .5f;
	if (m_world)
	{
		const auto& points = m_buildingAccess.onEdge(originEdge);
		if (points.isEmpty()) { return false; }
		origin = points[Random<size_t>(0, points.size() - 1)];
		lane = origin->lane; arc = origin->arc;
	}
	else
	{
		for (int candidate = 0; candidate < static_cast<int>(edge->lanes.size()) && lane < 0; ++candidate) { if (TrafficSpawn::laneOpen(*edge, candidate)) { lane = candidate; } }
	}
	if (lane < 0 || !TrafficSpawn::laneOpen(*edge, lane) || !m_laneTraffic.hasSpace(originEdge, lane, arc, type)) { return false; }
	Vehicle vehicle;
	vehicle.id = m_nextId++;
	vehicle.currentEdge = originEdge; vehicle.currentLane = lane; vehicle.arcPos = arc; vehicle.type = type;
	vehicle.originBuilding = origin ? origin->buildingKey : -1;
	vehicle.speed = origin ? 0.0f : Min(5.0f, edge->speedLimit / 3.6f);
	vehicle.departedAt = m_lastGameNow;
	vehicle.outsideTrip = -1;
	// 目的地は地図端の流出車線。端点の手前で走行を終え、地図外へ出たものとして消える。
	vehicle.goalEdgeId = exit->id;
	vehicle.goalLane = connection.outboundLanes[Random<size_t>(0, connection.outboundLanes.size() - 1)];
	vehicle.goalArc = connection.atNodeA ? kOutsideEndClearance : exit->length - kOutsideEndClearance;
	m_laneTraffic.reserve(vehicle, lane);
	m_vehicleIndices[vehicle.id] = m_vehicles.size();
	m_vehicles << std::move(vehicle);
	++m_populationStats.spawned;
	return true;
}

void VehicleManager::replenishOutsideTraffic(double dt, const SimGraph& graph)
{
	const auto& settings = GenerationSettings::get();
	m_outsideTraffic.inbound = m_outsideTraffic.outbound = 0;
	for (const auto& vehicle : m_vehicles)
	{
		if (vehicle.currentEdge < 0 || vehicle.tripCompleted) { continue; }
		m_outsideTraffic.inbound += vehicle.outsideTrip > 0;
		m_outsideTraffic.outbound += vehicle.outsideTrip < 0;
	}
	// 需要が未設定（単体試験・起動直後）でも、地図外どうしの最低限の往来は出す。
	const double eventMultiplier = m_eventDemandMultiplier;
	m_outsideTraffic.targetInbound = static_cast<int>(Math::Round((m_trafficDemandConfigured ? m_trafficDemand.outsideInbound : settings.traffic_outsideMinimumVehicles / 2) * eventMultiplier));
	m_outsideTraffic.targetOutbound = static_cast<int>(Math::Round((m_trafficDemandConfigured ? m_trafficDemand.outsideOutbound : settings.traffic_outsideMinimumVehicles / 2) * eventMultiplier));
	if (m_outsideConnections.isEmpty() || dt <= 0) { return; }
	m_outsideCredit = Min(settings.traffic_burst, m_outsideCredit + dt * settings.traffic_outsideSpawnRate);
	const int capacity = settings.traffic_maximumVehicles - static_cast<int>(m_vehicles.size());
	for (int attempt = 0; attempt < 16 && m_outsideCredit >= 1 && capacity > attempt; ++attempt)
	{
		// 目標との差に比例した確率で向きを選び、流入と流出を並行して補う。
		const int inboundGap = Max(0, m_outsideTraffic.targetInbound - m_outsideTraffic.inbound);
		const int outboundGap = Max(0, m_outsideTraffic.targetOutbound - m_outsideTraffic.outbound);
		if (inboundGap <= 0 && outboundGap <= 0) { break; }
		const bool inbound = Random(0.0, static_cast<double>(inboundGap + outboundGap)) < inboundGap;
		const auto chosen = OutsideConnections::choose(m_outsideConnections, inbound);
		if (!chosen) { break; }
		const auto& connection = m_outsideConnections[*chosen];
		// 広域の物流は高速道路と幹線に多い。
		const bool freightRoad = connection.roadType == RoadType::Expressway || connection.roadType == RoadType::Arterial;
		const VehicleType type = freightRoad && Random(0, 9) < 2 ? VehicleType::LargeTruck : selectDemandVehicleType();
		m_outsideCredit -= 1;
		if (inbound ? trySpawnInbound(connection, graph, type) : trySpawnOutbound(connection, graph, type))
		{
			(inbound ? m_outsideTraffic.inbound : m_outsideTraffic.outbound) += 1;
		}
		else if (m_outsideFailureLogs < 12)
		{
			++m_outsideFailureLogs;
			const auto* edge = graph.getEdge(connection.edgeId);
			DBG_LOG(U"[OutsideTraffic] spawn failed inbound={} edge={} length={} lanes={}/{}"_fmt(inbound, connection.edgeId, edge ? edge->length : -1.0f, connection.inboundLanes.size(), connection.outboundLanes.size()));
		}
	}
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
		m_localSpawnEdges.clear(); m_globalSpawnEdges.clear(); m_localSpawnSet.clear(); m_throughSpawnEdges.clear(); m_throughSpawnSet.clear();
		OutsideConnections::Diagnostics diagnostics;
		m_outsideConnections = OutsideConnections::find(network, graph, &diagnostics);
		if (m_outsideConnections.size() != m_loggedOutsideConnections)
		{
			m_loggedOutsideConnections = m_outsideConnections.size();
			DBG_LOG(U"[OutsideTraffic] connections={} nearBorder={} branched={} missingEdge={} unbuilt={} short={} closed={}"_fmt(
				m_outsideConnections.size(), diagnostics.nearBorder, diagnostics.branched, diagnostics.missingEdge, diagnostics.unbuilt, diagnostics.short_, diagnostics.closed));
		}
		size_t nearbyBuildings = 0;
		for (const auto& [id, edge] : graph.edges)
		{
			if (!edge.isRoadbedBuilt() || edge.length < 24) { continue; }
			if (edge.roadType == RoadType::Expressway && edge.lanes.size() >= 2 && edge.lanes.size() <= 3 && m_trafficFocus)
			{
				if (const auto curve = network.getBezier(id))
				{
					const auto point = curve->positionAt(edge.length * .5f);
					const double radius = GenerationSettings::get().traffic_focusRadius * 2;
					if (Vec2{ point.x, point.z }.distanceFromSq(*m_trafficFocus) < radius * radius) { m_throughSpawnEdges << id; m_throughSpawnSet.insert(id); }
				}
			}
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
		if (vehicle.currentEdge < 0 || vehicle.tripCompleted || vehicle.outsideTrip != 0 || vehicle.type == VehicleType::Bus || vehicle.type == VehicleType::Emergency) { continue; }
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
	// 高速道路の通過交通: 視点付近の本線 1 区間あたりの目標台数まで補充する。
	int throughCount = 0;
	for (const auto& vehicle : m_vehicles) { throughCount += vehicle.currentEdge >= 0 && !vehicle.tripCompleted && m_throughSpawnSet.contains(vehicle.currentEdge); }
	const int throughTarget = static_cast<int>(m_throughSpawnEdges.size() * GenerationSettings::get().expressway_throughVehiclesPerEdge);
	for (int attempt = 0; attempt < kAttempts && throughCount < throughTarget && !m_throughSpawnEdges.isEmpty(); ++attempt)
	{
		const int id = m_throughSpawnEdges[Random<size_t>(0, m_throughSpawnEdges.size() - 1)];
		const VehicleType type = Random(0, 9) < 2 ? VehicleType::LargeTruck : selectDemandVehicleType();
		if (trySpawnThroughTraffic(id, graph, type, network)) { ++throughCount; }
	}
	replenishOutsideTraffic(dt, graph);
	m_populationStats.localVehicles = localCount;
	m_populationStats.localTarget = m_localTrafficTarget;
}
