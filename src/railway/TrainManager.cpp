#include "TrainManager.hpp"

void TrainManager::init(TrainNetwork* network)
{
	m_network = network;
}

void TrainManager::update(double dt, GameTime gameNow)
{
	if (!m_network) return;

	spawnScheduledTrains(gameNow);

	for (auto& t : m_trains)
		updateTrain(t, dt, gameNow);

	// 完走（currentEdge == -1）した列車を削除する
	m_trains.remove_if([](const Train& t) { return t.currentEdge == -1; });
}

void TrainManager::addTrain(Train train)
{
	train.id = m_nextId++;
	m_trains << std::move(train);
}

void TrainManager::updateTrain(Train& t, double dt, GameTime gameNow)
{
	// 駅停車中
	if (t.state == TrainState::WaitingStation)
	{
		t.waitRemaining -= static_cast<float>(dt);
		if (t.waitRemaining <= 0.0f)
		{
			t.waitRemaining = 0.0f;
			t.state = TrainState::Running;
			++t.nextStopIdx;
		}
		return;
	}

	// 信号待ち: 占有解除されるまで待機
	if (t.state == TrainState::WaitingSignal)
	{
		if (t.routeProgress < static_cast<int>(t.routeEdges.size()))
		{
			const int nextEdge = t.routeEdges[t.routeProgress];
			if (m_network->tryOccupy(nextEdge, t.id))
				t.state = TrainState::Running;
		}
		return;
	}

	advanceTrain(t, dt, gameNow);
}

void TrainManager::advanceTrain(Train& t, double dt, GameTime gameNow)
{
	if (t.currentEdge < 0) return;

	const TrackEdge* edge = m_network->getEdge(t.currentEdge);
	if (!edge) { t.currentEdge = -1; return; }

	// 速度制御（簡易 IDM: 目標速度に向かって加速/減速）
	const float vTarget = targetSpeed(t);
	const float dv = vTarget - t.speed;
	if (dv > 0)
		t.speed = Min(t.speed + kMaxAccel * static_cast<float>(dt), vTarget);
	else
		t.speed = Max(t.speed + (-kMaxDecel) * static_cast<float>(dt), vTarget);

	t.speed = Max(t.speed, 0.0f);

	// 弧長を進める
	t.arcPos += t.speed * static_cast<float>(dt);

	// エッジ終端に達したら次のエッジへ
	if (t.arcPos >= edge->length)
	{
		t.arcPos -= edge->length;

		// 現在エッジの占有を解放する
		m_network->releaseOccupy(t.currentEdge, t.id);
		++t.routeProgress;

		if (t.routeProgress >= static_cast<int>(t.routeEdges.size()))
		{
			// 路線の終端に到達
			t.currentEdge = -1;
			return;
		}

		const int nextEdge = t.routeEdges[t.routeProgress];
		if (!m_network->tryOccupy(nextEdge, t.id))
		{
			// 閉塞待ち
			t.state = TrainState::WaitingSignal;
			--t.routeProgress;
			t.arcPos = edge->length - 0.1f;
			t.speed = 0.0f;
			return;
		}
		t.currentEdge = nextEdge;
	}

	// ワールド座標を更新する
	if (const auto bez = m_network->getBezier(t.currentEdge))
	{
		const TrackEdge* e = m_network->getEdge(t.currentEdge);
		const float tParam = (e && e->length > 0) ? (t.arcPos / e->length) : 0.0f;
		const Vec3 pos     = bez->evaluate(Clamp(tParam, 0.0f, 1.0f));
		const Vec3 tangent = bez->tangent(Clamp(tParam, 0.0f, 1.0f)).normalized();

		t.position = pos;
		t.heading  = static_cast<float>(Math::Atan2(tangent.x, tangent.z));
	}

	// 駅停車チェック（スケジュールがある場合）
	if (t.scheduleId >= 0)
	{
		for (const auto& sched : m_network->schedules())
		{
			if (sched.id != t.scheduleId) continue;
			if (t.nextStopIdx >= static_cast<int>(sched.stops.size())) break;

			const StopEntry& stop = sched.stops[t.nextStopIdx];
			const TrackNode* stNode = m_network->getNode(stop.stationNodeId);
			if (!stNode) break;

			// 駅ノードの近くにいるか確認（エッジ端点付近）
			const float dist = static_cast<float>(t.position.distanceFrom(stNode->position));
			if (dist < 15.0f && t.speed < 3.0f)
			{
				t.state        = TrainState::WaitingStation;
				t.speed        = 0.0f;
				t.waitRemaining = stop.dwellSec;
			}
			break;
		}
	}
}

float TrainManager::targetSpeed(const Train& t) const
{
	if (t.currentEdge < 0) return 0.0f;
	const TrackEdge* e = m_network->getEdge(t.currentEdge);
	if (!e) return 0.0f;

	// 残り距離が短くなったら減速する
	const float remaining = e->length - t.arcPos;
	float vLimit = e->speedLimit / 3.6f;

	// 次の閉塞が占有中なら停止する
	if (t.routeProgress + 1 < static_cast<int>(t.routeEdges.size()))
	{
		const int nextEdge = t.routeEdges[t.routeProgress + 1];
		const TrackEdge* next = m_network->getEdge(nextEdge);
		if (next && next->occupiedBy >= 0 && next->occupiedBy != t.id)
		{
			// kBrakeZone 以内なら減速開始
			if (remaining < kBrakeZone)
				vLimit = Min(vLimit, (remaining / kBrakeZone) * vLimit);
		}
	}

	return Min(t.speed + kMaxAccel * 0.016f, vLimit);  // 1フレーム先読み
}

void TrainManager::spawnScheduledTrains(GameTime gameNow)
{
	for (auto& sched : m_network->schedules())
	{
		if (sched.stops.isEmpty()) continue;
		if (gameNow - sched.lastSpawnAt < sched.headwaySec) continue;

		// 先頭駅のノードに最も近いエッジを探して始発列車を生成する
		const int firstStationId = sched.stops[0].stationNodeId;
		const TrackNode* firstStation = m_network->getNode(firstStationId);
		if (!firstStation || firstStation->edgeIds.isEmpty()) continue;

		const int startEdge = firstStation->edgeIds[0];
		if (!m_network->tryOccupy(startEdge, m_nextId)) continue;

		Train train;
		train.type        = TrainType::Local;
		train.currentEdge = startEdge;
		train.arcPos      = 0.0f;
		train.speed       = 0.0f;
		train.state       = TrainState::Running;
		train.scheduleId  = sched.id;
		train.nextStopIdx = 0;
		train.departedAt  = gameNow;

		// ルートはスケジュールの駅間エッジ列（簡易: 登録順のエッジを使う）
		for (const auto& stop : sched.stops)
		{
			const TrackNode* n = m_network->getNode(stop.stationNodeId);
			if (n)
				for (int eid : n->edgeIds)
					train.routeEdges << eid;
		}
		train.routeEdges.stable_unique();

		addTrain(std::move(train));
		sched.lastSpawnAt = gameNow;
	}
}
