#include "TrafficManager.hpp"
#include <cmath>
#include <chrono>

using namespace TrafficCommon;

// ===== update =====

void TrafficManager::update(double dt, GameTime gameNow)
{
	if (!m_simGraph) return;
	m_lastGameNow = gameNow;
	m_simStats = {};

	using Clock = std::chrono::steady_clock;
	auto toMs = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };

	const auto t0 = Clock::now();

	if (m_graphDirty)
	{
		buildTrafficLights();
		rebuildGraph(gameNow);
		m_graphDirty = false;
	}

	const auto t1 = Clock::now();
	m_simStats.graph = toMs(t1 - t0);

	updateTrafficLights(gameNow);

	const auto t2 = Clock::now();
	m_simStats.signal = toMs(t2 - t1);

	updateBusRoutes(gameNow);
	processRerouteQueue(gameNow);

	const auto t3 = Clock::now();
	m_simStats.reroute = toMs(t3 - t2);

	// 車両更新
	double idmTotal = 0, lcTotal = 0;
	for (auto& v : m_vehicles)
	{
		if (v.currentEdge == -1) continue;
		if (!m_simGraph->getEdge(v.currentEdge)) { v.currentEdge = -1; continue; }

		if (v.state == VehicleState::WaitingBusStop)
		{
			v.busWaitRemaining -= static_cast<float>(dt);
			if (v.busWaitRemaining <= 0.0f)
			{
				v.busWaitRemaining = 0.0f;
				v.state = VehicleState::Moving;
				++v.busNextStopIdx;
			}
			continue;
		}

		const auto lcStart = Clock::now();
		if (RandomBool(0.01))
			tryLaneChange(v);
		lcTotal += toMs(Clock::now() - lcStart);

		if (v.type == VehicleType::Bus && v.busRouteId >= 0)
			updateBusStop(v, dt, gameNow);
		if (v.state == VehicleState::WaitingBusStop) continue;

		const auto idmStart = Clock::now();
		advanceOnEdge(v, dt, gameNow);
		idmTotal += toMs(Clock::now() - idmStart);
	}
	m_simStats.idm = idmTotal;
	m_simStats.laneChange = lcTotal;

	m_vehicles.remove_if([](const Vehicle& v) { return v.currentEdge == -1; });

	const auto t4 = Clock::now();
	m_simStats.other = toMs(t4 - t0) - m_simStats.total();
	if (m_simStats.other < 0) m_simStats.other = 0;
}

// ===== addVehicle / spawnVehicle =====

void TrafficManager::addVehicle(Vehicle v)
{
	v.id = m_nextId++;
	enqueueReroute(v.id);
	m_vehicles << std::move(v);
}

void TrafficManager::spawnVehicle(VehicleType type)
{
	if (!m_simGraph) return;

	const auto candidates = collectDrivableEdges(*m_simGraph);
	if (candidates.isEmpty()) return;

	const int edgeId = candidates[Random(0, static_cast<int>(candidates.size()) - 1)];

	Vehicle v;
	v.currentEdge = edgeId;
	v.currentLane = 0;
	v.speed       = 5.0f;
	v.type        = type;

	const auto* edge = m_simGraph->getEdge(edgeId);
	if (edge)
		v.arcPos = static_cast<float>(Random(0.0, static_cast<double>(edge->length) * 0.8));

	addVehicle(std::move(v));
}

// ===== 信号機クエリ =====

const TrafficLight* TrafficManager::getTrafficLight(int nodeId) const
{
	const auto it = m_trafficLights.find(nodeId);
	return (it != m_trafficLights.end()) ? &it->second : nullptr;
}

// ===== グラフ管理 =====

void TrafficManager::rebuildGraph(GameTime now)
{
	if (!m_simGraph) return;
	m_graph.rebuild(*m_simGraph, now, m_trafficLights);

	for (const auto& v : m_vehicles)
		enqueueReroute(v.id);
}

// ===== 車両更新 =====

void TrafficManager::updateVehicle(Vehicle& v, double dt, GameTime gameNow)
{
	if (v.currentEdge == -1) return;
	if (!m_simGraph->getEdge(v.currentEdge)) { v.currentEdge = -1; return; }

	if (v.state == VehicleState::WaitingBusStop)
	{
		v.busWaitRemaining -= static_cast<float>(dt);
		if (v.busWaitRemaining <= 0.0f)
		{
			v.busWaitRemaining = 0.0f;
			v.state = VehicleState::Moving;
			++v.busNextStopIdx;
		}
		return;
	}

	if (RandomBool(0.01))
		tryLaneChange(v);

	if (v.type == VehicleType::Bus && v.busRouteId >= 0)
		updateBusStop(v, dt, gameNow);
	if (v.state == VehicleState::WaitingBusStop) return;

	advanceOnEdge(v, dt, gameNow);
}

void TrafficManager::advanceOnEdge(Vehicle& v, double dt, [[maybe_unused]] GameTime gameNow)
{
	const auto* edge = m_simGraph->getEdge(v.currentEdge);
	if (!edge) return;

	const bool fwdLane = isForwardLane(*edge, v.currentLane);
	const IDMParams params = getDefaultIDMParams(v.type, edge->speedLimit);
	float accel = idmAcceleration(m_vehicles, v, params, fwdLane);

	// 信号停止チェック
	const int exitNId = fwdLane ? edge->nodeB : edge->nodeA;
	const TrafficLight* tl = getTrafficLight(exitNId);
	if (tl && !tl->isGreen(v.currentEdge))
	{
		const float distToStop = fwdLane
			? (edge->length - 2.0f - v.arcPos)
			: (v.arcPos - 2.0f);

		if (const auto a = stopLineAccel(v.speed, distToStop, kSignalStopDist, params))
			accel = *a;
	}

	v.speed = static_cast<float>(Clamp(
		static_cast<double>(v.speed) + accel * dt,
		0.0, static_cast<double>(params.v0)));

	const float advance = v.speed * static_cast<float>(dt);
	if (fwdLane)
		v.arcPos += advance;
	else
		v.arcPos -= advance;

	const bool reachedEnd = fwdLane
		? (v.arcPos >= edge->length)
		: (v.arcPos <= 0.0f);

	if (reachedEnd)
		transitToNextEdge(v, gameNow);
}

bool TrafficManager::transitToNextEdge(Vehicle& v, [[maybe_unused]] GameTime gameNow)
{
	while (v.routeProgress < static_cast<int>(v.routeNodeIds.size()))
	{
		const int nodeId = v.routeNodeIds[v.routeProgress++];
		const LaneNode* ln = m_graph.getLaneNode(nodeId);
		if (!ln) continue;

		if (ln->edgeId != v.currentEdge)
		{
			v.currentEdge = ln->edgeId;
			v.currentLane = ln->laneIndex;
			v.arcPos      = ln->arcPos;
			v.speed      *= 0.8f;
			return true;
		}
	}

	v.goalEdgeId = -1;
	enqueueReroute(v.id);

	// フォールバック: ランダムに次エッジを選ぶ
	const auto* edge = m_simGraph->getEdge(v.currentEdge);
	if (!edge) { v.currentEdge = -1; return false; }

	const int exitNId = isForwardLane(*edge, v.currentLane) ? edge->nodeB : edge->nodeA;
	const auto* exitNode = m_simGraph->getNode(exitNId);
	if (!exitNode) { v.currentEdge = -1; return false; }

	Array<int> candidates;
	for (const int eid : exitNode->edgeIds)
	{
		if (eid != v.currentEdge)
			candidates << eid;
	}

	if (candidates.isEmpty())
	{
		v.currentEdge = -1;
		return false;
	}

	v.currentEdge = candidates[Random(0, static_cast<int>(candidates.size()) - 1)];
	v.arcPos      = 0.0f;
	v.speed      *= 0.8f;
	return true;
}

// ===== 経路探索 =====

void TrafficManager::doReroute(Vehicle& v, GameTime now)
{
	if (v.goalEdgeId == -1 || v.goalEdgeId == v.currentEdge)
	{
		const int goal = selectRandomGoalEdge(*m_simGraph, v.currentEdge);
		if (goal == -1) return;
		v.goalEdgeId = goal;
	}

	int startNode = m_graph.entryNodeId(v.currentEdge, v.currentLane);

	if (startNode == -1)
	{
		const auto* e = m_simGraph->getEdge(v.currentEdge);
		if (e)
		{
			for (int i = 0; i < static_cast<int>(e->lanes.size()); ++i)
			{
				startNode = m_graph.entryNodeId(v.currentEdge, i);
				if (startNode != -1) { v.currentLane = i; break; }
			}
		}
	}
	if (startNode == -1) return;

	using Clock = std::chrono::steady_clock;
	const auto tStart = Clock::now();

	const PathResult result = m_graph.dijkstra(startNode, v.goalEdgeId);

	const double ms = std::chrono::duration<double, std::milli>(Clock::now() - tStart).count();
	if (ms > 1.0)
	{
		Console << U"[Reroute] vid={} edge={} goal={} | {:.1f}ms | visited={}/{} | {}"_fmt(
			v.id, v.currentEdge, v.goalEdgeId,
			ms, result.nodesVisited, result.graphSize,
			result.found ? U"FOUND" : U"NOT_FOUND");
	}

	if (result.found)
	{
		v.routeNodeIds  = result.nodeIds;
		v.routeProgress = 1;
	}

	v.lastReroute = now;
}

void TrafficManager::processRerouteQueue(GameTime now)
{
	int processed = 0;
	Array<int> remaining;

	for (const int vid : m_rerouteQueue)
	{
		if (processed >= kReroutePerFrame)
		{
			remaining << vid;
			continue;
		}

		for (auto& v : m_vehicles)
		{
			if (v.id == vid)
			{
				doReroute(v, now);
				break;
			}
		}
		++processed;
	}

	m_rerouteQueue = std::move(remaining);
}

void TrafficManager::enqueueReroute(int vehicleId)
{
	if (!m_rerouteQueue.contains(vehicleId))
		m_rerouteQueue << vehicleId;
}

// ===== 信号機 =====

void TrafficManager::buildTrafficLights()
{
	m_trafficLights.clear();
	if (!m_simGraph) return;

	for (const auto& [nid, node] : m_simGraph->nodes)
	{
		Array<int> validEdges;
		for (const int eid : node.edgeIds)
		{
			if (m_simGraph->getEdge(eid)) validEdges << eid;
		}
		if (validEdges.size() < 3) continue;

		auto phases = buildTwoGroupPhases(validEdges);
		m_trafficLights.emplace(node.id, TrafficLight{ node.id, std::move(phases) });
	}
}

void TrafficManager::updateTrafficLights(GameTime gameNow)
{
	for (auto& kv : m_trafficLights)
		kv.second.update(gameNow);
}

// ===== 車線変更 =====

void TrafficManager::tryLaneChange(Vehicle& v)
{
	if (v.currentEdge == -1) return;
	const auto* edge = m_simGraph->getEdge(v.currentEdge);
	if (!edge) return;

	const bool fwdLane = isForwardLane(*edge, v.currentLane);
	const IDMParams params = getDefaultIDMParams(v.type, edge->speedLimit);

	const float distToExit = fwdLane ? (edge->length - v.arcPos) : v.arcPos;
	if (distToExit < kLaneChangeMinExitDist) return;

	// 現在車線の前方ギャップ
	float frontGapCurrent = 1e9f;
	for (const auto& other : m_vehicles)
	{
		if (other.id == v.id) continue;
		if (other.currentEdge != v.currentEdge || other.currentLane != v.currentLane) continue;
		const float delta = fwdLane ? (other.arcPos - v.arcPos) : (v.arcPos - other.arcPos);
		if (delta > 0.0f && delta < frontGapCurrent)
			frontGapCurrent = delta;
	}

	auto tryTarget = [&](int targetLane) -> bool
	{
		float frontGap, rearGap;
		measureGaps(m_vehicles, v.id, v.currentEdge, targetLane,
		            v.arcPos, fwdLane, false, frontGap, rearGap);
		if (!isLaneChangeSafe(*edge, targetLane, fwdLane, frontGap, rearGap, params))
			return false;
		v.currentLane = targetLane;
		return true;
	};

	if (v.currentLane > 0 && tryTarget(v.currentLane - 1))
		return;

	if (frontGapCurrent < params.s0 * 4.0f)
		tryTarget(v.currentLane + 1);
}

// ===== バス路線 =====

int TrafficManager::addBusStop(BusStop stop)
{
	stop.id = m_nextStopId++;
	m_busStops << std::move(stop);
	return m_busStops.back().id;
}

int TrafficManager::addBusRoute(BusRoute route)
{
	route.id = m_nextRouteId++;
	m_busRoutes << std::move(route);
	return m_busRoutes.back().id;
}

void TrafficManager::addStopToRoute(int routeId, int stopId)
{
	for (auto& route : m_busRoutes)
	{
		if (route.id == routeId)
		{
			route.stopIds << stopId;
			return;
		}
	}
}

void TrafficManager::updateBusRoutes(GameTime gameNow)
{
	for (auto& route : m_busRoutes)
	{
		if (route.stopIds.size() < 2) continue;
		if (gameNow - route.lastSpawnAt < route.headwaySec) continue;

		const int firstStopId = route.stopIds[0];
		const BusStop* stop = nullptr;
		for (const auto& s : m_busStops)
		{
			if (s.id == firstStopId) { stop = &s; break; }
		}
		if (!stop) continue;

		if (stop->edgeId >= 0)
		{
			Vehicle bus;
			bus.type         = VehicleType::Bus;
			bus.currentEdge  = stop->edgeId;
			bus.arcPos       = stop->arcPos;
			bus.speed        = 0.0f;
			bus.busRouteId   = route.id;
			bus.busNextStopIdx = 1;
			addVehicle(std::move(bus));
		}
		route.lastSpawnAt = gameNow;
	}
}

void TrafficManager::updateBusStop(Vehicle& v, [[maybe_unused]] double dt, [[maybe_unused]] GameTime gameNow)
{
	if (v.busRouteId < 0) return;

	BusRoute* route = nullptr;
	for (auto& r : m_busRoutes)
	{
		if (r.id == v.busRouteId) { route = &r; break; }
	}
	if (!route || route->stopIds.isEmpty()) return;

	if (v.busNextStopIdx >= static_cast<int>(route->stopIds.size()))
	{
		v.currentEdge = -1;
		return;
	}

	const int nextStopId = route->stopIds[v.busNextStopIdx];
	const BusStop* nextStop = nullptr;
	for (const auto& s : m_busStops)
	{
		if (s.id == nextStopId) { nextStop = &s; break; }
	}
	if (!nextStop) return;

	if (v.currentEdge == nextStop->edgeId)
	{
		const float dist = Abs(v.arcPos - nextStop->arcPos);
		if (dist < 8.0f && v.speed < 2.0f)
		{
			v.state            = VehicleState::WaitingBusStop;
			v.speed            = 0.0f;
			v.busWaitRemaining = 5.0f;
			v.arcPos           = nextStop->arcPos;
		}
	}
}
