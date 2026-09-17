#include "PedestrianManager.hpp"
#include "../gen/GenerationSettings.hpp"
#include "../railway/TrainConsist.hpp"

void PedestrianManager::boardTrain(const Train& train, int station)
{
	const auto found = m_stationQueues.find(station);
	if (found == m_stationQueues.end())
	{
		return;
	}
	const auto origin = std::find_if(train.serviceStops.begin(), train.serviceStops.end(),
		[&](const StopEntry& stop) { return stop.stationNodeId == station; });
	if (origin == train.serviceStops.end())
	{
		return;
	}
	auto& passengers = m_trainPassengers[train.id];
	const int capacity =
		TrainConsist::profile(train.type).cars * GenerationSettings::get().pedestrians_passengersPerCarriage;
	for (const int id : found->second)
	{
		auto& person = m_people[id];
		if (person.state != PedestrianState::WaitingTrain || person.fromStation != station)
		{
			continue;
		}
		if (static_cast<int>(passengers.size()) >= capacity)
		{
			break;
		}
		if (std::none_of(origin + 1, train.serviceStops.end(),
				[&](const StopEntry& stop) { return stop.stationNodeId == person.toStation; }))
		{
			continue;
		}
		person.state = PedestrianState::RidingTrain;
		person.carrier = train.id;
		passengers << id;
		++m_stats.boardedTrains;
	}
	found->second.remove_if([&](int id) { return m_people[id].state != PedestrianState::WaitingTrain; });
}
void PedestrianManager::transfers(
	VehicleManager& vehicles, const SimGraph& graph, const RoadNetwork& roads, TrainManager& trains)
{
	for (const auto& arrival : vehicles.drainPassengerArrivals())
	{
		if (!InRange(arrival.passengerId, 0, static_cast<int>(m_people.size()) - 1))
		{
			continue;
		}
		auto& person = m_people[arrival.passengerId];
		if (person.state != PedestrianState::RidingCar)
		{
			continue;
		}
		auto& parking = m_parking[person.toParking];
		parking.reserved = Max(0, parking.reserved - 1);
		if (arrival.arrived)
		{
			++parking.available;
			const auto site = m_walk.siteIndex(person.toParking);
			if (site)
			{
				startFinalWalk(person, m_walk.sites()[*site].node);
				continue;
			}
		}
		else
		{
			++m_parking[person.fromParking].available;
		}
		const auto node = m_walk.nearestNode(arrival.position, 2000);
		if (node)
		{
			startFinalWalk(person, *node);
		}
		else
		{
			person.state = PedestrianState::Inside;
			person.readyAt = m_now + 10;
		}
	}
	for (const auto& event : trains.drainStopEvents())
	{
		const auto found = m_trainPassengers.find(event.trainId);
		if (found != m_trainPassengers.end())
		{
			for (const int id : found->second)
			{
				auto& person = m_people[id];
				if (!event.ended && person.toStation != event.stationId)
				{
					continue;
				}
				const auto station = m_walk.stationIndex(event.stationId);
				const auto node = station ? Optional<int>{m_walk.stations()[*station].node}
										  : m_walk.nearestNode(event.position, 2000);
				if (node)
				{
					startFinalWalk(person, *node);
				}
				else
				{
					person.state = PedestrianState::Inside;
					person.readyAt = m_now + 10;
				}
			}
			found->second.remove_if([&](int id) { return m_people[id].state != PedestrianState::RidingTrain; });
			if (event.ended)
			{
				m_trainPassengers.erase(found);
			}
		}
		// 始発は生成イベント、途中駅は停車中の便だけを乗車対象にする。
		if (!event.ended)
		{
			for (const auto& train : trains.trains())
			{
				if (train.id == event.trainId)
				{
					if ((train.state == TrainState::WaitingStation &&
							train.serviceStops[train.nextStopIdx].stationNodeId == event.stationId) ||
						(train.nextStopIdx == 1 && train.serviceStops.front().stationNodeId == event.stationId))
					{
						boardTrain(train, event.stationId);
					}
					break;
				}
			}
		}
	}
	for (const auto& train : trains.trains())
	{
		if (train.state == TrainState::WaitingStation &&
			InRange(train.nextStopIdx, 0, static_cast<int>(train.serviceStops.size()) - 1))
		{
			boardTrain(train, train.serviceStops[train.nextStopIdx].stationNodeId);
		}
		const auto found = m_trainPassengers.find(train.id);
		trains.setPassengerCount(
			train.id, found == m_trainPassengers.end() ? 0 : static_cast<int>(found->second.size()));
	}
	for (auto& person : m_people)
	{
		if (person.state != PedestrianState::WaitingCar || person.readyAt > m_now)
		{
			continue;
		}
		person.readyAt = m_now + 1 + (person.id % 7) * .1;
		const auto from = m_walk.siteIndex(person.fromParking), to = m_walk.siteIndex(person.toParking);
		if (!from || !to || m_now - person.waitingSince > GenerationSettings::get().pedestrians_maximumParkingWait)
		{
			startFinalWalk(person, person.node);
			continue;
		}
		auto& source = m_parking[person.fromParking];
		auto& target = m_parking[person.toParking];
		if (source.available <= 0 ||
			target.available + target.reserved >= GenerationSettings::get().pedestrians_parkingSpaces)
		{
			continue;
		}
		const auto vehicle = vehicles.boardPassenger(
			person.id, m_walk.sites()[*from].vehicle, m_walk.sites()[*to].vehicle, graph, roads);
		if (vehicle)
		{
			--source.available;
			++target.reserved;
			person.carrier = *vehicle;
			person.state = PedestrianState::RidingCar;
			++m_stats.boardedCars;
		}
	}
}
