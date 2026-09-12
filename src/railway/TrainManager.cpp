#include "TrainManager.hpp"

void TrainManager::init(TrainNetwork* network) { m_network=network; }
void TrainManager::addTrain(Train train) { train.id=m_nextId++; m_trains << std::move(train); }

void TrainManager::update(double dt,GameTime now)
{
	if (!m_network) { return; }
	spawnScheduledTrains(now);
	for (auto& train : m_trains) { updateTrain(train,dt,now); }
	m_trains.remove_if([](const Train& train) { return train.currentEdge<0; });
}

void TrainManager::updateTrain(Train& train,double dt,GameTime now)
{
	if (train.state==TrainState::WaitingStation)
	{
		train.waitRemaining-=static_cast<float>(dt);
		if (train.waitRemaining>0) { return; }
		train.waitRemaining=0; ++train.nextStopIdx; train.state=TrainState::Running;
	}
	if (train.state==TrainState::WaitingSignal)
	{
		if (train.routeProgress+1>=static_cast<int>(train.routeEdges.size())) { return; }
		const auto* next=m_network->getEdge(train.routeEdges[train.routeProgress+1]);
		if (next && next->occupiedBy>=0 && next->occupiedBy!=train.id) { return; }
		train.state=TrainState::Running;
	}
	advanceTrain(train,dt,now);
}

void TrainManager::advanceTrain(Train& train,double dt,[[maybe_unused]] GameTime now)
{
	const auto* edge=m_network->getEdge(train.currentEdge);
	if (!edge) { train.currentEdge=-1; return; }
	const float target=targetSpeed(train),step=static_cast<float>(dt);
	train.speed=target>train.speed ? Min(target,train.speed+kMaxAccel*step) : Max(target,train.speed-kMaxDecel*step);
	float travel=Max(0.0f,train.speed*step);
	for (;;)
	{
		edge=m_network->getEdge(train.currentEdge);
		const int end=train.forward ? edge->nodeB : edge->nodeA;
		const float remaining=Max(0.0f,edge->length-train.arcPos);
		const float move=Min(remaining,travel); train.arcPos+=move; travel-=move;
		if (train.arcPos<edge->length-.05f) { break; }
		train.arcPos=edge->length;
		bool stopping=false;
		for (const auto& schedule : m_network->schedules())
		{
			if (schedule.id==train.scheduleId && train.nextStopIdx<static_cast<int>(schedule.stops.size()) && schedule.stops[train.nextStopIdx].stationNodeId==end)
			{
				train.state=TrainState::WaitingStation; train.speed=0;
				train.waitRemaining=schedule.stops[train.nextStopIdx].dwellSec; stopping=true; break;
			}
		}
		if (stopping) { break; }
		if (train.routeProgress+1>=static_cast<int>(train.routeEdges.size()))
		{
			m_network->releaseOccupy(train.currentEdge,train.id); train.currentEdge=-1; return;
		}
		const int nextId=train.routeEdges[train.routeProgress+1];
		const auto* next=m_network->getEdge(nextId);
		if (!next || (next->nodeA!=end && next->nodeB!=end))
		{
			m_network->releaseOccupy(train.currentEdge,train.id); train.currentEdge=-1; return;
		}
		if (!m_network->tryOccupy(nextId,train.id)) { train.state=TrainState::WaitingSignal; train.speed=0; break; }
		m_network->releaseOccupy(train.currentEdge,train.id);
		train.currentEdge=nextId; ++train.routeProgress; train.forward=next->nodeA==end; train.arcPos=0;
		if (travel<=0) { break; }
	}
	if (const auto curve=m_network->getBezier(train.currentEdge))
	{
		const float arc=train.forward ? train.arcPos : curve->totalLength-train.arcPos;
		train.position=curve->positionAt(arc);
		const Vec3 tangent=curve->tangentAt(arc)*(train.forward ? 1 : -1);
		train.heading=static_cast<float>(Math::Atan2(tangent.x,tangent.z));
	}
}

float TrainManager::targetSpeed(const Train& train) const
{
	const auto* current=m_network->getEdge(train.currentEdge);
	if (!current) { return 0; }
	float speed=current->speedLimit/3.6f;
	int targetStation=-1;
	for (const auto& schedule : m_network->schedules())
	{
		if (schedule.id==train.scheduleId && train.nextStopIdx<static_cast<int>(schedule.stops.size())) { targetStation=schedule.stops[train.nextStopIdx].stationNodeId; break; }
	}
	float distance=-train.arcPos;
	int start=train.forward ? current->nodeA : current->nodeB;
	for (int index=train.routeProgress;index<static_cast<int>(train.routeEdges.size());++index)
	{
		const auto* edge=m_network->getEdge(train.routeEdges[index]);
		if (!edge) { break; }
		if (index>train.routeProgress && edge->occupiedBy>=0 && edge->occupiedBy!=train.id)
		{
			speed=Min(speed,Math::Sqrt(Max(.01f,2*kMaxDecel*distance))); break;
		}
		distance+=edge->length; start=edge->nodeA==start ? edge->nodeB : edge->nodeA;
		if (start==targetStation)
		{
			speed=Min(speed,Math::Sqrt(Max(.01f,2*kMaxDecel*distance))); break;
		}
		if (distance>1000) { break; }
	}
	return speed;
}

void TrainManager::spawnScheduledTrains(GameTime now)
{
	for (auto& schedule : m_network->schedules())
	{
		if (schedule.stops.size()<2 || now-schedule.lastSpawnAt<schedule.headwaySec) { continue; }
		Array<int> route; bool valid=true;
		for (size_t i=1;i<schedule.stops.size();++i)
		{
			const auto leg=m_network->findRoute(schedule.stops[i-1].stationNodeId,schedule.stops[i].stationNodeId);
			if (leg.isEmpty()) { valid=false; break; }
			route.append(leg);
		}
		if (!valid || route.isEmpty() || !m_network->tryOccupy(route.front(),m_nextId)) { continue; }
		Train train; train.currentEdge=route.front(); train.routeEdges=std::move(route);
		train.forward=m_network->getEdge(train.currentEdge)->nodeA==schedule.stops.front().stationNodeId;
		train.position=m_network->getNode(schedule.stops.front().stationNodeId)->position;
		train.nextStopIdx=1; train.scheduleId=schedule.id; train.departedAt=now;
		addTrain(std::move(train)); schedule.lastSpawnAt=now;
	}
}
