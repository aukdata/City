#pragma once
#include "PedestrianNetwork.hpp"
#include "../traffic/VehicleManager.hpp"
#include "../railway/TrainManager.hpp"

/// @brief 住民の移動手段。見た目のLODを切り替えても乗換の状態は共通。
enum class PedestrianTripMode : uint8
{
	Walk,
	Train,
	Car
};
enum class PedestrianState : uint8
{
	Inside,
	Planning,
	Walking,
	WaitingTrain,
	RidingTrain,
	WaitingCar,
	RidingCar
};
struct Pedestrian
{
	int id = -1, node = -1, carrier = -1;
	int64 origin = -1, destination = -1, fromParking = -1, toParking = -1;
	int fromStation = -1, toStation = -1;
	PedestrianState state = PedestrianState::Inside;
	PedestrianTripMode mode = PedestrianTripMode::Walk;
	PedestrianNetwork::Route route;
	size_t step = 0;
	float distance = 0, speed = 1.35f, heading = 0;
	Vec3 position;
	double sampledAt = 0, readyAt = 0, waitingSince = 0;
	bool finalWalk = false;
};
/// @brief 敷地間の移動、駅・駐車場の乗換、距離別の更新予算を管理する。
class PedestrianManager
{
public:
	void setCarsEnabled(bool enabled) { m_carsEnabled = enabled; }
	struct Stats
	{
		int population = 0, walking = 0, waitingTrain = 0, ridingTrain = 0, waitingCar = 0, ridingCar = 0;
		int planned = 0, nearUpdates = 0, farUpdates = 0, expansions = 0;
		int completed = 0, boardedTrains = 0, boardedCars = 0, failedRoutes = 0;
		double updateMs = 0;
	};
	void initialize(const World& world, const RoadNetwork& roads, const BuildingAccessIndex& access,
		const TrainNetwork& trains, int population = -1);
	void invalidate() { m_dirty = true; }
	void update(double dt, Vec3 camera, const World& world, const RoadNetwork& roads, const SimGraph& graph,
		VehicleManager& vehicles, TrainNetwork& network, TrainManager& trains);
	/// @brief 明示的な目的地変更。通常生成とテストは同じ乗換計画を使う。
	bool beginTrip(int personId, int64 destination, PedestrianTripMode mode);
	[[nodiscard]] const Array<Pedestrian>& people() const { return m_people; }
	[[nodiscard]] const PedestrianNetwork& network() const { return m_walk; }
	[[nodiscard]] const Stats& stats() const { return m_stats; }
	[[nodiscard]] double now() const { return m_now; }

private:
	bool m_carsEnabled = true;
	struct Parking
	{
		int available = 0, reserved = 0;
	};
	PedestrianNetwork m_walk;
	Array<Pedestrian> m_people;
	HashTable<int64, Parking> m_parking;
	Array<int> m_nearStation, m_nearParking;
	HashTable<int, Array<int>> m_stationQueues, m_trainPassengers;
	HashSet<uint64> m_busyCrossings, m_servedStations;
	uint64 m_accessRevision = 0;
	HashSet<int> m_busyJunctions;
	HashTable<int, Array<size_t>> m_components, m_stationSites;
	Stats m_stats;
	double m_now = 0, m_refresh = 0, m_stepDt = 0;
	bool m_dirty = false;
	size_t m_planCursor = 0;
	uint32 m_random = 42;
	int m_targetPopulation = 0;
	void populate();
	uint32 random();
	static uint64 pairKey(int first, int second);
	void rebuild(
		const World& world, const RoadNetwork& roads, const BuildingAccessIndex& access, const TrainNetwork& trains);
	void refreshServices(const TrainNetwork& trains);
	void chooseTrip(Pedestrian& person);
	bool plan(Pedestrian& person);
	void arriveOnFoot(Pedestrian& person);
	void startFinalWalk(Pedestrian& person, int node);
	void advance(Pedestrian& person, double elapsed, const RoadNetwork& roads, const VehicleManager& vehicles);
	void transfers(VehicleManager& vehicles, const SimGraph& graph, const RoadNetwork& roads, TrainManager& trains);
	void boardTrain(const Train& train, int station);
	void finish(Pedestrian& person);
};
