#include "VehicleManager.hpp"
#include <cmath>
#include <chrono>

// ===== ヘルパー =====

static bool isForwardLane(const SimGraph::Edge& edge, int laneIdx)
{
	if (laneIdx < 0 || laneIdx >= static_cast<int>(edge.lanes.size()))
		return true;
	return edge.lanes[laneIdx].dir == LaneDir::Forward;
}

// ===== 初期化 =====

void VehicleManager::init(const SimGraph& simGraph)
{
	buildTrafficLights(simGraph);
	m_lightsDirty = false;
}

void VehicleManager::onNetworkChanged([[maybe_unused]] const SimGraph& simGraph)
{
	m_lightsDirty = true;
	// 全車両の経路を無効化して再リクエスト
	for (auto& v : m_vehicles)
	{
		v.routeWaypoints.clear();
		v.routeIdx = 0;
		v.routeRequested = false;
	}
}

// ===== スポーン =====

void VehicleManager::spawnRandom(const SimGraph& simGraph, VehicleType type)
{
	Array<int> candidates;
	for (const auto& [eid, e] : simGraph.edges)
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
	const auto* edge = simGraph.getEdge(edgeId);

	Vehicle v;
	v.id          = m_nextId++;
	v.currentEdge = edgeId;
	v.currentLane = 0;
	v.speed       = 5.0f;
	v.type        = type;
	v.mode        = VehicleMode::Active;
	if (edge)
		v.arcPos = static_cast<float>(Random(0.0, static_cast<double>(edge->length) * 0.8));

	m_vehicles << std::move(v);
}

// ===== RouteResponse 適用 =====

void VehicleManager::applyRouteResponse(const RouteResponse& resp)
{
	for (auto& v : m_vehicles)
	{
		if (v.id != resp.vehicleId) continue;
		v.routeRequested = false;
		if (resp.found)
		{
			v.routeWaypoints = resp.waypoints;
			v.routeIdx = 0;
		}
		break;
	}
}

// ===== リクエスト収集 =====

Array<SimRequest> VehicleManager::collectRequests()
{
	Array<SimRequest> result = std::move(m_pendingRequests);
	m_pendingRequests.clear();
	return result;
}

// ===== メインフレーム更新 =====

void VehicleManager::update(double dt, GameTime gameNow,
                            const SimGraph& simGraph,
                            const HashSet<int>& visibleEdges)
{
	using Clock = std::chrono::steady_clock;
	auto toMs = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
	m_stats = {};

	const auto t0 = Clock::now();

	if (m_lightsDirty)
	{
		buildTrafficLights(simGraph);
		m_lightsDirty = false;
	}
	updateTrafficLights(gameNow);

	const auto t1 = Clock::now();
	m_stats.signal = toMs(t1 - t0);

	double idmTotal = 0, lcTotal = 0;

	for (auto& v : m_vehicles)
	{
		if (v.currentEdge == -1) continue;
		if (!simGraph.getEdge(v.currentEdge)) { v.currentEdge = -1; continue; }

		// --- Active / Dormant 遷移 ---
		const bool edgeVisible = visibleEdges.contains(v.currentEdge);
		if (edgeVisible && v.mode == VehicleMode::Dormant)
			activateVehicle(v, simGraph);
		else if (!edgeVisible && v.mode == VehicleMode::Active)
			deactivateVehicle(v, simGraph);

		// --- モード別更新 ---
		if (v.mode == VehicleMode::Active)
		{
			const auto lcStart = Clock::now();
			if (RandomBool(0.01))
				tryLaneChange(v, simGraph);
			lcTotal += toMs(Clock::now() - lcStart);

			const auto idmStart = Clock::now();
			updateActiveVehicle(v, dt, gameNow, simGraph);
			idmTotal += toMs(Clock::now() - idmStart);
		}
		else
		{
			updateDormantVehicle(v, dt);
		}

		// --- 経路リクエスト ---
		if (!v.routeRequested && v.routeIdx >= static_cast<int>(v.routeWaypoints.size()))
			requestRoute(v, simGraph);
	}

	m_stats.idm = idmTotal;
	m_stats.laneChange = lcTotal;

	m_vehicles.remove_if([](const Vehicle& v) { return v.currentEdge == -1; });

	const auto t2 = Clock::now();
	m_stats.other = toMs(t2 - t0) - m_stats.total();
	if (m_stats.other < 0) m_stats.other = 0;
}

// ===== Active 車両更新 =====

void VehicleManager::updateActiveVehicle(Vehicle& v, double dt, [[maybe_unused]] GameTime gameNow, const SimGraph& simGraph)
{
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
	advanceOnEdge(v, dt, gameNow, simGraph);
}

void VehicleManager::advanceOnEdge(Vehicle& v, double dt, [[maybe_unused]] GameTime gameNow, const SimGraph& simGraph)
{
	const auto* edge = simGraph.getEdge(v.currentEdge);
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

	const bool reachedEnd = fwdLane
		? (v.arcPos >= edge->length)
		: (v.arcPos <= 0.0f);

	if (reachedEnd)
		transitToNextWaypoint(v, simGraph);
}

bool VehicleManager::transitToNextWaypoint(Vehicle& v, const SimGraph& simGraph)
{
	// 経路のウェイポイントがあれば使う
	while (v.routeIdx < static_cast<int>(v.routeWaypoints.size()))
	{
		const auto& wp = v.routeWaypoints[v.routeIdx++];
		if (wp.edgeId != v.currentEdge)
		{
			v.currentEdge    = wp.edgeId;
			v.currentLane    = wp.laneIndex;
			v.arcPos         = wp.entryArcPos;
			v.dormantTotalTime = wp.estimatedTimeSec;
			v.speed         *= 0.8f;
			return true;
		}
	}

	// フォールバック: ランダムに隣接エッジへ
	const auto* edge = simGraph.getEdge(v.currentEdge);
	if (!edge) { v.currentEdge = -1; return false; }

	const int exitNId = isForwardLane(*edge, v.currentLane) ? edge->nodeB : edge->nodeA;
	const auto* exitNode = simGraph.getNode(exitNId);
	if (!exitNode) { v.currentEdge = -1; return false; }

	Array<int> candidates;
	for (const int eid : exitNode->edgeIds)
		if (eid != v.currentEdge)
			candidates << eid;

	if (candidates.isEmpty()) { v.currentEdge = -1; return false; }

	const int nextEdge = candidates[Random(0, static_cast<int>(candidates.size()) - 1)];
	const auto* nextE = simGraph.getEdge(nextEdge);
	v.currentEdge = nextEdge;
	v.speed      *= 0.8f;

	if (nextE)
	{
		// exitNode が nextEdge の nodeA なら Forward、nodeB なら Backward
		const bool enterAtA = (nextE->nodeA == exitNId);
		const LaneDir needDir = enterAtA ? LaneDir::Forward : LaneDir::Backward;

		v.currentLane = 0;
		for (int i = 0; i < static_cast<int>(nextE->lanes.size()); ++i)
		{
			if (nextE->lanes[i].dir == needDir &&
				(nextE->lanes[i].op == OpState::Open || nextE->lanes[i].op == OpState::Provisional))
			{
				v.currentLane = i;
				break;
			}
		}
		v.arcPos = enterAtA ? 0.0f : nextE->length;
		v.dormantTotalTime = nextE->length / Max(1.0f, nextE->speedLimit / 3.6f);
	}
	else
	{
		v.currentLane = 0;
		v.arcPos = 0.0f;
		v.dormantTotalTime = 1.0f;
	}
	return true;
}

// ===== Dormant 車両更新 =====

void VehicleManager::updateDormantVehicle(Vehicle& v, double dt)
{
	v.dormantTimer -= static_cast<float>(dt);
	if (v.dormantTimer <= 0.0f)
	{
		// 次のウェイポイントへ遷移
		if (v.routeIdx < static_cast<int>(v.routeWaypoints.size()))
		{
			const auto& wp = v.routeWaypoints[v.routeIdx++];
			v.currentEdge      = wp.edgeId;
			v.currentLane      = wp.laneIndex;
			v.arcPos           = wp.entryArcPos;
			v.dormantTimer     = wp.estimatedTimeSec;
			v.dormantTotalTime = wp.estimatedTimeSec;
		}
		else
		{
			// 経路枯渇: 停止して経路リクエスト待ち
			v.dormantTimer = 0.0f;
		}
	}
}

// ===== Active/Dormant 遷移 =====

void VehicleManager::activateVehicle(Vehicle& v, const SimGraph& simGraph)
{
	v.mode = VehicleMode::Active;

	// タイマー消化率から arcPos を復元
	if (v.dormantTotalTime > 0.0f)
	{
		const float elapsed = v.dormantTotalTime - v.dormantTimer;
		const float fraction = Clamp(elapsed / v.dormantTotalTime, 0.0f, 1.0f);

		const auto* edge = simGraph.getEdge(v.currentEdge);
		if (edge)
		{
			const bool fwd = isForwardLane(*edge, v.currentLane);
			if (fwd)
				v.arcPos = v.arcPos + fraction * (edge->length - v.arcPos);
			else
				v.arcPos = v.arcPos - fraction * v.arcPos;

			v.speed = edge->speedLimit / 3.6f * 0.8f;
		}
	}
}

void VehicleManager::deactivateVehicle(Vehicle& v, const SimGraph& simGraph)
{
	v.mode = VehicleMode::Dormant;

	const auto* edge = simGraph.getEdge(v.currentEdge);
	if (edge)
	{
		const bool fwd = isForwardLane(*edge, v.currentLane);
		const float remaining = fwd
			? (edge->length - v.arcPos)
			: v.arcPos;
		const float speedMs = Max(1.0f, edge->speedLimit / 3.6f);
		v.dormantTimer     = remaining / speedMs;
		v.dormantTotalTime = edge->length / speedMs;
	}
	else
	{
		v.dormantTimer     = 1.0f;
		v.dormantTotalTime = 1.0f;
	}
}

// ===== IDM =====

float VehicleManager::idmAcceleration(const Vehicle& v, const IDMParams& params, bool fwdLane) const
{
	float gap   = 1e9f;
	float vLead = params.v0;

	for (const auto& other : m_vehicles)
	{
		if (other.id == v.id)           continue;
		if (other.mode != VehicleMode::Active) continue;
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
		return params.aMax * (1.0f - std::powf(v.speed / Max(0.1f, params.v0), 4.0f));

	const float dv    = v.speed - vLead;
	const float sStar = params.s0 + Max(0.0f,
		v.speed * params.T + v.speed * dv / (2.0f * std::sqrtf(params.aMax * params.b)));
	const float safeGap = Max(0.1f, gap);

	return params.aMax * (
		1.0f - std::powf(v.speed / Max(0.1f, params.v0), 4.0f)
		- (sStar / safeGap) * (sStar / safeGap));
}

// ===== 車線変更 =====

void VehicleManager::tryLaneChange(Vehicle& v, const SimGraph& simGraph)
{
	if (v.currentEdge == -1) return;
	const auto* edge = simGraph.getEdge(v.currentEdge);
	if (!edge) return;

	const bool fwdLane = isForwardLane(*edge, v.currentLane);
	const IDMParams params = getDefaultIDMParams(v.type, edge->speedLimit);

	const float distToExit = fwdLane ? (edge->length - v.arcPos) : v.arcPos;
	if (distToExit < 30.0f) return;

	float frontGapCurrent = 1e9f;
	for (const auto& other : m_vehicles)
	{
		if (other.id == v.id || other.mode != VehicleMode::Active) continue;
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
			if (other.id == v.id || other.mode != VehicleMode::Active) continue;
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

// ===== 信号機 =====

void VehicleManager::buildTrafficLights(const SimGraph& simGraph)
{
	m_trafficLights.clear();
	for (const auto& [nid, node] : simGraph.nodes)
	{
		int validEdges = 0;
		for (const int eid : node.edgeIds)
			if (simGraph.getEdge(eid)) ++validEdges;
		if (validEdges < 3) continue;

		const int half = static_cast<int>(node.edgeIds.size()) / 2;
		Array<SignalPhase> phases;
		SignalPhase phaseA; phaseA.duration = 30.0f;
		SignalPhase phaseB; phaseB.duration = 30.0f;
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

void VehicleManager::updateTrafficLights(GameTime gameNow)
{
	for (auto& [nid, tl] : m_trafficLights)
		tl.update(gameNow);
}

const TrafficLight* VehicleManager::getTrafficLight(int nodeId) const
{
	const auto it = m_trafficLights.find(nodeId);
	return (it != m_trafficLights.end()) ? &it->second : nullptr;
}

// ===== 経路リクエスト =====

void VehicleManager::requestRoute(Vehicle& v, const SimGraph& simGraph)
{
	// ゴールを選ぶ（ランダム）
	if (v.goalEdgeId == -1 || v.goalEdgeId == v.currentEdge)
	{
		const auto ids = simGraph.edgeIds();
		Array<int> candidates;
		for (const int eid : ids)
			if (eid != v.currentEdge) candidates << eid;
		if (candidates.isEmpty()) return;
		v.goalEdgeId = candidates[Random(0, static_cast<int>(candidates.size()) - 1)];
	}

	RouteRequest req;
	req.vehicleId = v.id;
	req.startEdge = v.currentEdge;
	req.startLane = v.currentLane;
	req.goalEdge  = v.goalEdgeId;

	m_pendingRequests << SimRequest{ req };
	v.routeRequested = true;
}
