#include "PedestrianManager.hpp"
#include "../gen/GenerationSettings.hpp"

bool PedestrianManager::beginTrip(int id, int64 destination, PedestrianTripMode mode)
{
	if (!InRange(id, 0, static_cast<int>(m_people.size()) - 1))
	{
		return false;
	}
	auto& person = m_people[id];
	const auto from = m_walk.siteIndex(person.origin), to = m_walk.siteIndex(destination);
	if (!from || !to || person.origin == destination || person.state == PedestrianState::RidingCar ||
		person.state == PedestrianState::RidingTrain)
	{
		return false;
	}
	person.destination = destination;
	person.mode = mode;
	person.finalWalk = false;
	person.fromStation = person.toStation = -1;
	person.fromParking = person.toParking = -1;
	if (mode == PedestrianTripMode::Train)
	{
		const int a = m_nearStation[*from], b = m_nearStation[*to];
		if (a < 0 || b < 0 || a == b ||
			!m_servedStations.contains(pairKey(m_walk.stations()[a].stationId, m_walk.stations()[b].stationId)))
		{
			person.mode = PedestrianTripMode::Walk;
		}
		else
		{
			person.fromStation = m_walk.stations()[a].stationId;
			person.toStation = m_walk.stations()[b].stationId;
		}
	}
	if (mode == PedestrianTripMode::Car)
	{
		const int a = m_nearParking[*from], b = m_nearParking[*to];
		if (a < 0 || b < 0 || a == b)
		{
			person.mode = PedestrianTripMode::Walk;
		}
		else
		{
			person.fromParking = m_walk.sites()[a].key;
			person.toParking = m_walk.sites()[b].key;
		}
	}
	person.state = PedestrianState::Planning;
	person.readyAt = m_now;
	person.route = {};
	person.step = 0;
	person.distance = 0;
	return true;
}
bool PedestrianManager::plan(Pedestrian& person)
{
	const auto destination = m_walk.siteIndex(person.destination);
	if (!destination)
	{
		finish(person);
		return false;
	}
	int target = m_walk.sites()[*destination].node;
	if (!person.finalWalk && person.mode == PedestrianTripMode::Train)
	{
		const auto station = m_walk.stationIndex(person.fromStation);
		if (station)
		{
			target = m_walk.stations()[*station].node;
		}
		else
		{
			person.finalWalk = true;
		}
	}
	if (!person.finalWalk && person.mode == PedestrianTripMode::Car)
	{
		const auto parking = m_walk.siteIndex(person.fromParking);
		if (parking)
		{
			target = m_walk.sites()[*parking].node;
		}
		else
		{
			person.finalWalk = true;
		}
	}
	const auto route = m_walk.route(person.node, target, GenerationSettings::get().pedestrians_routeExpansionLimit);
	m_stats.expansions += m_walk.lastExpansions();
	++m_stats.planned;
	if (!route)
	{
		++m_stats.failedRoutes;
		person.state = PedestrianState::Inside;
		person.readyAt = m_now + 15 + (random() % 20);
		return false;
	}
	person.route = *route;
	person.step = 0;
	person.distance = 0;
	person.sampledAt = m_now;
	person.state = PedestrianState::Walking;
	if (person.route.steps.isEmpty())
	{
		arriveOnFoot(person);
	}
	return true;
}
void PedestrianManager::finish(Pedestrian& person)
{
	const auto destination = m_walk.siteIndex(person.destination);
	if (destination)
	{
		person.origin = person.destination;
		person.node = m_walk.sites()[*destination].node;
		person.position = m_walk.sites()[*destination].entrance;
		++m_stats.completed;
	}
	person.destination = -1;
	person.carrier = -1;
	person.route = {};
	person.state = PedestrianState::Inside;
	person.readyAt =
		m_now + GenerationSettings::get().pedestrians_destinationRestSeconds * (.5 + (random() % 100) * .01);
}
void PedestrianManager::arriveOnFoot(Pedestrian& person)
{
	person.route = {};
	person.step = 0;
	person.distance = 0;
	person.waitingSince = m_now;
	person.sampledAt = m_now;
	if (person.finalWalk || person.mode == PedestrianTripMode::Walk)
	{
		finish(person);
	}
	else if (person.mode == PedestrianTripMode::Train)
	{
		person.state = PedestrianState::WaitingTrain;
		m_stationQueues[person.fromStation] << person.id;
	}
	else
	{
		person.state = PedestrianState::WaitingCar;
		person.readyAt = m_now;
	}
}
void PedestrianManager::startFinalWalk(Pedestrian& person, int node)
{
	person.finalWalk = true;
	person.node = node;
	person.position = m_walk.nodes()[node].position;
	person.carrier = -1;
	person.state = PedestrianState::Planning;
	person.readyAt = m_now;
	person.route = {};
}
void PedestrianManager::advance(
	Pedestrian& person, double elapsed, const RoadNetwork& roads, const VehicleManager& vehicles)
{
	float travel = person.speed * static_cast<float>(elapsed);
	while (person.step < person.route.steps.size())
	{
		const int step = person.route.steps[person.step];
		const auto& link = m_walk.links()[Abs(step) - 1];
		if (link.crossingNode >= 0 && person.distance < .01f)
		{
			bool blocked = m_busyCrossings.contains(pairKey(link.crossingNode, link.crossingEdge)) ||
						   m_busyJunctions.contains(link.crossingNode);
			const auto* light = vehicles.getTrafficLight(link.crossingNode);
			const auto* node = roads.getNode(link.crossingNode);
			if (light && node)
			{
				for (const auto& connection : node->laneConnections)
				{
					blocked |= connection.fromEdgeId == link.crossingEdge && light->isGreen(connection.id);
				}
			}
			if (blocked)
			{
				break;
			}
		}
		if (link.crossingNode >= 0)
		{
			travel = Min(travel, person.speed * static_cast<float>(m_stepDt));
		}
		const float amount = Min(travel, link.length - person.distance);
		person.distance += amount;
		travel -= amount;
		const Vec3 before = person.position;
		person.position = m_walk.position(step, person.distance);
		const Vec3 direction = person.position - before;
		if (direction.lengthSq() > .00001)
		{
			person.heading = static_cast<float>(Math::Atan2(direction.x, direction.z));
		}
		if (person.distance + .001f < link.length)
		{
			break;
		}
		person.node = step > 0 ? link.b : link.a;
		++person.step;
		person.distance = 0;
		if (person.step == person.route.steps.size())
		{
			arriveOnFoot(person);
			break;
		}
		if (travel <= 0)
		{
			break;
		}
	}
}
