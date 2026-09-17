#include "../gen/GenerationSettings.hpp"
#include "VehicleManager.hpp"
#include "TrafficSpawn.hpp"
#include "VehiclePose.hpp"
#include <cmath>
#include <chrono>

using namespace TrafficCommon;
using namespace TrafficConfig;

// 匿名名前空間内ヘルパーの前方宣言（定義は本ファイル下方）
namespace
{
	int findPlannedConnectionId(const Vehicle& v, const SimGraph::Node& node);
}

// ===== 初期化 =====

void VehicleManager::init(const SimGraph& simGraph, const RoadNetwork& network, const World* world)
{
	m_world = world;
	m_spawnCredit = GenerationSettings::get().traffic_burst;
	if (m_world) { m_buildingAccess.rebuild(*m_world, network, simGraph); }
	buildTrafficLights(simGraph, &network);
	m_lightsDirty = false;
}

void VehicleManager::onNetworkChanged(const SimGraph& simGraph, const RoadNetwork& network,
                                      const NetworkChangeContext& context)
{
	if (context.kind == NetworkChangeKind::MovedIntersectionNode)
	{
		for (const int nodeId : context.dirtyNodeIds)
			rebuildTrafficLightForNode(nodeId, simGraph, network);
	}
	else
	{
		buildTrafficLights(simGraph, &network);
	}
	if (m_world) { m_buildingAccess.rebuild(*m_world, network, simGraph); }
	m_spawnRefresh = 0;
	m_lightsDirty = false;
}

// ===== スポーン =====

void VehicleManager::spawnRandom(const SimGraph& simGraph, VehicleType type)
{
	const auto candidates = simGraph.edgeIds();
	if (candidates.isEmpty()) { return; }
	for (int attempt = 0; attempt < 16; ++attempt)
	{
		const int id = candidates[Random(0, static_cast<int>(candidates.size())-1)];
		if (tryAutomaticSpawn(id, simGraph, type, nullptr)) { return; }
	}
}

void VehicleManager::spawnOnEdge(int edgeId, const SimGraph& simGraph, VehicleType type, int goalEdgeId)
{
	const auto* edge = simGraph.getEdge(edgeId);
	if (!edge) { return; }

	// デバッグスポーンでも破綻しないよう、閉鎖車線を避けて走行可能車線を選ぶ。
	int laneIdx = -1;
	for (int i = 0; i < static_cast<int>(edge->lanes.size()); ++i)
	{
		if (edge->lanes[i].op == OpState::Open || edge->lanes[i].op == OpState::Provisional)
		{
			laneIdx = i;
			break;
		}
	}
	if (laneIdx < 0) { return; }

	Vehicle v;
	v.id          = m_nextId++;
	v.currentEdge = edgeId;
	v.currentLane = laneIdx;
	v.speed       = kSpawnSpeed;
	v.type        = type;
	v.mode        = VehicleMode::Active;
	v.goalEdgeId  = goalEdgeId;
	v.departedAt  = m_lastGameNow;
	v.arcPos      = static_cast<float>(Random(0.0, static_cast<double>(edge->length) * kSpawnPosRatioGoal));

	m_vehicleIndices[v.id] = m_vehicles.size();
	m_vehicles << std::move(v);
}

int VehicleManager::spawnBus(const Array<int>& stopEdgeIds, int routeId,
	const SimGraph& simGraph)
{
	if (stopEdgeIds.size() < 2) return -1;
	const int originEdgeId = stopEdgeIds.front();
	const int destinationEdgeId = stopEdgeIds[1];
	const int expectedId = m_nextId;
	spawnOnEdge(originEdgeId, simGraph, VehicleType::Bus, destinationEdgeId);
	if (m_nextId == expectedId) return -1;
	Vehicle& bus = m_vehicles.back();
	bus.busRouteId = routeId;
	bus.busNextStopIdx = 1;
	bus.busStopEdgeIds = stopEdgeIds;
	return bus.id;
}

VehicleType VehicleManager::selectDemandVehicleType() const
{
	if (!m_trafficDemandConfigured) return VehicleType::PassengerCar;
	const double roll = Random(0.0, 1.0);
	if (roll < m_trafficDemand.largeTruckShare) return VehicleType::LargeTruck;
	if (roll < m_trafficDemand.largeTruckShare + m_trafficDemand.smallTruckShare)
		return VehicleType::SmallTruck;
	return (Random(0, 1) == 0) ? VehicleType::PassengerCar : VehicleType::KeiCar;
}

void VehicleManager::recordCompletedTrip(const Vehicle& vehicle, GameTime gameNow)
{
	if (vehicle.type == VehicleType::Bus
		|| vehicle.type == VehicleType::SmallTruck
		|| vehicle.type == VehicleType::LargeTruck
		|| vehicle.type == VehicleType::Emergency)
	{
		return;
	}
	const double elapsed = Max(0.0, gameNow - vehicle.departedAt);
	const double calendarMinutes = elapsed * GameClock::kCalendarMinutesPerSecond;
	if (calendarMinutes <= 0.0) return;
	m_completedTripMinutes << calendarMinutes;
	while (m_completedTripMinutes.size() > 200)
	{
		m_completedTripMinutes.erase(m_completedTripMinutes.begin());
	}
}

void VehicleManager::setGoalAndReroute(int vehicleId, int goalEdgeId, const SimGraph& simGraph)
{
	for (auto& v : m_vehicles)
	{
		if (v.id != vehicleId) continue;
		v.goalEdgeId = goalEdgeId;
		v.goalLane = -1; v.goalArc = -1; v.destinationBuilding = -1;
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
	const auto found = m_vehicleIndices.find(resp.vehicleId);
	if (found == m_vehicleIndices.end()) { return; }
	{
		auto& v = m_vehicles[found->second];
		v.routeRequested = false;
		if (resp.found)
		{
			// 経路探索成功時は経路全体を差し替え、失敗状態もここで解消する。
			v.routeWaypoints = resp.waypoints;
			v.routeIdx = 0;
			v.routeFailCount = 0;
		}
		else
		{
			// 経路探索失敗 → 5回連続失敗で削除、そうでなければゴールをリセットして再試行
			++v.routeFailCount;
			++m_populationStats.routeFailures;
			if (v.routeFailCount >= 5)
				v.currentEdge = -1;
			else { v.goalEdgeId = -1; v.goalArc = -1; v.goalLane = -1; }
		}

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
	m_lastGameNow = gameNow;
	if (m_trafficDemandConfigured)
	{
		m_targetVehicleCount = Clamp(static_cast<int>(Math::Round(
			m_trafficDemand.targetVehicleCount * m_eventDemandMultiplier)), 0, GenerationSettings::get().traffic_maximumVehicles);
	}

	const auto t0 = Clock::now();

	// フレーム冒頭で台数補充と信号キャッシュ更新を済ませ、以降の車両更新は
	// 同じネットワーク状態を前提に進める。
	m_laneTraffic.rebuild(m_vehicles);
	replenishTraffic(dt, simGraph, network, visibleEdges);

	if (m_lightsDirty)
	{
		buildTrafficLights(simGraph, &network);
		m_lightsDirty = false;
	}
	updateTrafficLights(gameNow);

	const auto t1 = Clock::now();
	m_stats.signal = toMs(t1 - t0);

	double idmTotal = 0, lcTotal = 0;

	for (auto& v : m_vehicles)
	{
		// 描画範囲外も同じ追従・交差点更新を続け、車列を維持する。
		if (v.currentEdge == -1 && v.location != VehicleLocation::OnConnection) continue;
		if (v.location == VehicleLocation::OnLane || v.location == VehicleLocation::ChangingLane)
		{
			if (!simGraph.getEdge(v.currentEdge)) { v.currentEdge = -1; continue; }
		}

		// Visibility controls rendering only. Signals and queues continue offscreen.
		v.mode = visibleEdges.contains(v.currentEdge) || v.location == VehicleLocation::OnConnection
			? VehicleMode::Active : VehicleMode::Dormant;
		const auto laneStart = Clock::now();
		if (v.location == VehicleLocation::OnLane) { tryLaneChange(v, simGraph); }
		lcTotal += toMs(Clock::now() - laneStart);
		if (v.location == VehicleLocation::ChangingLane)
		{
			v.laneChangeBlend += static_cast<float>(dt) * kLaneChangeBlendRate;
			if (v.laneChangeBlend >= 1)
			{
				v.currentLane = v.laneTo; v.location = VehicleLocation::OnLane;
				v.laneChangeBlend = 0; v.laneFrom = -1; v.laneTo = -1;
			}
		}
		const auto moveStart = Clock::now();
		updateActiveVehicle(v, dt, simGraph, network);
		idmTotal += toMs(Clock::now() - moveStart);

		// 目的地エッジを実際に走り切った車両だけを完了履歴へ記録する。
		if (v.tripCompleted)
		{
			++m_populationStats.completed;
			recordCompletedTrip(v, gameNow);
			v.currentEdge = -1;
		}
		else if (v.routeWaypoints.isEmpty() && !v.routeRequested
			&& v.state == VehicleState::Moving)
		{
			requestRoute(v, simGraph);
		}
		else if (v.routeIdx < static_cast<int>(v.routeWaypoints.size())
			&& !v.routeRequested
			&& !simGraph.getEdge(v.routeWaypoints[v.routeIdx].edgeId))
		{
			v.routeWaypoints.clear();
			v.routeIdx = 0;
			requestRoute(v, simGraph);
		}
	}

	m_stats.idm = idmTotal;
	m_stats.laneChange = lcTotal;

	m_vehicles.remove_if([](const Vehicle& v) { return v.currentEdge == -1; });
	m_vehicleIndices.clear();
	for (size_t i = 0; i < m_vehicles.size(); ++i) { m_vehicleIndices[m_vehicles[i].id] = i; }

	const auto t2 = Clock::now();
	m_stats.other = toMs(t2 - t0) - m_stats.total();
	if (m_stats.other < 0) m_stats.other = 0;
}

// ===== Active 車両更新 =====

void VehicleManager::updateActiveVehicle(Vehicle& v, double dt,
                                         const SimGraph& simGraph, const RoadNetwork& network)
{
	// 停車待ち状態はここで完結させ、通常走行へ戻った車両だけを区間更新へ進める。
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

	// 交差点内部では LaneConnection の軌跡に沿って進め、終端で次エッジへ着地させる。
	// 交差点途中で接触回避のため停止した車も再発進できる。
	constexpr float kConnectionRecoverySpeed=8.0f;
	const auto* fromEdge=simGraph.getEdge(conn->fromEdgeId);
	const auto* toEdge=simGraph.getEdge(conn->toEdgeId);
	const float speedLimit=Min(fromEdge ? fromEdge->speedLimit : 30.0f,toEdge ? toEdge->speedLimit : 30.0f);
	const IDMParams recovery=getDefaultIDMParams(v.type,speedLimit);
	const float targetSpeed=Min(kConnectionRecoverySpeed,recovery.v0);
	const auto pair = m_laneTraffic.neighbors(v.connectionNodeId, v.connectionId, v.arcPos, true, v.id, true);
	const float length = static_cast<float>(TrafficSpawn::vehicleLength(v.type));
	float gap = pair.front ? Max(0.0f, pair.front->arc - v.arcPos - (length + pair.front->length) * .5f) : 1e9f;
	float leaderSpeed = pair.front ? pair.front->speed : targetSpeed;
	if (toEdge)
	{
		const bool forward = isForwardLane(*toEdge, conn->toLaneIndex);
		const auto* road = network.getEdge(toEdge->id);
		const float entry = road ? (forward ? road->cutoffA : toEdge->length - road->cutoffB) : (forward ? 0.0f : toEdge->length);
		const auto ahead = m_laneTraffic.neighbors(toEdge->id, conn->toLaneIndex, entry, forward, v.id);
		if (ahead.front)
		{
			const float remaining = conn->path.totalLength - v.arcPos + Abs(ahead.front->arc - entry)
				- (length + ahead.front->length) * .5f;
			if (remaining < gap) { gap = Max(0.0f, remaining); leaderSpeed = ahead.front->speed; }
		}
	}
	IDMParams params = recovery; params.v0 = targetSpeed;
	v.speed = Clamp(v.speed + static_cast<float>(dt) * idmFollowingAcceleration(v.speed, params, gap, leaderSpeed), 0.0f, targetSpeed);
	if (dt > 0) { v.speed = Min(v.speed, Max(0.0f, gap - .15f) / static_cast<float>(dt)); }
	avoidDrivenVehicle(v,dt,network);
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
		m_laneTraffic.reserve(v, v.currentLane);
	}
}

/// @brief OnLane 状態の車両を IDM + 交通制御で進める
void VehicleManager::advanceOnLane(Vehicle& v, double dt,
                                   const SimGraph& simGraph, const RoadNetwork& network)
{
	const auto* edge = simGraph.getEdge(v.currentEdge);
	if (!edge) return;

	// 通常走行ではまず IDM で基本加速度を決め、その後で信号や停止規制を重ねて
	// 交差点手前の減速や停止状態への遷移を制御する。
	const bool fwdLane = isForwardLane(*edge, v.currentLane);
	const IDMParams params = getDefaultIDMParams(v.type,
		static_cast<float>(edge->speedLimit * m_globalSpeedMultiplier));
	const auto neighbors = m_laneTraffic.neighbors(v.currentEdge, v.currentLane, v.arcPos, fwdLane, v.id);
	float gap = neighbors.front ? Max(0.0f, Abs(neighbors.front->arc - v.arcPos)
		- (neighbors.front->length + static_cast<float>(TrafficSpawn::vehicleLength(v.type))) * .5f) : 1e9f;
	float accel = idmFollowingAcceleration(v.speed, params, gap, neighbors.front ? neighbors.front->speed : params.v0);

	// Approach the destination curb as a stationary virtual leader, stopping at the access point.
	if (v.goalArc >= 0 && v.currentEdge == v.goalEdgeId && v.currentLane == v.goalLane)
	{
		const float remaining = (v.goalArc - v.arcPos) * (fwdLane ? 1.0f : -1.0f);
		if (remaining >= 0) { accel = Min(accel, idmFollowingAcceleration(v.speed, params, remaining + params.s0, 0)); }
	}

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

	// Keep the intersection clear when the receiving lane is full. Queues therefore
	// propagate onto upstream roads instead of overlapping cars inside the crossing.
	if (distToStop < 45 && v.goalEdgeId != v.currentEdge)
	{
		const auto* node = network.getNode(exitNId);
		const auto* simNode = simGraph.getNode(exitNId);
		const int planned = simNode ? findPlannedConnectionId(v, *simNode) : -1;
		if (node)
		{
			for (const auto& connection : node->laneConnections)
			{
				if (connection.id != planned) { continue; }
				const auto* next = simGraph.getEdge(connection.toEdgeId);
				const auto* road = network.getEdge(connection.toEdgeId);
				if (!next || !road) { continue; }
				const bool forward = isForwardLane(*next, connection.toLaneIndex);
				const float entry = forward ? road->cutoffA : next->length - road->cutoffB;
				const auto queue = m_laneTraffic.neighbors(next->id, connection.toLaneIndex, entry, forward, v.id);
				if (queue.front && Abs(queue.front->arc - entry) < TrafficSpawn::vehicleLength(v.type) + queue.front->length * .5 + 2)
				{
					if (const auto braking = stopLineAccel(v.speed, distToStop, 50, params)) { accel = Min(accel, *braking); }
				}
			}
		}
	}

	switch (ctrl)
	{
	case TrafficControl::None:
		break;

	case TrafficControl::Signal:
	{
		const TrafficLight* tl = getTrafficLight(exitNId);
		const auto* exitNode = simGraph.getNode(exitNId);
		if (tl && exitNode)
		{
			// これから使う LaneConnection を先読みして判定。
			// 経路あり: findPlannedConnectionId が一意に特定。
			// 経路なし or 一致なし: 現在車線から出る candidate のうち一つでも青なら進入 OK（案 β）。
			const int planned = findPlannedConnectionId(v, *exitNode);

			bool anyMatch = false;
			bool anyGreen = false;
			if (planned >= 0)
			{
				anyMatch = true;
				anyGreen = tl->isGreen(planned);
			}
			else
			{
				for (const auto& conn : exitNode->laneConnections)
				{
					if (conn.fromEdgeId == v.currentEdge && conn.fromLaneIndex == v.currentLane)
					{
						anyMatch = true;
						if (tl->isGreen(conn.id)) { anyGreen = true; break; }
					}
				}
			}

			if (anyMatch && !anyGreen)
			{
				if (const auto a = stopLineAccel(v.speed, distToStop, kSignalStopDist, params))
					accel = Min(accel, *a);
			}
		}
		break;
	}

	case TrafficControl::Stop:
		if (const auto a = stopLineAccel(v.speed, distToStop, kStopSignDist, params))
			accel = Min(accel, *a);
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
				accel = Min(accel, *a);
		}
		break;
	}

	v.speed = static_cast<float>(Clamp(
		static_cast<double>(v.speed) + accel * dt,
		0.0, static_cast<double>(params.v0)));

	avoidDrivenVehicle(v,dt,network);
	float advance = Min(v.speed * static_cast<float>(dt), Max(0.0f, gap - .15f));
	if (dt > 0) { v.speed = Min(v.speed, advance / static_cast<float>(dt)); }
	if (v.goalArc >= 0 && v.currentEdge == v.goalEdgeId && v.currentLane == v.goalLane)
	{
		const float remaining = (v.goalArc - v.arcPos) * (fwdLane ? 1.0f : -1.0f);
		if (remaining >= -.5f && remaining <= advance + .5f)
		{
			v.arcPos = v.goalArc; v.speed = 0; v.tripCompleted = true; return;
		}
	}
	if (fwdLane)
		v.arcPos += advance;
	else
		v.arcPos -= advance;

	// エッジ端ではなく cutoff に達した時点で、この区間の走行は終了とみなす。
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

namespace
{
	/// @brief 車両を Connection に遷移させる
	void enterConnection(Vehicle& v, int exitNodeId, int connectionId)
	{
		v.location         = VehicleLocation::OnConnection;
		v.connectionNodeId = exitNodeId;
		v.connectionId     = connectionId;
		v.arcPos           = 0.0f;
		v.speed           *= kConnectionSpeedFactor;
	}

	/// @brief 車両を指定エッジに直接遷移させる（Connection なし）
	void enterEdgeDirect(Vehicle& v, int edgeId, int laneIndex,
	                     float arcPos, float estimatedTime)
	{
		v.currentEdge      = edgeId;
		v.currentLane      = laneIndex;
		v.arcPos           = arcPos;
		v.dormantTotalTime = estimatedTime;
		v.location         = VehicleLocation::OnLane;
		v.speed           *= kDirectTransitFactor;
	}

	/// @brief routeWaypoints から「現在エッジと異なる最初のウェイポイント」を返す
	const RouteWaypoint* findNextDifferentWaypoint(const Vehicle& v)
	{
		for (int i = v.routeIdx; i < static_cast<int>(v.routeWaypoints.size()); ++i)
		{
			if (v.routeWaypoints[i].edgeId != v.currentEdge)
				return &v.routeWaypoints[i];
		}
		return nullptr;
	}

	/// @brief 経路の次ウェイポイントが要求する車線（targetLane）を返す
	/// @details 詳細は plan/19_vehicle_movement_spec.md §7 参照。
	/// @return 必要な fromLaneIndex (-1 = 経路駆動車線変更不要 / 必要情報なし)
	int findRouteTargetLane(const Vehicle& v, const SimGraph& simGraph)
	{
		const RouteWaypoint* wp = findNextDifferentWaypoint(v);
		if (!wp) return -1;

		const auto* edge = simGraph.getEdge(v.currentEdge);
		if (!edge) return -1;

		const int exitNId = isForwardLane(*edge, v.currentLane) ? edge->nodeB : edge->nodeA;
		const auto* node = simGraph.getNode(exitNId);
		if (!node) return -1;

		// 候補のうち conn.fromLaneIndex が現在車線に最も近いものを選ぶ
		int bestFromLane = -1;
		int bestDelta    = 1 << 20;
		for (const auto& conn : node->laneConnections)
		{
			if (conn.fromEdgeId  != v.currentEdge) continue;
			if (conn.toEdgeId    != wp->edgeId)    continue;
			if (conn.toLaneIndex != wp->laneIndex) continue;

			const int delta = std::abs(conn.fromLaneIndex - v.currentLane);
			if (delta < bestDelta)
			{
				bestDelta    = delta;
				bestFromLane = conn.fromLaneIndex;
			}
		}
		return bestFromLane;
	}

	/// @brief 信号判定用に、車両がこの先使う LaneConnection を経路から特定する
	/// @details 詳細は plan/19_vehicle_movement_spec.md §6 参照。
	/// @return 経路一致した LaneConnection ID (-1 = 経路なし、または現在車線から行けない)
	int findPlannedConnectionId(const Vehicle& v, const SimGraph::Node& node)
	{
		const RouteWaypoint* wp = findNextDifferentWaypoint(v);
		if (!wp) return -1;

		for (const auto& conn : node.laneConnections)
		{
			if (conn.fromEdgeId   == v.currentEdge
			    && conn.fromLaneIndex == v.currentLane
			    && conn.toEdgeId    == wp->edgeId
			    && conn.toLaneIndex == wp->laneIndex)
				return conn.id;
		}
		return -1;
	}

	/// @brief 出口ノード ID を求める
	int getExitNodeId(const Vehicle& v, const SimGraph::Edge& edge)
	{
		return isForwardLane(edge, v.currentLane) ? edge.nodeB : edge.nodeA;
	}
}

bool VehicleManager::transitToNextWaypoint(Vehicle& v, const SimGraph& simGraph, const RoadNetwork& network)
{
	// 経路列を前から消化し、交差点接続が定義されている時は Connection を優先し、
	// そうでなければウェイポイント上のエッジへ直接遷移する。
	while (v.routeIdx < static_cast<int>(v.routeWaypoints.size()))
	{
		const auto& wp = v.routeWaypoints[v.routeIdx++];
		if (wp.edgeId == v.currentEdge) continue;

		const auto* edge = simGraph.getEdge(v.currentEdge);
		if (edge)
		{
			const int exitNId = getExitNodeId(v, *edge);
			const RoadNode* node = network.getNode(exitNId);
			if (node)
			{
				for (const auto& conn : node->laneConnections)
				{
					if (conn.fromEdgeId == v.currentEdge && conn.fromLaneIndex == v.currentLane
						&& conn.toEdgeId == wp.edgeId && conn.toLaneIndex == wp.laneIndex)
					{
						if (!m_laneTraffic.connectionEntryClear(exitNId, conn.id, v.type)) { --v.routeIdx; v.speed = 0; return false; }
						enterConnection(v, exitNId, conn.id); m_laneTraffic.reserveConnection(v);
						return true;
					}
				}
			}
		}

		enterEdgeDirect(v, wp.edgeId, wp.laneIndex, wp.entryArcPos, wp.estimatedTimeSec);
		return true;
	}

	if (!v.routeWaypoints.isEmpty())
	{
		if (v.type == VehicleType::Bus && v.busStopEdgeIds.size() >= 2)
		{
			v.busNextStopIdx = (v.busNextStopIdx + 1)
				% static_cast<int>(v.busStopEdgeIds.size());
			v.goalEdgeId = v.busStopEdgeIds[v.busNextStopIdx];
			v.routeWaypoints.clear();
			v.routeIdx = 0;
			v.routeRequested = false;
			v.state = VehicleState::WaitingBusStop;
			v.busWaitRemaining = 15.0f;
			v.speed = 0.0f;
			return false;
		}

		if (v.goalArc >= 0)
		{
			v.routeWaypoints.clear(); v.routeIdx = 0; v.routeRequested = false;
			v.goalEdgeId = -1; v.goalArc = -1; v.goalLane = -1;
			return false;
		}
		v.tripCompleted = true;
		return false;
	}

	return fallbackRandomTransit(v, simGraph, network);
}

bool VehicleManager::fallbackRandomTransit(Vehicle& v,
                                           const SimGraph& simGraph, const RoadNetwork& network)
{
	const auto* edge = simGraph.getEdge(v.currentEdge);
	if (!edge) { v.currentEdge = -1; return false; }

	const int exitNId = getExitNodeId(v, *edge);
	const RoadNode* node = network.getNode(exitNId);
	if (!node) { v.currentEdge = -1; return false; }

	// LaneConnection から候補を探す
	Array<const LaneConnection*> candidates;
	for (const auto& conn : node->laneConnections)
	{
		if (conn.fromEdgeId == v.currentEdge && conn.fromLaneIndex == v.currentLane)
			candidates << &conn;
	}

	// 経路が切れても、まずは既存の LaneConnection を使ってネットワーク上に留める。
	// それも無理な簡易ノードだけ、接続エッジへの直接遷移で交通流を維持する。
	if (!candidates.isEmpty())
	{
		const auto* conn = candidates[Random(0, static_cast<int>(candidates.size()) - 1)];
		if (!m_laneTraffic.connectionEntryClear(exitNId, conn->id, v.type)) { v.speed = 0; return false; }
		enterConnection(v, exitNId, conn->id); m_laneTraffic.reserveConnection(v);
		return true;
	}

	const auto* exitNode = simGraph.getNode(exitNId);
	if (!exitNode) { v.currentEdge = -1; return false; }

	Array<int> edgeCandidates;
	for (const int eid : exitNode->edgeIds)
		if (eid != v.currentEdge) edgeCandidates << eid;

	if (edgeCandidates.isEmpty()) { v.currentEdge = -1; return false; }

	const int nextEdge = edgeCandidates[Random(0, static_cast<int>(edgeCandidates.size()) - 1)];
	const auto* nextE = simGraph.getEdge(nextEdge);

	if (nextE)
	{
		const bool enterAtA = (nextE->nodeA == exitNId);
		const LaneDir needDir = enterAtA ? LaneDir::Forward : LaneDir::Backward;
		int lane = 0;
		for (int i = 0; i < static_cast<int>(nextE->lanes.size()); ++i)
		{
			if (nextE->lanes[i].dir == needDir &&
				(nextE->lanes[i].op == OpState::Open || nextE->lanes[i].op == OpState::Provisional))
			{ lane = i; break; }
		}
		const float arcPos = enterAtA ? 0.0f : nextE->length;
		const float estTime = nextE->length / Max(1.0f, nextE->speedLimit / kKmhToMps);
		enterEdgeDirect(v, nextEdge, lane, arcPos, estTime);
	}
	else
	{
		enterEdgeDirect(v, nextEdge, 0, 0.0f, kDefaultDormantTime);
	}
	return true;
}

// ===== 車線変更 =====

void VehicleManager::tryLaneChange(Vehicle& v, const SimGraph& simGraph)
{
	if (v.currentEdge == -1) return;
	const auto* edge = simGraph.getEdge(v.currentEdge);
	if (!edge) return;

	const bool fwdLane = isForwardLane(*edge, v.currentLane);
	const IDMParams baseParams = getDefaultIDMParams(v.type, edge->speedLimit);

	const float distToExit = fwdLane ? (edge->length - v.arcPos) : v.arcPos;

	// ===== 1. 経路駆動: targetLane を計算 =====
	const int routeTargetLane = v.goalArc >= 0 && v.currentEdge == v.goalEdgeId
		? v.goalLane : findRouteTargetLane(v, simGraph);
	const int routeNeeded = (routeTargetLane >= 0 && routeTargetLane != v.currentLane)
		? std::abs(routeTargetLane - v.currentLane)
		: 0;

	// 車線変更を確定する小ヘルパー（経路駆動・気まぐれ共通）
	auto beginLaneChange = [&](int targetLane)
	{
		v.location = VehicleLocation::ChangingLane;
		v.laneFrom = v.currentLane;
		v.laneTo   = targetLane;
		v.laneChangeBlend = 0.0f;
		m_laneTraffic.reserve(v, targetLane);
	};

	// ===== 2. 経路駆動車線変更（urgency モデル + 段階的緩和 = 強引モード） =====
	if (routeNeeded > 0)
	{
		// 経路に必要な車線へ間に合わせることを最優先し、出口が近いほど
		// 安全余裕を段階的に緩めてでも寄せに行く。
		const float spaceNeeded = static_cast<float>(routeNeeded) * kLaneChangePerNeedDist;
		const float urgency     = 1.0f - distToExit / spaceNeeded;
		const int   targetLane  = v.currentLane + ((routeTargetLane > v.currentLane) ? +1 : -1);

		// urgency kUrgencyRelaxStart 〜 kUrgencyAggressive: マージンを段階的に縮小して試行
		IDMParams params = baseParams;
		if (urgency >= kUrgencyRelaxStart)
		{
			const float t     = (urgency - kUrgencyRelaxStart) / (kUrgencyAggressive - kUrgencyRelaxStart);
			const float scale = Math::Lerp(1.0f, kUrgencyMinScale, t);
			params.s0 *= scale;
			params.T  *= scale;
		}

		float frontGap, rearGap;
		m_laneTraffic.measureGaps(v.id, v.currentEdge, targetLane,
		            v.arcPos, fwdLane, frontGap, rearGap);
		if (isLaneChangeSafe(*edge, targetLane, fwdLane, frontGap, rearGap, params))
			beginLaneChange(targetLane);
		// 経路駆動を試みた場合はここで終了（気まぐれ変更には進まない）
		return;
	}

	if (v.goalArc >= 0 && v.currentEdge == v.goalEdgeId) { return; }
	// ===== 3. 経路駆動不要: 気まぐれ車線変更（追い越し / キープレフト） =====
	if (!RandomBool(kLaneChangeProbability)) return;
	if (distToExit < kLaneChangeMinExitDist) return;

	// 経路制約がない時だけ、追い越しやキープレフトで交通流をばらけさせる。
	float frontGapCurrent = 1e9f, rearGapDummy = 1e9f;
	m_laneTraffic.measureGaps(v.id, v.currentEdge, v.currentLane,
	            v.arcPos, fwdLane, frontGapCurrent, rearGapDummy);

	auto tryTarget = [&](int targetLane) -> bool
	{
		float frontGap, rearGap;
		m_laneTraffic.measureGaps(v.id, v.currentEdge, targetLane,
		            v.arcPos, fwdLane, frontGap, rearGap);
		if (!isLaneChangeSafe(*edge, targetLane, fwdLane, frontGap, rearGap, baseParams))
			return false;
		beginLaneChange(targetLane);
		return true;
	};

	// キープレフト優先（左へ）
	if (v.currentLane > 0 && tryTarget(v.currentLane - 1))
		return;

	// 追い越し: 前方が詰まっていれば右へ
	if (frontGapCurrent < baseParams.s0 * kRightLaneGapMultiplier)
		tryTarget(v.currentLane + 1);
}

// ===== 信号機 =====

void VehicleManager::buildTrafficLights(const SimGraph& simGraph, const RoadNetwork* network)
{
	m_trafficLights.clear();
	for (const auto& [nid, node] : simGraph.nodes)
	{
		// 信号制御対象のノードだけを拾い、phase は明示定義を最優先、
		// 無ければ道路側の既定定義で補う。
		Array<int> signalEdges;
		for (const int eid : node.edgeIds)
		{
			const auto it = node.edgeControl.find(eid);
			if (it != node.edgeControl.end() && it->second == TrafficControl::Signal)
			{
				if (simGraph.getEdge(eid)) signalEdges << eid;
			}
		}
		if (static_cast<int>(signalEdges.size()) < kMinEdgesForSignal) continue;

		Array<SignalPhase> phases;
		const RoadNode* rn = network ? network->getNode(nid) : nullptr;
		if (rn && rn->signalPlacement && !rn->signalPlacement->phases.isEmpty())
		{
			phases = convertPhaseDefs(rn->signalPlacement->phases);
		}
		else if (rn && network)
		{
			phases = convertPhaseDefs(network->buildDefaultSignalPhases(nid));
		}
		else
		{
			phases = buildAllGreenPhase(node.laneConnections);
		}
		m_trafficLights.emplace(node.id, TrafficLight{ node.id, std::move(phases) });
	}
}

void VehicleManager::rebuildTrafficLightForNode(int nodeId, const SimGraph& simGraph, const RoadNetwork& network)
{
	m_trafficLights.erase(nodeId);

	const SimGraph::Node* node = simGraph.getNode(nodeId);
	if (!node) return;

	Array<int> signalEdges;
	for (const int eid : node->edgeIds)
	{
		const auto it = node->edgeControl.find(eid);
		if (it != node->edgeControl.end() && it->second == TrafficControl::Signal)
		{
			if (simGraph.getEdge(eid)) signalEdges << eid;
		}
	}
	if (static_cast<int>(signalEdges.size()) < kMinEdgesForSignal) return;

	Array<SignalPhase> phases;
	const RoadNode* roadNode = network.getNode(nodeId);
	if (roadNode && roadNode->signalPlacement && !roadNode->signalPlacement->phases.isEmpty())
	{
		phases = convertPhaseDefs(roadNode->signalPlacement->phases);
	}
	else
	{
		phases = convertPhaseDefs(network.buildDefaultSignalPhases(nodeId));
	}

	m_trafficLights.emplace(nodeId, TrafficLight{ nodeId, std::move(phases) });
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

	for (const size_t index : m_laneTraffic.nodeVehicles(nodeId))
	{
		if (m_vehicles[index].id != v.id) { return true; }
	}
	for (const int nearbyEdge : node->edgeIds)
	{
	for (const size_t index : m_laneTraffic.edgeVehicles(nearbyEdge))
	{
		const auto& other = m_vehicles[index];
		if (other.id == v.id) continue;
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

	}
	return false;
}

// ===== 経路リクエスト =====

void VehicleManager::requestRoute(Vehicle& v, const SimGraph& simGraph)
{
	if (v.goalEdgeId < 0)
	{
		if (!assignBuildingGoal(v, simGraph)) { return; }
	}
	if (v.goalEdgeId == v.currentEdge && v.goalArc >= 0) { return; }

	RouteRequest req;
	req.vehicleId = v.id;
	req.startEdge = v.currentEdge;
	req.startLane = v.currentLane;
	req.goalEdge  = v.goalEdgeId;
	req.goalLane = v.goalLane;

	m_pendingRequests << SimRequest{ req };
	v.routeRequested = true;
}
