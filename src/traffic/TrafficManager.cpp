#include "TrafficManager.hpp"
#include <cmath>

// ===== update =====

void TrafficManager::update(double dt, GameTime gameNow)
{
	if (!m_simGraph) return;
	m_lastGameNow = gameNow;

	// グラフが更新された場合は再構築する
	if (m_graphDirty)
	{
		buildTrafficLights();
		rebuildGraph(gameNow);
		m_graphDirty = false;
	}

	updateTrafficLights(gameNow);
	updateBusRoutes(gameNow);
	processRerouteQueue(gameNow);

	for (auto& v : m_vehicles)
		updateVehicle(v, dt, gameNow);

	m_vehicles.remove_if([](const Vehicle& v) { return v.currentEdge == -1; });
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

	// Forward 方向の走行可能な車線があるエッジを候補として収集する
	Array<int> candidates;
	for (const auto& [eid, e] : m_simGraph->edges)
	{
		if (!e.isRoadbedBuilt()) continue;
		for (const auto& lane : e.lanes)
		{
			if ((lane.op == OpState::Open || lane.op == OpState::Provisional) && lane.dir == LaneDir::Forward)
			{
				candidates << e.id;
				break;
			}
		}
	}
	if (candidates.isEmpty()) return;

	const int edgeId = candidates[Random(0, static_cast<int>(candidates.size()) - 1)];

	Vehicle v;
	v.currentEdge = edgeId;
	v.currentLane = 0;
	v.speed       = 5.0f;
	v.type        = type;

	// 初期位置をランダムに設定する
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

// ===== ヘルパー =====

static bool isForwardLane(const SimGraph::Edge& edge, int laneIdx)
{
	if (laneIdx < 0 || laneIdx >= static_cast<int>(edge.lanes.size()))
		return true;
	return edge.lanes[laneIdx].dir == LaneDir::Forward;
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

	if (v.speed < v.rerouteSpeedThreshold)
	{
		if (RandomBool(0.005))
			enqueueReroute(v.id);
	}

	if (static_cast<float>(gameNow - v.lastReroute) > kPeriodicRerouteInterval)
		enqueueReroute(v.id);
}

void TrafficManager::advanceOnEdge(Vehicle& v, double dt, GameTime gameNow)
{
	const auto* edge = m_simGraph->getEdge(v.currentEdge);
	if (!edge) return;

	const bool fwdLane = isForwardLane(*edge, v.currentLane);

	const IDMParams params = getDefaultIDMParams(v.type, edge->speedLimit);
	float accel = idmAcceleration(v, params, fwdLane);

	// 信号停止チェック
	const int exitNId = fwdLane ? edge->nodeB : edge->nodeA;
	const TrafficLight* tl = getTrafficLight(exitNId);
	if (tl && !tl->isGreen(v.currentEdge))
	{
		const float distToStop = fwdLane
			? (edge->length - 2.0f - v.arcPos)
			: (v.arcPos - 2.0f);

		if (distToStop > 0.0f && distToStop < kSignalStopDist)
		{
			const float dv    = v.speed;
			const float sStar = params.s0 + Max(0.0f,
				v.speed * params.T + v.speed * dv / (2.0f * std::sqrtf(params.aMax * params.b)));
			const float gap = Max(0.1f, distToStop);
			accel = params.aMax * (
				1.0f - std::powf(v.speed / Max(0.1f, params.v0), 4.0f)
				- (sStar / gap) * (sStar / gap));
		}
	}

	v.speed = static_cast<float>(Clamp(
		static_cast<double>(v.speed) + accel * dt,
		0.0, static_cast<double>(params.v0)));

	const float advance = v.speed * static_cast<float>(dt);
	if (fwdLane)
		v.arcPos += advance;
	else
		v.arcPos -= advance;

	// 注: position/heading はメインスレッドで描画時にベジェ変換する

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

// ===== IDM =====

float TrafficManager::idmAcceleration(const Vehicle& v, const IDMParams& params, bool fwdLane) const
{
	float gap   = 1e9f;
	float vLead = params.v0;

	for (const auto& other : m_vehicles)
	{
		if (other.id == v.id)           continue;
		if (other.currentEdge != v.currentEdge) continue;
		if (other.currentLane != v.currentLane) continue;

		const float delta = fwdLane
			? (other.arcPos - v.arcPos)
			: (v.arcPos - other.arcPos);

		if (delta > 0.0f && delta < gap)
		{
			gap   = delta;
			vLead = other.speed;
		}
	}

	if (gap > 500.0f)
	{
		return params.aMax * (1.0f - std::powf(v.speed / Max(0.1f, params.v0), 4.0f));
	}

	const float dv    = v.speed - vLead;
	const float sStar = params.s0 + Max(0.0f,
		v.speed * params.T + v.speed * dv / (2.0f * std::sqrtf(params.aMax * params.b)));
	const float safeGap = Max(0.1f, gap);

	return params.aMax * (
		1.0f - std::powf(v.speed / Max(0.1f, params.v0), 4.0f)
		- (sStar / safeGap) * (sStar / safeGap));
}

// ===== 経路探索 =====

void TrafficManager::doReroute(Vehicle& v, GameTime now)
{
	if (v.goalEdgeId == -1 || v.goalEdgeId == v.currentEdge)
	{
		const auto ids = m_simGraph->edgeIds();
		Array<int> candidates;
		for (const int eid : ids)
		{
			if (eid != v.currentEdge)
				candidates << eid;
		}
		if (candidates.isEmpty()) return;
		v.goalEdgeId = candidates[Random(0, static_cast<int>(candidates.size()) - 1)];
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

	const PathResult result = m_graph.dijkstra(startNode, v.goalEdgeId);
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
		int validEdges = 0;
		for (const int eid : node.edgeIds)
		{
			if (m_simGraph->getEdge(eid)) ++validEdges;
		}
		if (validEdges < 3) continue;

		const int half = static_cast<int>(node.edgeIds.size()) / 2;
		Array<SignalPhase> phases;

		SignalPhase phaseA;
		phaseA.duration = 30.0f;
		SignalPhase phaseB;
		phaseB.duration = 30.0f;

		for (int k = 0; k < static_cast<int>(node.edgeIds.size()); ++k)
		{
			if (k < half)
				phaseA.greenEdgeIds << node.edgeIds[k];
			else
				phaseB.greenEdgeIds << node.edgeIds[k];
		}

		phases << std::move(phaseA) << std::move(phaseB);

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
	if (distToExit < 30.0f) return;

	float frontGapCurrent = 1e9f;
	for (const auto& other : m_vehicles)
	{
		if (other.id == v.id) continue;
		if (other.currentEdge != v.currentEdge || other.currentLane != v.currentLane) continue;
		const float delta = fwdLane ? (other.arcPos - v.arcPos) : (v.arcPos - other.arcPos);
		if (delta > 0.0f && delta < frontGapCurrent)
			frontGapCurrent = delta;
	}

	const auto isSafe = [&](int targetLane) -> bool
	{
		if (targetLane < 0 || targetLane >= static_cast<int>(edge->lanes.size())) return false;
		const Lane& tgt = edge->lanes[targetLane];
		if (!(edge->isRoadbedBuilt() && (tgt.op == OpState::Open || tgt.op == OpState::Provisional))) return false;
		const LaneDir dir = fwdLane ? LaneDir::Forward : LaneDir::Backward;
		if (tgt.dir != dir) return false;

		float frontGap = 1e9f, rearGap = 1e9f;
		for (const auto& other : m_vehicles)
		{
			if (other.id == v.id) continue;
			if (other.currentEdge != v.currentEdge || other.currentLane != targetLane) continue;
			const float delta = fwdLane ? (other.arcPos - v.arcPos) : (v.arcPos - other.arcPos);
			if (delta > 0.0f) frontGap = Min(frontGap, delta);
			else              rearGap  = Min(rearGap, -delta);
		}

		constexpr float kDeltaVMax   = 15.0f;
		const float     safetyFront  = params.s0 + 8.0f;
		const float     safetyRear   = params.s0 + params.T * kDeltaVMax + 8.0f;
		return (frontGap >= safetyFront) && (rearGap >= safetyRear);
	};

	if (v.currentLane > 0 && isSafe(v.currentLane - 1))
	{
		v.currentLane--;
		return;
	}

	if (frontGapCurrent < params.s0 * 4.0f)
	{
		if (isSafe(v.currentLane + 1))
			v.currentLane++;
	}
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

