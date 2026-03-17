#include "TrafficManager.hpp"
#include "../world/World.hpp"
#include "../zone/ZoneManager.hpp"
#include <cmath>

// ===== init =====

void TrafficManager::init(RoadNetwork* network, World* world, ZoneManager* zoneManager)
{
	m_network     = network;
	m_world       = world;
	m_zoneManager = zoneManager;
	m_graphDirty  = true;
}

// ===== update =====

void TrafficManager::update(double dt, GameTime gameNow)
{
	m_lastGameNow = gameNow;

	// 期限切れ TempOp を削除し、変化があればグラフを再構築する
	if (m_network->clearExpiredTempOps(gameNow))
		m_graphDirty = true;

	// グラフが更新された場合は再構築する（信号機を先にビルドしてからグラフに反映する）
	if (m_graphDirty)
	{
		buildTrafficLights();
		rebuildGraph(gameNow);
		m_graphDirty = false;
	}

	// 信号機を更新する
	updateTrafficLights(gameNow);

	// バス路線を更新する（時刻表に従いバスを生成する）
	updateBusRoutes(gameNow);

	// 再探索キューを処理する
	processRerouteQueue(gameNow);

	// 各車両を更新する
	for (auto& v : m_vehicles)
		updateVehicle(v, dt, gameNow);

	// 目的地に到達した（currentEdge == -1）車両を削除する
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
	if (!m_network) return;

	// Forward 方向の走行可能な車線があるエッジを候補として収集する
	Array<int> candidates;
	for (const auto& e : m_network->edges())
	{
		if (e.id == -1) continue;
		for (const auto& lane : e.lanes)
		{
			if (isPassable(lane) && lane.dir == LaneDir::Forward)
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

	// ゾーン別・時間帯別車種選択（World と ZoneManager が利用可能な場合）
	if (m_world && m_zoneManager)
	{
		Vec3 spawnPos = Vec3::Zero();
		if (const auto bezier = m_network->getBezier(edgeId))
			spawnPos = bezier->positionAt(bezier->totalLength * 0.5f);

		const ZoneType zone = m_zoneManager->getZone(*m_world, spawnPos);
		const float hour = static_cast<float>(
			std::fmod(m_lastGameNow, 86400.0) / 3600.0);
		v.type = selectVehicleType(zone, hour);
	}
	else
	{
		v.type = type;
	}

	// 初期位置をランダムに設定する（車両が重ならないようにする）
	if (const auto bezier = m_network->getBezier(edgeId))
	{
		v.arcPos   = static_cast<float>(Random(0.0, static_cast<double>(bezier->totalLength) * 0.8));
		v.position = bezier->positionAt(v.arcPos);
	}

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
	if (!m_network) return;
	m_graph.rebuild(*m_network, now, m_trafficLights);

	// グラフ再構築後に全車両を再探索キューへ追加する
	for (const auto& v : m_vehicles)
		enqueueReroute(v.id);
}

// ===== ヘルパー: 車線方向を返す =====

static bool isForwardLane(const RoadEdge& edge, int laneIdx)
{
	if (laneIdx < 0 || laneIdx >= static_cast<int>(edge.lanes.size()))
		return true;
	return edge.lanes[laneIdx].dir == LaneDir::Forward;
}

// ===== 車両更新 =====

void TrafficManager::updateVehicle(Vehicle& v, double dt, GameTime gameNow)
{
	if (v.currentEdge == -1) return;
	if (!m_network->getEdge(v.currentEdge)) { v.currentEdge = -1; return; }

	// バス停待機中は待機タイマーを消化して復帰する
	if (v.state == VehicleState::WaitingBusStop)
	{
		v.busWaitRemaining -= static_cast<float>(dt);
		if (v.busWaitRemaining <= 0.0f)
		{
			v.busWaitRemaining = 0.0f;
			v.state = VehicleState::Moving;
			// 次のバス停インデックスを進める
			++v.busNextStopIdx;
		}
		return;
	}

	// 車線変更の試行（確率的に実行してフレームごとの処理負荷を分散する）
	if (RandomBool(0.01))
		tryLaneChange(v);

	// バス路線の停車チェック（移動前に実施）
	if (v.type == VehicleType::Bus && v.busRouteId >= 0)
		updateBusStop(v, dt, gameNow);
	if (v.state == VehicleState::WaitingBusStop) return;

	advanceOnEdge(v, dt, gameNow);

	// 渋滞検知による再探索判定
	if (v.speed < v.rerouteSpeedThreshold)
	{
		if (RandomBool(0.005))  // 低速時に 0.5% の確率で再探索
			enqueueReroute(v.id);
	}

	// 定期再探索
	if (static_cast<float>(gameNow - v.lastReroute) > kPeriodicRerouteInterval)
		enqueueReroute(v.id);
}

void TrafficManager::advanceOnEdge(Vehicle& v, double dt, GameTime gameNow)
{
	const RoadEdge* edge = m_network->getEdge(v.currentEdge);
	if (!edge) return;

	const bool fwdLane = isForwardLane(*edge, v.currentLane);

	// IDM 加速度を計算する
	const IDMParams params = getDefaultIDMParams(v.type, edge->speedLimit);
	float accel = idmAcceleration(v, params, fwdLane);

	// 信号停止チェック（終端付近で赤なら停止線へ向かう）
	const int exitNId = fwdLane ? edge->nodeB : edge->nodeA;
	const RoadNode* exitRoadNode = m_network->getNode(exitNId);
	if (exitRoadNode)
	{
		const TrafficLight* tl = getTrafficLight(exitRoadNode->id);
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
	}

	// 速度を更新する（0 以上 v0 以下にクランプ）
	v.speed = static_cast<float>(Clamp(
		static_cast<double>(v.speed) + accel * dt,
		0.0, static_cast<double>(params.v0)));

	// 弧長位置を更新する（Forward = 増加, Backward = 減少）
	const float advance = v.speed * static_cast<float>(dt);
	if (fwdLane)
		v.arcPos += advance;
	else
		v.arcPos -= advance;

	// ワールド座標と向きを更新する
	if (const auto bezier = m_network->getBezier(v.currentEdge))
	{
		const float clampedArc = Clamp(v.arcPos, 0.0f, bezier->totalLength);
		v.position = bezier->positionAt(clampedArc);
		const Vec3 tangent = bezier->tangentAt(clampedArc);
		// Backward 方向の場合は向きを反転する
		const float sign = fwdLane ? 1.0f : -1.0f;
		v.heading = static_cast<float>(Math::Atan2(sign * tangent.x, sign * tangent.z));
	}

	// 終端到達判定
	const bool reachedEnd = fwdLane
		? (v.arcPos >= edge->length)
		: (v.arcPos <= 0.0f);

	if (reachedEnd)
		transitToNextEdge(v, gameNow);
}

bool TrafficManager::transitToNextEdge(Vehicle& v, GameTime gameNow)
{
	// ルートが残っているならルートに従って遷移する
	while (v.routeProgress < static_cast<int>(v.routeNodeIds.size()))
	{
		const int nodeId = v.routeNodeIds[v.routeProgress++];
		const LaneNode* ln = m_graph.getLaneNode(nodeId);
		if (!ln) continue;

		// 異なるエッジへの遷移ノードを見つけたら遷移する
		if (ln->edgeId != v.currentEdge)
		{
			v.currentEdge = ln->edgeId;
			v.currentLane = ln->laneIndex;
			v.arcPos      = ln->arcPos;
			v.speed      *= 0.8f;  // 交差点での減速
			return true;
		}
	}

	// ルートが終了した場合: 次のゴールを割り当てて再探索する
	v.goalEdgeId = -1;
	enqueueReroute(v.id);

	// フォールバック: ランダムに次エッジを選ぶ
	const RoadEdge* edge = m_network->getEdge(v.currentEdge);
	if (!edge) { v.currentEdge = -1; return false; }

	const int exitNId = isForwardLane(*edge, v.currentLane) ? edge->nodeB : edge->nodeA;
	const RoadNode* exitRoadNode = m_network->getNode(exitNId);
	if (!exitRoadNode) { v.currentEdge = -1; return false; }

	Array<int> candidates;
	for (const int eid : exitRoadNode->edgeIds)
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

	// 同一エッジ・同一車線上の先行車を探す
	for (const auto& other : m_vehicles)
	{
		if (other.id == v.id)           continue;
		if (other.currentEdge != v.currentEdge) continue;
		if (other.currentLane != v.currentLane) continue;

		// 進行方向に「前」にいる車両を探す
		const float delta = fwdLane
			? (other.arcPos - v.arcPos)
			: (v.arcPos - other.arcPos);

		if (delta > 0.0f && delta < gap)
		{
			gap   = delta;
			vLead = other.speed;
		}
	}

	// 自由走行（先行車なし）
	if (gap > 500.0f)
	{
		return params.aMax * (1.0f - std::powf(v.speed / Max(0.1f, params.v0), 4.0f));
	}

	// IDM 加速度計算
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
	// ゴールエッジを割り当てる
	if (v.goalEdgeId == -1 || v.goalEdgeId == v.currentEdge)
	{
		Array<int> candidates;
		for (const auto& e : m_network->edges())
		{
			if (e.id != -1 && e.id != v.currentEdge)
				candidates << e.id;
		}
		if (candidates.isEmpty()) return;
		v.goalEdgeId = candidates[Random(0, static_cast<int>(candidates.size()) - 1)];
	}

	// 現在車線の入口 LaneNode を取得する
	int startNode = m_graph.entryNodeId(v.currentEdge, v.currentLane);

	// 入口ノードが存在しない場合は他の車線を試みる
	if (startNode == -1)
	{
		const RoadEdge* e = m_network->getEdge(v.currentEdge);
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
		v.routeProgress = 1;  // 出発ノード自身はスキップする
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
	if (!m_network) return;

	for (const auto& node : m_network->nodes())
	{
		if (node.id == -1) continue;

		// エッジが 3 本以上ある交差点に信号機を設置する
		int validEdges = 0;
		for (const int eid : node.edgeIds)
		{
			if (m_network->getEdge(eid)) ++validEdges;
		}
		if (validEdges < 3) continue;

		// 2 フェーズの信号: 前半エッジ群 / 後半エッジ群
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
	const RoadEdge* edge = m_network->getEdge(v.currentEdge);
	if (!edge) return;

	const bool fwdLane = isForwardLane(*edge, v.currentLane);
	const IDMParams params = getDefaultIDMParams(v.type, edge->speedLimit);

	// 交差点に近い場合は車線変更を禁止する
	const float distToExit = fwdLane ? (edge->length - v.arcPos) : v.arcPos;
	if (distToExit < 30.0f) return;

	// 現在車線の前方ギャップを測る（追い越しの必要性を判断する）
	float frontGapCurrent = 1e9f;
	for (const auto& other : m_vehicles)
	{
		if (other.id == v.id) continue;
		if (other.currentEdge != v.currentEdge || other.currentLane != v.currentLane) continue;
		const float delta = fwdLane ? (other.arcPos - v.arcPos) : (v.arcPos - other.arcPos);
		if (delta > 0.0f && delta < frontGapCurrent)
			frontGapCurrent = delta;
	}

	// 対象車線への車線変更が安全かチェックする
	const auto isSafe = [&](int targetLane) -> bool
	{
		if (targetLane < 0 || targetLane >= static_cast<int>(edge->lanes.size())) return false;
		const Lane tgt = edge->effectiveLane(targetLane, m_lastGameNow);
		if (!isPassable(tgt)) return false;
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

		// 安全ギャップ判定: 前方 s0+8m、後方 s0+T×ΔvMax+8m
		constexpr float kDeltaVMax   = 15.0f;
		const float     safetyFront  = params.s0 + 8.0f;
		const float     safetyRear   = params.s0 + params.T * kDeltaVMax + 8.0f;
		return (frontGap >= safetyFront) && (rearGap >= safetyRear);
	};

	// キープレフト: 左車線（idx-1）へ戻れるなら優先して戻る
	if (v.currentLane > 0 && isSafe(v.currentLane - 1))
	{
		v.currentLane--;
		return;
	}

	// 前方が詰まっている場合のみ右車線（idx+1）への追い越しを試みる
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

		// バスを先頭バス停に生成する
		const int firstStopId = route.stopIds[0];
		const BusStop* stop = nullptr;
		for (const auto& s : m_busStops)
		{
			if (s.id == firstStopId) { stop = &s; break; }
		}
		if (!stop) continue;

		// 近傍エッジを探してバスを生成する
		if (stop->edgeId >= 0)
		{
			Vehicle bus;
			bus.type         = VehicleType::Bus;
			bus.currentEdge  = stop->edgeId;
			bus.arcPos       = stop->arcPos;
			bus.speed        = 0.0f;
			bus.busRouteId   = route.id;
			bus.busNextStopIdx = 1;  // 次は index=1 のバス停へ
			addVehicle(std::move(bus));
		}
		route.lastSpawnAt = gameNow;
	}
}

void TrafficManager::updateBusStop(Vehicle& v, double dt, GameTime gameNow)
{
	if (v.busRouteId < 0) return;

	// 対象路線を探す
	BusRoute* route = nullptr;
	for (auto& r : m_busRoutes)
	{
		if (r.id == v.busRouteId) { route = &r; break; }
	}
	if (!route || route->stopIds.isEmpty()) return;

	// 次のバス停インデックスが終端を超えたら目的地到達と見なして消す
	if (v.busNextStopIdx >= static_cast<int>(route->stopIds.size()))
	{
		v.currentEdge = -1;  // 削除マーク
		return;
	}

	const int nextStopId = route->stopIds[v.busNextStopIdx];
	const BusStop* nextStop = nullptr;
	for (const auto& s : m_busStops)
	{
		if (s.id == nextStopId) { nextStop = &s; break; }
	}
	if (!nextStop) return;

	// 同じエッジ上で停車位置が近ければ停車する
	if (v.currentEdge == nextStop->edgeId)
	{
		const float dist = Abs(v.arcPos - nextStop->arcPos);
		if (dist < 8.0f && v.speed < 2.0f)
		{
			// バス停に到着 → 待機開始
			v.state            = VehicleState::WaitingBusStop;
			v.speed            = 0.0f;
			v.busWaitRemaining = 5.0f;  // 5 ゲーム秒停車
			v.arcPos           = nextStop->arcPos;
		}
	}
}

// ===== 車種選択 =====

VehicleType TrafficManager::selectVehicleType(ZoneType zone, float hour) const
{
	// ゾーン別基本比率: {PassengerCar, KeiCar, Moped, LightVehicle, SmallTruck, LargeTruck}
	float base[6];
	switch (zone)
	{
	case ZoneType::LowResidential:
		base[0]=30; base[1]=30; base[2]=20; base[3]=10; base[4]=10; base[5]= 0; break;
	case ZoneType::Residential:
		base[0]=40; base[1]=30; base[2]=15; base[3]= 5; base[4]=10; base[5]= 0; break;
	case ZoneType::Commercial:
		base[0]=40; base[1]=20; base[2]=10; base[3]= 5; base[4]=20; base[5]= 5; break;
	case ZoneType::Industrial:
		base[0]=15; base[1]=10; base[2]= 5; base[3]= 0; base[4]=40; base[5]=30; break;
	case ZoneType::Agriculture:
		base[0]=20; base[1]=35; base[2]=20; base[3]=15; base[4]=10; base[5]= 0; break;
	default: // Unzoned, UrbanControl
		base[0]=35; base[1]=25; base[2]=15; base[3]=10; base[4]=15; base[5]= 0; break;
	}

	// 時間帯別スケール（§7）
	float scCar, scMoped, scTruck;
	if      (hour >=  6.0f && hour <  9.0f) { scCar=1.7f; scMoped=1.3f; scTruck=0.8f; }
	else if (hour >=  9.0f && hour < 17.0f) { scCar=1.0f; scMoped=1.0f; scTruck=1.5f; }
	else if (hour >= 17.0f && hour < 20.0f) { scCar=1.5f; scMoped=1.2f; scTruck=0.6f; }
	else if (hour >= 20.0f && hour < 23.0f) { scCar=0.5f; scMoped=0.3f; scTruck=0.8f; }
	else                                     { scCar=0.1f; scMoped=0.05f; scTruck=1.2f; }

	// スケール適用後の重みを計算する
	const float w[6] =
	{
		base[0] * scCar,    // PassengerCar
		base[1] * scCar,    // KeiCar
		base[2] * scMoped,  // Moped
		base[3] * scMoped,  // LightVehicle
		base[4] * scTruck,  // SmallTruck
		base[5] * scTruck,  // LargeTruck
	};

	float total = 0.0f;
	for (const float x : w) total += x;
	if (total <= 0.0f) return VehicleType::PassengerCar;

	float r = Random(0.0f, total);
	static const VehicleType kTypes[6] =
	{
		VehicleType::PassengerCar,
		VehicleType::KeiCar,
		VehicleType::Moped,
		VehicleType::LightVehicle,
		VehicleType::SmallTruck,
		VehicleType::LargeTruck,
	};
	for (int k = 0; k < 6; ++k)
	{
		r -= w[k];
		if (r <= 0.0f) return kTypes[k];
	}
	return VehicleType::PassengerCar;
}
