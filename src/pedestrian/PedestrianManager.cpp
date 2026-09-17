#include "PedestrianManager.hpp"
#include "../gen/GenerationSettings.hpp"
#include "../railway/RailTimetable.hpp"

uint64 PedestrianManager::pairKey(int first, int second)
{
	return (static_cast<uint64>(first) << 32) | static_cast<uint32>(second);
}
uint32 PedestrianManager::random()
{
	m_random ^= m_random << 13;
	m_random ^= m_random >> 17;
	m_random ^= m_random << 5;
	return m_random;
}
void PedestrianManager::rebuild(
	const World& world, const RoadNetwork& roads, const BuildingAccessIndex& access, const TrainNetwork& trains)
{
	m_walk.rebuild(world, roads, access, trains);
	m_components.clear();
	m_stationSites.clear();
	m_accessRevision = access.revision();
	refreshServices(trains);
	m_nearStation.assign(m_walk.sites().size(), -1);
	m_nearParking.assign(m_walk.sites().size(), -1);
	Array<size_t> parkingSites;
	for (size_t i = 0; i < m_walk.sites().size(); ++i)
	{
		const auto& site = m_walk.sites()[i];
		m_components[m_walk.nodes()[site.node].component] << i;
		if (site.parking)
		{
			parkingSites << i;
			if (!m_parking.contains(site.key))
			{
				m_parking[site.key].available = GenerationSettings::get().pedestrians_initialParkedCars;
			}
		}
	}
	// 施設への接続は道路・敷地の変更時にだけ調べ、1万人が毎フレーム全施設を走査しない。
	HashTable<Point, Array<size_t>> parkingCells;
	constexpr double kFacilityCell = 300;
	for (const size_t index : parkingSites)
	{
		const Vec3 p = m_walk.sites()[index].entrance;
		parkingCells[{static_cast<int>(p.x / kFacilityCell), static_cast<int>(p.z / kFacilityCell)}] << index;
	}
	for (size_t i = 0; i < m_walk.sites().size(); ++i)
	{
		const auto& site = m_walk.sites()[i];
		const int component = m_walk.nodes()[site.node].component;
		double stationDistance = Square(800.0), parkingDistance = Square(500.0);
		for (size_t station = 0; station < m_walk.stations().size(); ++station)
		{
			const auto& candidate = m_walk.stations()[station];
			const double distance = site.entrance.distanceFromSq(candidate.platform);
			if (m_walk.nodes()[candidate.node].component == component && distance < stationDistance)
			{
				stationDistance = distance;
				m_nearStation[i] = static_cast<int>(station);
			}
		}
		if (m_nearStation[i] >= 0)
		{
			m_stationSites[m_nearStation[i]] << i;
		}
		const Point cell{
			static_cast<int>(site.entrance.x / kFacilityCell), static_cast<int>(site.entrance.z / kFacilityCell)};
		for (int z = -2; z <= 2; ++z)
		{
			for (int x = -2; x <= 2; ++x)
			{
				const auto found = parkingCells.find(cell + Point{x, z});
				if (found == parkingCells.end())
				{
					continue;
				}
				for (const size_t candidate : found->second)
				{
					const auto& parking = m_walk.sites()[candidate];
					const double distance = site.entrance.distanceFromSq(parking.entrance);
					if (m_walk.nodes()[parking.node].component == component && distance < parkingDistance)
					{
						parkingDistance = distance;
						m_nearParking[i] = static_cast<int>(candidate);
					}
				}
			}
		}
	}
	m_stationQueues.clear();
	for (auto& person : m_people)
	{
		if (person.state == PedestrianState::RidingCar || person.state == PedestrianState::RidingTrain)
		{
			continue;
		}
		person.route = {};
		person.step = 0;
		person.distance = 0;
		if (person.state == PedestrianState::Inside)
		{
			const auto home = m_walk.siteIndex(person.origin);
			if (home)
			{
				person.node = m_walk.sites()[*home].node;
				person.position = m_walk.sites()[*home].entrance;
			}
			continue;
		}
		// 駅や駐車場自体が残る編集では、待機列と乗換計画を保つ。
		if (person.state == PedestrianState::WaitingTrain)
		{
			const auto station = m_walk.stationIndex(person.fromStation);
			if (station)
			{
				person.node = m_walk.stations()[*station].node;
				person.position = m_walk.nodes()[person.node].position;
				m_stationQueues[person.fromStation] << person.id;
				continue;
			}
		}
		if (person.state == PedestrianState::WaitingCar)
		{
			const auto parking = m_walk.siteIndex(person.fromParking);
			if (parking)
			{
				person.node = m_walk.sites()[*parking].node;
				person.position = m_walk.nodes()[person.node].position;
				continue;
			}
		}
		const auto node = m_walk.nearestNode(person.position, 1000);
		if (node)
		{
			person.node = *node;
			person.position = m_walk.nodes()[*node].position;
			person.state = PedestrianState::Planning;
		}
		else
		{
			person.state = PedestrianState::Inside;
			person.readyAt = m_now + 2;
		}
	}
	m_dirty = false;
	m_refresh = GenerationSettings::get().pedestrians_refreshSeconds;
}
void PedestrianManager::initialize(const World& world, const RoadNetwork& roads, const BuildingAccessIndex& access,
	const TrainNetwork& trains, int population)
{
	m_people.clear();
	m_parking.clear();
	m_trainPassengers.clear();
	m_stats = {};
	m_now = 0;
	m_planCursor = 0;
	m_random = 42;
	m_targetPopulation = population < 0 ? GenerationSettings::get().pedestrians_targetPopulation : Max(0, population);
	rebuild(world, roads, access, trains);
	populate();
}
void PedestrianManager::populate()
{
	if (m_walk.sites().size() < 2)
	{
		return;
	}
	const int count = m_targetPopulation;
	m_people.reserve(count);
	for (int id = 0; id < count; ++id)
	{
		const auto& home = m_walk.sites()[random() % m_walk.sites().size()];
		Pedestrian person;
		person.id = id;
		person.origin = home.key;
		person.node = home.node;
		person.position = home.entrance;
		person.speed =
			static_cast<float>(GenerationSettings::get().pedestrians_walkSpeed * (.82 + (random() % 360) * .001));
		person.readyAt = m_now + (random() % 600) * .01;
		m_people << std::move(person);
	}
	m_stats.population = count;
}
void PedestrianManager::refreshServices(const TrainNetwork& trains)
{
	m_servedStations.clear();
	for (const auto& schedule : trains.schedules())
	{
		if (!schedule.enabled || !RailTimetable::validate(trains, schedule).isEmpty())
		{
			continue;
		}
		for (const auto& from : schedule.stops)
		{
			for (const auto& to : schedule.stops)
			{
				if (from.stationNodeId != to.stationNodeId)
				{
					m_servedStations.insert(pairKey(from.stationNodeId, to.stationNodeId));
				}
			}
		}
	}
}
void PedestrianManager::chooseTrip(Pedestrian& person)
{
	auto source = m_walk.siteIndex(person.origin);
	if (!source)
	{
		if (m_walk.sites().isEmpty())
		{
			person.readyAt = m_now + 10;
			return;
		}
		source = random() % m_walk.sites().size();
		person.origin = m_walk.sites()[*source].key;
	}
	person.node = m_walk.sites()[*source].node;
	person.position = m_walk.sites()[*source].entrance;
	const int component = m_walk.nodes()[person.node].component;
	const auto& local = m_components.at(component);
	if (local.size() < 2)
	{
		person.readyAt = m_now + 30;
		return;
	}
	PedestrianTripMode mode = PedestrianTripMode::Walk;
	size_t destination = local[random() % local.size()];
	const uint32 preference = random() % 100;
	if (preference < 28 && m_nearStation[*source] >= 0 && m_stationSites.size() > 1)
	{
		for (int attempt = 0; attempt < 12; ++attempt)
		{
			const int station = static_cast<int>(random() % m_walk.stations().size());
			const auto found = m_stationSites.find(station);
			if (station == m_nearStation[*source] || found == m_stationSites.end())
			{
				continue;
			}
			destination = found->second[random() % found->second.size()];
			mode = PedestrianTripMode::Train;
			break;
		}
	}
	else
	{
		// 徒歩は近い目的地を優先。車を使う住民は別の駐車場がある敷地を探す。
		double best = Math::Inf;
		for (int attempt = 0; attempt < 16; ++attempt)
		{
			const size_t candidate = local[random() % local.size()];
			if (candidate == *source)
			{
				continue;
			}
			const double distance = person.position.distanceFromSq(m_walk.sites()[candidate].entrance);
			if (m_carsEnabled && preference < 55 && m_nearParking[*source] >= 0 && m_nearParking[candidate] >= 0 &&
				m_nearParking[*source] != m_nearParking[candidate] && distance > Square(300.0))
			{
				destination = candidate;
				mode = PedestrianTripMode::Car;
				break;
			}
			const double score = Abs(distance - Square(350.0));
			if (score < best)
			{
				best = score;
				destination = candidate;
			}
		}
	}
	if (!beginTrip(person.id, m_walk.sites()[destination].key, mode))
	{
		person.readyAt = m_now + 3;
	}
}
void PedestrianManager::update(double dt, Vec3 camera, const World& world, const RoadNetwork& roads,
	const SimGraph& graph, VehicleManager& vehicles, TrainNetwork& network, TrainManager& trains)
{
	if (dt <= 0)
	{
		return;
	}
	Stopwatch timer{StartImmediately::Yes};
	m_now += dt;
	m_refresh -= dt;
	m_stepDt = dt;
	m_stats.planned = m_stats.nearUpdates = m_stats.farUpdates = m_stats.expansions = 0;
	if (m_dirty || (m_refresh <= 0 && vehicles.buildingAccess().revision() != m_accessRevision))
	{
		rebuild(world, roads, vehicles.buildingAccess(), network);
	}
	else if (m_refresh <= 0)
	{
		refreshServices(network);
		m_refresh = GenerationSettings::get().pedestrians_refreshSeconds;
	}
	if (m_people.isEmpty() && m_targetPopulation > 0 && m_walk.sites().size() >= 2)
	{
		populate();
	}
	transfers(vehicles, graph, roads, trains);
	m_busyCrossings.clear();
	m_busyJunctions.clear();
	for (const auto& vehicle : vehicles.vehicles())
	{
		if (vehicle.location == VehicleLocation::OnConnection)
		{
			m_busyJunctions.insert(vehicle.connectionNodeId);
			continue;
		}
		const auto* edge = roads.getEdge(vehicle.currentEdge);
		if (!edge || vehicle.speed < .3f)
		{
			continue;
		}
		const float clearance = 18 + vehicle.speed * 2;
		if (vehicle.arcPos < edge->cutoffA + clearance)
		{
			m_busyCrossings.insert(pairKey(edge->nodeA, edge->id));
		}
		if (edge->length - vehicle.arcPos < edge->cutoffB + clearance)
		{
			m_busyCrossings.insert(pairKey(edge->nodeB, edge->id));
		}
	}
	const double nearSq = Square(GenerationSettings::get().pedestrians_nearDistance);
	HashSet<int> crossingPedestrians;
	for (auto& person : m_people)
	{
		if (person.state == PedestrianState::Inside && person.readyAt <= m_now)
		{
			chooseTrip(person);
		}
		if (person.state == PedestrianState::WaitingTrain &&
			m_now - person.waitingSince > GenerationSettings::get().pedestrians_maximumTrainWait)
		{
			startFinalWalk(person, person.node);
		}
		if (person.state != PedestrianState::Walking)
		{
			continue;
		}
		const bool near = person.position.distanceFromSq(camera) < nearSq;
		const bool crossing = person.step < person.route.steps.size() &&
							  m_walk.links()[Abs(person.route.steps[person.step]) - 1].crossingNode >= 0;
		const double interval = (near || crossing) ? 0 : GenerationSettings::get().pedestrians_farUpdateSeconds;
		if (m_now - person.sampledAt >= interval)
		{
			advance(person, m_now - person.sampledAt, roads, vehicles);
			person.sampledAt = m_now;
			if (near)
			{
				++m_stats.nearUpdates;
			}
			else
			{
				++m_stats.farUpdates;
			}
		}
		if (person.state == PedestrianState::Walking && person.step < person.route.steps.size())
		{
			const auto& link = m_walk.links()[Abs(person.route.steps[person.step]) - 1];
			if (link.crossingNode >= 0 && person.distance > .01f)
			{
				crossingPedestrians.insert(link.crossingNode);
			}
		}
	}
	for (auto& [station, queue] : m_stationQueues)
	{
		queue.remove_if([&](int id)
		{ return m_people[id].state != PedestrianState::WaitingTrain || m_people[id].fromStation != station; });
	}
	vehicles.setPedestrianCrossings(std::move(crossingPedestrians));
	const int budget = GenerationSettings::get().pedestrians_routesPerUpdate;
	for (size_t scanned = 0; scanned < m_people.size() && m_stats.planned < budget; ++scanned)
	{
		m_planCursor = (m_planCursor + 1) % m_people.size();
		auto& person = m_people[m_planCursor];
		if (person.state == PedestrianState::Planning && person.readyAt <= m_now)
		{
			plan(person);
		}
	}
	m_stats.population = static_cast<int>(m_people.size());
	m_stats.walking = m_stats.waitingTrain = m_stats.ridingTrain = m_stats.waitingCar = m_stats.ridingCar = 0;
	for (const auto& person : m_people)
	{
		m_stats.walking += person.state == PedestrianState::Walking;
		m_stats.waitingTrain += person.state == PedestrianState::WaitingTrain;
		m_stats.ridingTrain += person.state == PedestrianState::RidingTrain;
		m_stats.waitingCar += person.state == PedestrianState::WaitingCar;
		m_stats.ridingCar += person.state == PedestrianState::RidingCar;
	}
	// 自動補充が空きをすべて埋める前に、駐車場で待つ住民の分を確保する。
	vehicles.setPassengerDemand(m_stats.waitingCar);
	m_stats.updateMs = timer.msF();
}
