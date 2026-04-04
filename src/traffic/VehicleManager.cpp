#include "VehicleManager.hpp"
#include <cmath>
#include <chrono>

using namespace TrafficCommon;
using namespace TrafficConfig;

// ===== 初期化 =====

void VehicleManager::init(const SimGraph& simGraph)
{
	buildTrafficLights(simGraph);
	m_lightsDirty = false;
}

void VehicleManager::onNetworkChanged(const SimGraph& simGraph)
{
	buildTrafficLights(simGraph);
	m_lightsDirty = false;
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
	const auto candidates = collectDrivableEdges(simGraph);
	if (candidates.isEmpty()) return;

	const int edgeId = candidates[Random(0, static_cast<int>(candidates.size()) - 1)];
	const auto* edge = simGraph.getEdge(edgeId);

	Vehicle v;
	v.id          = m_nextId++;
	v.currentEdge = edgeId;
	v.currentLane = 0;
	v.speed       = kSpawnSpeed;
	v.type        = type;
	v.mode        = VehicleMode::Active;
	if (edge)
		v.arcPos = static_cast<float>(Random(0.0, static_cast<double>(edge->length) * kSpawnPosRatio));

	m_vehicles << std::move(v);
}

void VehicleManager::spawnOnEdge(int edgeId, const SimGraph& simGraph, VehicleType type, int goalEdgeId)
{
	const auto* edge = simGraph.getEdge(edgeId);
	if (!edge) return;

	// 走行可能な車線を探す
	int laneIdx = -1;
	for (int i = 0; i < static_cast<int>(edge->lanes.size()); ++i)
	{
		if (edge->lanes[i].op == OpState::Open || edge->lanes[i].op == OpState::Provisional)
		{
			laneIdx = i;
			break;
		}
	}
	if (laneIdx < 0) return;

	Vehicle v;
	v.id          = m_nextId++;
	v.currentEdge = edgeId;
	v.currentLane = laneIdx;
	v.speed       = kSpawnSpeed;
	v.type        = type;
	v.mode        = VehicleMode::Active;
	v.goalEdgeId  = goalEdgeId;
	v.arcPos      = static_cast<float>(Random(0.0, static_cast<double>(edge->length) * kSpawnPosRatioGoal));

	m_vehicles << std::move(v);
}

void VehicleManager::setGoalAndReroute(int vehicleId, int goalEdgeId, const SimGraph& simGraph)
{
	for (auto& v : m_vehicles)
	{
		if (v.id != vehicleId) continue;
		v.goalEdgeId = goalEdgeId;
		v.routeWaypoints.clear();
		v.routeIdx = 0;
		v.routeRequested = false;
		requestRoute(v, simGraph);
		break;
	}
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
                            const RoadNetwork& network,
                            const HashSet<int>& visibleEdges)
{
	using Clock = std::chrono::steady_clock;
	auto toMs = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
	m_stats = {};

	const auto t0 = Clock::now();

	if (static_cast<int>(m_vehicles.size()) < m_targetVehicleCount)
		spawnRandom(simGraph);

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
		if (v.currentEdge == -1 && v.location != VehicleLocation::OnConnection) continue;
		if (v.location == VehicleLocation::OnLane || v.location == VehicleLocation::ChangingLane)
		{
			if (!simGraph.getEdge(v.currentEdge)) { v.currentEdge = -1; continue; }
		}

		// Active / Dormant 遷移
		const bool edgeVisible = (v.location == VehicleLocation::OnConnection)
			? true
			: visibleEdges.contains(v.currentEdge);
		if (edgeVisible && v.mode == VehicleMode::Dormant)
			activateVehicle(v, simGraph);
		else if (!edgeVisible && v.mode == VehicleMode::Active && v.location == VehicleLocation::OnLane)
			deactivateVehicle(v, simGraph);

		// モード別更新
		if (v.mode == VehicleMode::Active)
		{
			const auto lcStart = Clock::now();
			if (v.location == VehicleLocation::OnLane && RandomBool(kLaneChangeProbability))
				tryLaneChange(v, simGraph);
			lcTotal += toMs(Clock::now() - lcStart);

			// 車線変更ブレンド更新
			if (v.location == VehicleLocation::ChangingLane)
			{
				v.laneChangeBlend += static_cast<float>(dt) * kLaneChangeBlendRate;
				if (v.laneChangeBlend >= 1.0f)
				{
					v.currentLane = v.laneTo;
					v.location = VehicleLocation::OnLane;
					v.laneChangeBlend = 0.0f;
					v.laneFrom = -1;
					v.laneTo = -1;
				}
			}

			const auto idmStart = Clock::now();
			updateActiveVehicle(v, dt, simGraph, network);
			idmTotal += toMs(Clock::now() - idmStart);
		}
		else
		{
			updateDormantVehicle(v, dt);
		}

		// 経路終端チェック: ウェイポイントを消化済みの車両はデスポーン
		if (v.routeIdx >= static_cast<int>(v.routeWaypoints.size()))
		{
			if (v.routeWaypoints.isEmpty() && !v.routeRequested)
				requestRoute(v, simGraph);  // 初回: まだ経路を持っていない
			else if (!v.routeWaypoints.isEmpty())
				v.currentEdge = -1;  // 経路を走りきった → デスポーン
		}
	}

	m_stats.idm = idmTotal;
	m_stats.laneChange = lcTotal;

	m_vehicles.remove_if([](const Vehicle& v) { return v.currentEdge == -1; });

	const auto t2 = Clock::now();
	m_stats.other = toMs(t2 - t0) - m_stats.total();
	if (m_stats.other < 0) m_stats.other = 0;
}

// ===== Active 車両更新 =====

void VehicleManager::updateActiveVehicle(Vehicle& v, double dt,
                                         const SimGraph& simGraph, const RoadNetwork& network)
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
	if (v.state == VehicleState::WaitingStopSign)
	{
		v.stopSignWait -= static_cast<float>(dt);
		if (v.stopSignWait <= 0.0f)
		{
			v.stopSignWait = 0.0f;
			v.state = VehicleState::Moving;
		}
		return;
	}
	advanceOnSegment(v, dt, simGraph, network);
}

/// @brief OnConnection 状態の車両を進める
void VehicleManager::advanceOnConnection(Vehicle& v, double dt,
                                         const SimGraph& simGraph, const RoadNetwork& network)
{
	const RoadNode* node = network.getNode(v.connectionNodeId);
	if (!node) { v.location = VehicleLocation::OnLane; return; }

	const LaneConnection* conn = nullptr;
	for (const auto& c : node->laneConnections)
		if (c.id == v.connectionId) { conn = &c; break; }
	if (!conn) { v.location = VehicleLocation::OnLane; return; }

	v.arcPos += v.speed * static_cast<float>(dt);

	if (v.arcPos >= conn->path.totalLength)
	{
		v.currentEdge = conn->toEdgeId;
		v.currentLane = conn->toLaneIndex;
		v.location = VehicleLocation::OnLane;

		const auto* nextEdge = simGraph.getEdge(v.currentEdge);
		if (nextEdge)
		{
			const bool fwd = isForwardLane(*nextEdge, v.currentLane);
			const RoadEdge* re = network.getEdge(v.currentEdge);
			const bool isNodeA = re && (re->nodeA == v.connectionNodeId);
			const float cutoff = re ? (isNodeA ? re->cutoffA : re->cutoffB) : 0.0f;
			v.arcPos = fwd
				? (isNodeA ? cutoff : nextEdge->length)
				: (isNodeA ? 0.0f : (nextEdge->length - cutoff));
		}
		else
		{
			v.arcPos = 0.0f;
		}
		v.connectionId = -1;
		v.connectionNodeId = -1;
	}
}

/// @brief OnLane 状態の車両を IDM + 交通制御で進める
void VehicleManager::advanceOnLane(Vehicle& v, double dt,
                                   const SimGraph& simGraph, const RoadNetwork& network)
{
	const auto* edge = simGraph.getEdge(v.currentEdge);
	if (!edge) return;

	const bool fwdLane = isForwardLane(*edge, v.currentLane);
	const IDMParams params = getDefaultIDMParams(v.type, edge->speedLimit);
	float accel = idmAcceleration(m_vehicles, v, params, fwdLane, true);

	// 出口ノードの交通規制チェック
	const int exitNId = fwdLane ? edge->nodeB : edge->nodeA;
	const TrafficControl ctrl = getEdgeControl(exitNId, v.currentEdge, simGraph);

	const RoadEdge* reStop = network.getEdge(v.currentEdge);
	const float stopCutoff = reStop
		? (fwdLane ? reStop->cutoffB : reStop->cutoffA)
		: 0.0f;
	const float distToStop = fwdLane
		? (edge->length - stopCutoff - kStopLineOffset - v.arcPos)
		: (v.arcPos - stopCutoff - kStopLineOffset);

	switch (ctrl)
	{
	case TrafficControl::None:
		break;

	case TrafficControl::Signal:
	{
		const TrafficLight* tl = getTrafficLight(exitNId);
		if (tl && !tl->isGreen(v.currentEdge))
		{
			if (const auto a = stopLineAccel(v.speed, distToStop, kSignalStopDist, params))
				accel = *a;
		}
		break;
	}

	case TrafficControl::Stop:
		if (const auto a = stopLineAccel(v.speed, distToStop, kStopSignDist, params))
			accel = *a;
		if (distToStop > 0.0f && distToStop < 1.0f && v.speed < kStopArrivalThreshold)
		{
			v.speed = 0.0f;
			v.state = VehicleState::WaitingStopSign;
			v.stopSignWait = kStopSignWait;
			return;
		}
		break;

	case TrafficControl::Yield:
		if (distToStop > 0.0f && distToStop < kYieldDist
			&& hasConflictingTraffic(v, exitNId, v.currentEdge, simGraph))
		{
			if (const auto a = stopLineAccel(v.speed, distToStop, kYieldDist, params))
				accel = *a;
		}
		break;
	}

	v.speed = static_cast<float>(Clamp(
		static_cast<double>(v.speed) + accel * dt,
		0.0, static_cast<double>(params.v0)));

	const float advance = v.speed * static_cast<float>(dt);
	if (fwdLane)
		v.arcPos += advance;
	else
		v.arcPos -= advance;

	// カットオフ位置に到達したらノードへ遷移
	const RoadEdge* re = network.getEdge(v.currentEdge);
	const float exitCutoff = re
		? (fwdLane ? re->cutoffB : re->cutoffA)
		: 0.0f;
	const bool reachedEnd = fwdLane
		? (v.arcPos >= edge->length - exitCutoff)
		: (v.arcPos <= exitCutoff);

	if (reachedEnd)
		transitToNextWaypoint(v, simGraph, network);
}

void VehicleManager::advanceOnSegment(Vehicle& v, double dt,
                                      const SimGraph& simGraph, const RoadNetwork& network)
{
	if (v.location == VehicleLocation::OnConnection)
		advanceOnConnection(v, dt, simGraph, network);
	else
		advanceOnLane(v, dt, simGraph, network);
}

bool VehicleManager::transitToNextWaypoint(Vehicle& v, const SimGraph& simGraph, const RoadNetwork& network)
{
	// 経路のウェイポイントがあれば使う
	while (v.routeIdx < static_cast<int>(v.routeWaypoints.size()))
	{
		const auto& wp = v.routeWaypoints[v.routeIdx++];
		if (wp.edgeId != v.currentEdge)
		{
			const auto* edge = simGraph.getEdge(v.currentEdge);
			if (edge)
			{
				const bool fwd = isForwardLane(*edge, v.currentLane);
				const int exitNId = fwd ? edge->nodeB : edge->nodeA;
				const RoadNode* node = network.getNode(exitNId);
				if (node)
				{
					for (const auto& conn : node->laneConnections)
					{
						if (conn.fromEdgeId == v.currentEdge && conn.fromLaneIndex == v.currentLane
							&& conn.toEdgeId == wp.edgeId && conn.toLaneIndex == wp.laneIndex)
						{
							v.location = VehicleLocation::OnConnection;
							v.connectionNodeId = exitNId;
							v.connectionId = conn.id;
							v.arcPos = 0.0f;
							v.speed *= kConnectionSpeedFactor;
							return true;
						}
					}
				}
			}

			// Connection が見つからない場合は直接遷移
			v.currentEdge      = wp.edgeId;
			v.currentLane      = wp.laneIndex;
			v.arcPos           = wp.entryArcPos;
			v.dormantTotalTime = wp.estimatedTimeSec;
			v.location         = VehicleLocation::OnLane;
			v.speed           *= kDirectTransitFactor;
			return true;
		}
	}

	// フォールバック: 交差点の LaneConnection をランダムに選ぶ
	return fallbackRandomTransit(v, simGraph, network);
}

bool VehicleManager::fallbackRandomTransit(Vehicle& v,
                                           const SimGraph& simGraph, const RoadNetwork& network)
{
	const auto* edge = simGraph.getEdge(v.currentEdge);
	if (!edge) { v.currentEdge = -1; return false; }

	const int exitNId = isForwardLane(*edge, v.currentLane) ? edge->nodeB : edge->nodeA;
	const RoadNode* node = network.getNode(exitNId);
	if (!node) { v.currentEdge = -1; return false; }

	Array<const LaneConnection*> candidates;
	for (const auto& conn : node->laneConnections)
	{
		if (conn.fromEdgeId == v.currentEdge && conn.fromLaneIndex == v.currentLane)
			candidates << &conn;
	}

	if (!candidates.isEmpty())
	{
		const auto* conn = candidates[Random(0, static_cast<int>(candidates.size()) - 1)];
		v.location = VehicleLocation::OnConnection;
		v.connectionNodeId = exitNId;
		v.connectionId = conn->id;
		v.arcPos = 0.0f;
		v.speed *= kConnectionSpeedFactor;
		return true;
	}

	// Connection もない場合は従来のフォールバック
	const auto* exitNode = simGraph.getNode(exitNId);
	if (!exitNode) { v.currentEdge = -1; return false; }

	Array<int> edgeCandidates;
	for (const int eid : exitNode->edgeIds)
		if (eid != v.currentEdge) edgeCandidates << eid;

	if (edgeCandidates.isEmpty()) { v.currentEdge = -1; return false; }

	const int nextEdge = edgeCandidates[Random(0, static_cast<int>(edgeCandidates.size()) - 1)];
	const auto* nextE = simGraph.getEdge(nextEdge);
	v.currentEdge = nextEdge;
	v.speed      *= kDirectTransitFactor;
	v.location    = VehicleLocation::OnLane;

	if (nextE)
	{
		const bool enterAtA = (nextE->nodeA == exitNId);
		const LaneDir needDir = enterAtA ? LaneDir::Forward : LaneDir::Backward;
		v.currentLane = 0;
		for (int i = 0; i < static_cast<int>(nextE->lanes.size()); ++i)
		{
			if (nextE->lanes[i].dir == needDir &&
				(nextE->lanes[i].op == OpState::Open || nextE->lanes[i].op == OpState::Provisional))
			{ v.currentLane = i; break; }
		}
		v.arcPos = enterAtA ? 0.0f : nextE->length;
		v.dormantTotalTime = nextE->length / Max(1.0f, nextE->speedLimit / kKmhToMps);
	}
	else
	{
		v.currentLane = 0;
		v.arcPos = 0.0f;
		v.dormantTotalTime = kDefaultDormantTime;
	}
	return true;
}

// ===== Dormant 車両更新 =====

void VehicleManager::updateDormantVehicle(Vehicle& v, double dt)
{
	v.dormantTimer -= static_cast<float>(dt);
	if (v.dormantTimer <= 0.0f)
	{
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
			v.dormantTimer = 0.0f;
		}
	}
}

// ===== Active/Dormant 遷移 =====

void VehicleManager::activateVehicle(Vehicle& v, const SimGraph& simGraph)
{
	v.mode = VehicleMode::Active;

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

			v.speed = edge->speedLimit / kKmhToMps * kActivationSpeedFactor;
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
		const float speedMs = Max(1.0f, edge->speedLimit / kKmhToMps);
		v.dormantTimer     = remaining / speedMs;
		v.dormantTotalTime = edge->length / speedMs;
	}
	else
	{
		v.dormantTimer     = kDefaultDormantTime;
		v.dormantTotalTime = kDefaultDormantTime;
	}
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
	if (distToExit < kLaneChangeMinExitDist) return;

	// 現在車線の前方ギャップ
	float frontGapCurrent = 1e9f;
	for (const auto& other : m_vehicles)
	{
		if (other.id == v.id || other.mode != VehicleMode::Active) continue;
		if (other.currentEdge != v.currentEdge || other.currentLane != v.currentLane) continue;
		const float delta = fwdLane ? (other.arcPos - v.arcPos) : (v.arcPos - other.arcPos);
		if (delta > 0.0f && delta < frontGapCurrent)
			frontGapCurrent = delta;
	}

	auto tryTarget = [&](int targetLane) -> bool
	{
		float frontGap, rearGap;
		measureGaps(m_vehicles, v.id, v.currentEdge, targetLane,
		            v.arcPos, fwdLane, true, frontGap, rearGap);
		if (!isLaneChangeSafe(*edge, targetLane, fwdLane, frontGap, rearGap, params))
			return false;
		v.location = VehicleLocation::ChangingLane;
		v.laneFrom = v.currentLane;
		v.laneTo   = targetLane;
		v.laneChangeBlend = 0.0f;
		return true;
	};

	// キープレフト優先
	if (v.currentLane > 0 && tryTarget(v.currentLane - 1))
		return;

	if (frontGapCurrent < params.s0 * kRightLaneGapMultiplier)
		tryTarget(v.currentLane + 1);
}

// ===== 信号機 =====

void VehicleManager::buildTrafficLights(const SimGraph& simGraph)
{
	m_trafficLights.clear();
	for (const auto& [nid, node] : simGraph.nodes)
	{
		Array<int> signalEdges;
		for (const int eid : node.edgeIds)
		{
			const auto it = node.edgeControl.find(eid);
			if (it != node.edgeControl.end() && it->second == TrafficControl::Signal)
				if (simGraph.getEdge(eid)) signalEdges << eid;
		}
		if (static_cast<int>(signalEdges.size()) < kMinEdgesForSignal) continue;

		auto phases = buildTwoGroupPhases(signalEdges);
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

// ===== 交通規制ヘルパー =====

TrafficControl VehicleManager::getEdgeControl(int nodeId, int edgeId, const SimGraph& simGraph) const
{
	const auto* node = simGraph.getNode(nodeId);
	if (!node) return TrafficControl::None;

	const auto it = node->edgeControl.find(edgeId);
	if (it != node->edgeControl.end())
		return it->second;

	return TrafficControl::None;
}

bool VehicleManager::hasConflictingTraffic(const Vehicle& v, int nodeId,
                                           int edgeId, const SimGraph& simGraph) const
{
	const auto* node = simGraph.getNode(nodeId);
	if (!node) return false;

	for (const auto& other : m_vehicles)
	{
		if (other.id == v.id) continue;
		if (other.mode != VehicleMode::Active) continue;
		if (other.currentEdge == edgeId) continue;

		if (other.location == VehicleLocation::OnConnection && other.connectionNodeId == nodeId)
			return true;

		const auto* otherEdge = simGraph.getEdge(other.currentEdge);
		if (!otherEdge) continue;

		bool otherConnected = false;
		for (const int eid : node->edgeIds)
			if (eid == other.currentEdge) { otherConnected = true; break; }
		if (!otherConnected) continue;

		const bool otherFwd = isForwardLane(*otherEdge, other.currentLane);
		const int otherExitNId = otherFwd ? otherEdge->nodeB : otherEdge->nodeA;
		if (otherExitNId != nodeId) continue;

		const float otherDist = otherFwd
			? (otherEdge->length - other.arcPos)
			: other.arcPos;
		if (otherDist < kYieldDist && otherDist > 0.0f)
			return true;
	}

	return false;
}

// ===== 経路リクエスト =====

void VehicleManager::requestRoute(Vehicle& v, const SimGraph& simGraph)
{
	if (v.goalEdgeId == -1 || v.goalEdgeId == v.currentEdge)
	{
		const int goal = selectRandomGoalEdge(simGraph, v.currentEdge);
		if (goal == -1) return;
		v.goalEdgeId = goal;
	}

	RouteRequest req;
	req.vehicleId = v.id;
	req.startEdge = v.currentEdge;
	req.startLane = v.currentLane;
	req.goalEdge  = v.goalEdgeId;

	m_pendingRequests << SimRequest{ req };
	v.routeRequested = true;
}
