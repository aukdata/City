#include "TrainManager.hpp"
#include "TrainNetwork.hpp"
#include "TrainConsist.hpp"
#include "RailTimetable.hpp"
#include "../debug/DebugLog.hpp"

namespace
{
	/// @brief 停車順を実際の運行方向へ変換する。呼び出し側が範囲を検査する。
	const StopEntry& serviceStop(const TrainSchedule& schedule, size_t index, bool reverse)
	{
		return schedule.stops[reverse ? schedule.stops.size() - 1 - index : index];
	}

	float brakingSpeed(float distance, float deceleration)
	{
		// 駅端点まで到達させるため、制動曲線の終端にも小さな速度を残す。
		constexpr float kMinimumSpeedSquared = 0.01f;
		return Math::Sqrt(Max(kMinimumSpeedSquared, 2 * deceleration * distance));
	}
}

void TrainManager::init(TrainNetwork* network)
{
	m_network = network;
	m_trains.clear();
	m_nextId = 0;
	m_stopEvents.clear();
	if (!m_network) { return; }
	for (const auto& edge : m_network->edges())
	{
		if (auto* current = m_network->getEdge(edge.id)) { current->occupiedBy = -1; for (auto& lane : current->lanes) { lane.reservedBy = -1; } }
	}
}

void TrainManager::finishService(Train& train)
{
	for (const int id : train.routeEdges) { m_network->releaseOccupy(id, train.id); }
	if (m_passengerEventsEnabled) { m_stopEvents << StopEvent{train.id,-1,train.position,true}; }
	train.currentEdge = -1;
}

void TrainManager::updatePosition(Train& train)
{
	if (const auto curve = m_network->getBezier(train.currentEdge))
	{
		const float arc = train.forward ? train.arcPos : curve->totalLength - train.arcPos;
		const auto* edge = m_network->getEdge(train.currentEdge);
		const int lane = TransportCrossSection::railLane(*edge,train.forward);
		if (lane < 0) { finishService(train); return; }
		train.position = TransportCrossSection::lanePosition(*edge,*curve,arc,lane);
		const Vec3 tangent = curve->tangentAt(arc) * (train.forward ? 1 : -1);
		train.heading = static_cast<float>(Math::Atan2(tangent.x, tangent.z));
	}
}

void TrainManager::addTrain(Train train)
{
	train.id = m_nextId++;
	m_trains << std::move(train);
}

void TrainManager::update(double dt, GameTime now)
{
	if (!m_network || dt <= 0) { return; }
	spawnScheduledTrains(now);
	for (double remaining = dt; remaining > 0; remaining -= Min(kMaximumStep, remaining))
	{
		const double step = Min(kMaximumStep, remaining);
		for (auto& train : m_trains)
		{
			if (train.currentEdge >= 0) { updateTrain(train, step); }
		}
	}
	m_trains.remove_if([](const Train& train) { return train.currentEdge < 0; });
}

void TrainManager::updateTrain(Train& train, double dt)
{
	if (train.state == TrainState::WaitingStation)
	{
		train.waitRemaining -= static_cast<float>(dt);
		if (train.waitRemaining > 0) { return; }
		train.waitRemaining = 0;
		++train.nextStopIdx;
		train.state = TrainState::Running;
	}
	if (train.state == TrainState::WaitingSignal)
	{
		if (train.routeProgress + 1 >= static_cast<int>(train.routeEdges.size())) { return; }
		const auto* next = m_network->getEdge(train.routeEdges[train.routeProgress + 1]);
		const auto* current = m_network->getEdge(train.currentEdge);
		if (!current || !next) { finishService(train); return; }
		const int end = train.forward ? current->nodeB : current->nodeA;
		if (!m_network->canOccupy(next->id,train.id,next->nodeA == end)) { return; }
		train.state = TrainState::Running;
	}
	advanceTrain(train, dt);
}

Optional<StopEntry> TrainManager::nextStop(const Train& train) const
{
	if (train.nextStopIdx >= 0 && train.nextStopIdx < static_cast<int>(train.serviceStops.size()))
	{
		return train.serviceStops[train.nextStopIdx];
	}
	return none;
}

void TrainManager::advanceTrain(Train& train, double dt)
{
	if (!m_network->getEdge(train.currentEdge) || TransportCrossSection::railLane(*m_network->getEdge(train.currentEdge),train.forward) < 0) { finishService(train); return; }
	const float target = targetSpeed(train);
	const float step = static_cast<float>(dt);
	train.speed = target > train.speed
		? Min(target, train.speed + kMaxAccel * step)
		: Max(target, train.speed - kMaxDecel * step);
	float travel = Max(0.0f, train.speed * step);
	for (;;)
	{
		const auto* edge = m_network->getEdge(train.currentEdge);
		const int end = train.forward ? edge->nodeB : edge->nodeA;
		const float remaining = Max(0.0f, edge->length - train.arcPos);
		const float move = Min(remaining, travel);
		train.arcPos += move;
		travel -= move;
		if (train.arcPos < edge->length - kArrivalTolerance) { break; }
		train.arcPos = edge->length;

		if (const auto stop = nextStop(train); stop && stop->stationNodeId == end)
		{
			train.state = TrainState::WaitingStation;
			train.speed = 0;
			train.waitRemaining = stop->dwellSec;
			if (m_passengerEventsEnabled) { m_stopEvents << StopEvent{train.id,end,m_network->getNode(end)->position}; }
			break;
		}
		if (train.routeProgress + 1 >= static_cast<int>(train.routeEdges.size()))
		{
			finishService(train);
			return;
		}
		const int nextId = train.routeEdges[train.routeProgress + 1];
		const auto* next = m_network->getEdge(nextId);
		if (!next || (next->nodeA != end && next->nodeB != end))
		{
			finishService(train);
			return;
		}
		if (!m_network->tryOccupy(nextId, train.id, next->nodeA == end))
		{
			train.state = TrainState::WaitingSignal;
			train.speed = 0;
			break;
		}
		train.currentEdge = nextId;
		++train.routeProgress;
		train.forward = next->nodeA == end;
		train.arcPos = 0;
		if (travel <= 0) { break; }
	}
	updatePosition(train);
}

float TrainManager::targetSpeed(const Train& train) const
{
	const auto* current = m_network->getEdge(train.currentEdge);
	if (!current) { return 0; }
	constexpr float kKmhPerMetrePerSecond = 3.6f;
	float speed = Min(current->speedLimit, TrainConsist::profile(train.type).maximumSpeed) / kKmhPerMetrePerSecond;
	const auto stop = nextStop(train);
	float distance = -train.arcPos;
	int start = train.forward ? current->nodeA : current->nodeB;
	for (int index = train.routeProgress; index < static_cast<int>(train.routeEdges.size()); ++index)
	{
		const auto* edge = m_network->getEdge(train.routeEdges[index]);
		if (!edge) { break; }
		if (index > train.routeProgress && !m_network->canOccupy(edge->id,train.id,edge->nodeA == start))
		{
			speed = Min(speed, brakingSpeed(distance, kMaxDecel));
			break;
		}
		if (index > train.routeProgress)
		{
			const float limit = Min(edge->speedLimit, TrainConsist::profile(train.type).maximumSpeed) / kKmhPerMetrePerSecond;
			speed = Min(speed, Math::Sqrt(limit * limit + 2 * kMaxDecel * Max(0.0f, distance)));
		}
		distance += edge->length;
		start = edge->nodeA == start ? edge->nodeB : edge->nodeA;
		if (stop && start == stop->stationNodeId)
		{
			speed = Min(speed, brakingSpeed(distance, kMaxDecel));
			break;
		}
		if (distance > kLookAheadDistance) { break; }
	}
	return speed;
}

Array<int> TrainManager::buildServiceRoute(const TrainSchedule& schedule) const
{
	if (!RailTimetable::validate(*m_network, schedule).isEmpty()) { return {}; }
	return RailTimetable::route(*m_network, schedule, schedule.reverseNext);
}

bool TrainManager::canReserveRoute(const Array<int>& route, TrainType type, int origin) const
{
	if (route.isEmpty()) { return false; }
	float length = 0;
	for (const int id : route)
	{
		const auto* edge = m_network->getEdge(id);
		if (!edge || !edge->electrified || !m_network->canOccupy(id,m_nextId,edge->nodeA == origin)) { return false; }
		origin = edge->nodeA == origin ? edge->nodeB : edge->nodeA;
		length += edge->length;
	}
	return length > TrainConsist::length(type) + kDepartureClearance;
}

Train TrainManager::makeScheduledTrain(const TrainSchedule& schedule, Array<int> route, GameTime now) const
{
	Train train;
	train.type = schedule.type;
	train.serviceStops = schedule.stops;
	if (schedule.reverseNext) { train.serviceStops.reverse(); }
	train.reverseService = schedule.reverseNext;
	train.currentEdge = route.front();
	train.routeEdges = std::move(route);
	const int origin = serviceStop(schedule, 0, schedule.reverseNext).stationNodeId;
	train.forward = m_network->getEdge(train.currentEdge)->nodeA == origin;
	train.position = m_network->getNode(origin)->position;
	train.nextStopIdx = 1;
	train.scheduleId = schedule.id;
	train.departedAt = now;
	// 編成全体を線路に収めて出発する。車両を線路外へ外挿しない。
	float departureArc = TrainConsist::length(train.type);
	while (departureArc > m_network->getEdge(train.currentEdge)->length)
	{
		const auto* edge = m_network->getEdge(train.currentEdge);
		departureArc -= edge->length;
		const int end = train.forward ? edge->nodeB : edge->nodeA;
		train.currentEdge = train.routeEdges[++train.routeProgress];
		train.forward = m_network->getEdge(train.currentEdge)->nodeA == end;
	}
	train.arcPos = departureArc;
	return train;
}

void TrainManager::spawnScheduledTrains(GameTime now)
{
	for (auto& schedule : m_network->schedules())
	{
		if (!RailTimetable::dueDeparture(schedule, now)) { continue; }
		auto route = buildServiceRoute(schedule);
		int origin = serviceStop(schedule,0,schedule.reverseNext).stationNodeId;
		if (!canReserveRoute(route, schedule.type, origin)) { continue; }
		// 単線は両方向で共有、複線は進行方向の軌道だけを予約する。
		for (const int id : route)
		{
			const auto* edge = m_network->getEdge(id); const bool forward = edge->nodeA == origin;
			m_network->tryOccupy(id,m_nextId,forward); origin = forward ? edge->nodeB : edge->nodeA;
		}
		Train train = makeScheduledTrain(schedule, std::move(route), now);
		updatePosition(train);
		DBG_LOG(U"[Train] spawn id={} type={} cars={} reverse={} sections={}"_fmt(
			m_nextId, static_cast<int>(train.type), TrainConsist::profile(train.type).cars,
			train.reverseService, train.routeEdges.size()));
		const int trainId = m_nextId;
		addTrain(std::move(train));
		if (m_passengerEventsEnabled) { m_stopEvents << StopEvent{trainId,serviceStop(schedule,0,schedule.reverseNext).stationNodeId,m_trains.back().position}; }
		schedule.lastSpawnAt = now;
		schedule.reverseNext = !schedule.reverseNext;
	}
}

Array<TrainManager::StopEvent> TrainManager::drainStopEvents()
{
	auto result=std::move(m_stopEvents); m_stopEvents.clear(); return result;
}
void TrainManager::setPassengerCount(int trainId,int count)
{
	for(auto& train:m_trains) { if(train.id==trainId) { train.passengerCount=Max(0,count); return; } }
}
