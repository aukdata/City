#include "VehicleManager.hpp"
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

void VehicleManager::init(const SimGraph& simGraph, const RoadNetwork& network)
{
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
	m_lightsDirty = false;
}

// ===== スポーン =====

void VehicleManager::spawnRandom(const SimGraph& simGraph, VehicleType type)
{
	// 交通量が自然に残るよう、走行可能エッジからランダムに初期配置する。
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
	v.departedAt  = m_lastGameNow;
	if (edge)
		v.arcPos = static_cast<float>(Random(0.0, static_cast<double>(edge->length) * kSpawnPosRatio));

	m_vehicles << std::move(v);
}

void VehicleManager::spawnOnEdge(int edgeId, const SimGraph& simGraph, VehicleType type, int goalEdgeId)
{
	const auto* edge = simGraph.getEdge(edgeId);
	if (!edge) { Console << U"[spawnOnEdge] edge not found: " << edgeId; return; }

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
	if (laneIdx < 0) { Console << U"[spawnOnEdge] no drivable lane on edge " << edgeId; return; }

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

	Console << U"[spawnOnEdge] id=" << v.id << U" edge=" << edgeId
		<< U" lane=" << laneIdx << U" dir=" << (isForwardLane(*edge, laneIdx) ? U"Fwd" : U"Bwd")
		<< U" arc=" << v.arcPos << U"/" << edge->length
		<< U" speed=" << v.speed << U" goal=" << goalEdgeId;

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
			// 経路探索成功時は経路全体を差し替え、失敗状態もここで解消する。
			v.routeWaypoints = resp.waypoints;
			v.routeIdx = 0;
			v.routeFailCount = 0;
		}
		else
		{
			// 経路探索失敗 → 5回連続失敗で削除、そうでなければゴールをリセットして再試行
			++v.routeFailCount;
			if (v.routeFailCount >= 5)
				v.currentEdge = -1;
			else
				v.goalEdgeId = -1;
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
	m_lastGameNow = gameNow;
	if (m_trafficDemandConfigured)
	{
		m_targetVehicleCount = Clamp(static_cast<int>(Math::Round(
			m_trafficDemand.targetVehicleCount * m_eventDemandMultiplier)), 0, 600);
	}

	const auto t0 = Clock::now();

	// フレーム冒頭で台数補充と信号キャッシュ更新を済ませ、以降の車両更新は
	// 同じネットワーク状態を前提に進める。
	const int demandVehicleCount = static_cast<int>(m_vehicles.count_if([](const Vehicle& vehicle)
	{
		return vehicle.type != VehicleType::Bus && vehicle.type != VehicleType::Emergency;
	}));
	if (demandVehicleCount < m_targetVehicleCount)
		spawnRandom(simGraph, selectDemandVehicleType());

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
		// 削除予定車両や破綻した参照を早めに整理しつつ、可視範囲の車両だけを
		// Active として精密更新し、不可視車両は Dormant 近似へ落とす。
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
			if (v.location == VehicleLocation::OnLane)
				tryLaneChange(v, simGraph);
			lcTotal += toMs(Clock::now() - lcStart);

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
			updateDormantVehicle(v, dt * m_globalSpeedMultiplier);
		}

		// 目的地エッジを実際に走り切った車両だけを完了履歴へ記録する。
		if (v.tripCompleted)
		{
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

	// 通常走行ではまず IDM で基本加速度を決め、その後で信号や停止規制を重ねて
	// 交差点手前の減速や停止状態への遷移を制御する。
	const bool fwdLane = isForwardLane(*edge, v.currentLane);
	const IDMParams params = getDefaultIDMParams(v.type,
		static_cast<float>(edge->speedLimit * m_globalSpeedMultiplier));
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
					accel = *a;
			}
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
						enterConnection(v, exitNId, conn.id);
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
		enterConnection(v, exitNId, conn->id);
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

// ===== Dormant 車両更新 =====

void VehicleManager::updateDormantVehicle(Vehicle& v, double dt)
{
	// Dormant 中は見えない区間をエッジ単位の通過時間で近似し、タイマー満了ごとに
	// 次ウェイポイントへ進める。
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
				}
				else
				{
					v.tripCompleted = true;
				}
			}
		}
	}
}

// ===== Active/Dormant 遷移 =====

void VehicleManager::activateVehicle(Vehicle& v, const SimGraph& simGraph)
{
	v.mode = VehicleMode::Active;

	const auto* edge = simGraph.getEdge(v.currentEdge);
	if (!edge || v.dormantTotalTime <= 0.0f) return;

	// Dormant 中に進んだ割合を arcPos に反映し、可視化時の位置飛びを抑える。
	const float fraction = Clamp(
		(v.dormantTotalTime - v.dormantTimer) / v.dormantTotalTime, 0.0f, 1.0f);
	const bool fwd = isForwardLane(*edge, v.currentLane);
	const float target = fwd ? edge->length : 0.0f;
	v.arcPos = v.arcPos + fraction * (target - v.arcPos);
	v.speed  = edge->speedLimit / kKmhToMps * kActivationSpeedFactor;
}

void VehicleManager::deactivateVehicle(Vehicle& v, const SimGraph& simGraph)
{
	v.mode = VehicleMode::Dormant;

	const auto* edge = simGraph.getEdge(v.currentEdge);
	if (!edge)
	{
		v.dormantTimer = v.dormantTotalTime = kDefaultDormantTime;
		return;
	}

	// 不可視化した時点で、詳細な追従状態を捨ててエッジ通過時間へ圧縮する。
	const bool fwd = isForwardLane(*edge, v.currentLane);
	const float remaining = fwd ? (edge->length - v.arcPos) : v.arcPos;
	const float speedMs   = Max(1.0f, edge->speedLimit / kKmhToMps);
	v.dormantTimer     = remaining / speedMs;
	v.dormantTotalTime = edge->length / speedMs;
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
	const int routeTargetLane = findRouteTargetLane(v, simGraph);
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
	};

	// ===== 2. 経路駆動車線変更（urgency モデル + 段階的緩和 = 強引モード） =====
	if (routeNeeded > 0)
	{
		// 経路に必要な車線へ間に合わせることを最優先し、出口が近いほど
		// 安全余裕を段階的に緩めてでも寄せに行く。
		const float spaceNeeded = static_cast<float>(routeNeeded) * kLaneChangePerNeedDist;
		const float urgency     = 1.0f - distToExit / spaceNeeded;
		const int   targetLane  = v.currentLane + ((routeTargetLane > v.currentLane) ? +1 : -1);

		// 距離不足 or 強引モード: 安全チェックスキップで割り込み
		// （後続車は次フレームから自車を新しい前車として IDM 追従し急減速する）
		if (urgency > kUrgencyForce || urgency >= kUrgencyAggressive)
		{
			beginLaneChange(targetLane);
			return;
		}

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
		measureGaps(m_vehicles, v.id, v.currentEdge, targetLane,
		            v.arcPos, fwdLane, true, frontGap, rearGap);
		if (isLaneChangeSafe(*edge, targetLane, fwdLane, frontGap, rearGap, params))
			beginLaneChange(targetLane);
		// 経路駆動を試みた場合はここで終了（気まぐれ変更には進まない）
		return;
	}

	// ===== 3. 経路駆動不要: 気まぐれ車線変更（追い越し / キープレフト） =====
	if (!RandomBool(kLaneChangeProbability)) return;
	if (distToExit < kLaneChangeMinExitDist) return;

	// 経路制約がない時だけ、追い越しやキープレフトで交通流をばらけさせる。
	float frontGapCurrent = 1e9f, rearGapDummy = 1e9f;
	measureGaps(m_vehicles, v.id, v.currentEdge, v.currentLane,
	            v.arcPos, fwdLane, true, frontGapCurrent, rearGapDummy);

	auto tryTarget = [&](int targetLane) -> bool
	{
		float frontGap, rearGap;
		measureGaps(m_vehicles, v.id, v.currentEdge, targetLane,
		            v.arcPos, fwdLane, true, frontGap, rearGap);
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

	// Yield 判定では、そのノードへ進入しそうな他車が近くにいるかを保守的に見る。
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
		// 目的地未設定や自己目的地は無意味なので、ランダムな到達先を引き直す。
		const int goal = selectRandomGoalEdge(simGraph, v.currentEdge);
		Console << U"[requestRoute] v=" << v.id << U" selectRandomGoal=" << goal;
		if (goal == -1) return;
		v.goalEdgeId = goal;
	}

	RouteRequest req;
	req.vehicleId = v.id;
	req.startEdge = v.currentEdge;
	req.startLane = v.currentLane;
	req.goalEdge  = v.goalEdgeId;
	Console << U"[requestRoute] v=" << v.id << U" start=" << req.startEdge << U" goal=" << req.goalEdge;

	m_pendingRequests << SimRequest{ req };
	v.routeRequested = true;
}
