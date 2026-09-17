#include "VehicleManager.hpp"
#include "VehiclePose.hpp"
#include "TrafficSpawn.hpp"
#include "../gen/GenerationSettings.hpp"

Optional<int> VehicleManager::boardPassenger(int passengerId, const BuildingAccessPoint& from,
	const BuildingAccessPoint& to, const SimGraph& graph, const RoadNetwork& roads)
{
	if (vehicleCount() >= GenerationSettings::get().traffic_maximumVehicles)
	{
		return none;
	}
	const auto* origin = graph.getEdge(from.edgeId);
	const auto* destination = graph.getEdge(to.edgeId);
	if (!origin || !destination || !TrafficSpawn::laneOpen(*origin, from.lane) ||
		!TrafficSpawn::laneOpen(*destination, to.lane))
	{
		return none;
	}
	const bool forward = TrafficCommon::isForwardLane(*origin, from.lane);
	// 同じ車線の後方はそのまま到着扱いにしない。別駐車場候補を選び直す。
	if (from.edgeId == to.edgeId && (from.lane != to.lane || (to.arc - from.arc) * (forward ? 1 : -1) < 4))
	{
		return none;
	}
	if (!m_laneTraffic.hasSpace(from.edgeId, from.lane, from.arc, VehicleType::PassengerCar))
	{
		return none;
	}
	Vehicle car;
	car.id = m_nextId++;
	car.passengerId = passengerId;
	car.currentEdge = from.edgeId;
	car.currentLane = from.lane;
	car.arcPos = from.arc;
	car.originBuilding = from.buildingKey;
	car.destinationBuilding = to.buildingKey;
	car.goalEdgeId = to.edgeId;
	car.goalLane = to.lane;
	car.goalArc = to.arc;
	car.departedAt = m_lastGameNow;
	car.position = {from.position.x, 0, from.position.y};
	if (const auto curve = roads.getBezier(from.edgeId))
	{
		car.position = curve->positionAt(from.arc) + tangentToRight(curve->tangentAt(from.arc)) *
														 origin->lanes[from.lane].centerAt(from.arc / origin->length);
	}
	requestRoute(car, graph);
	m_laneTraffic.reserve(car, car.currentLane);
	m_vehicleIndices[car.id] = m_vehicles.size();
	const int id = car.id;
	m_vehicles << std::move(car);
	return id;
}
Array<VehicleManager::PassengerArrival> VehicleManager::drainPassengerArrivals()
{
	auto result = std::move(m_passengerArrivals);
	m_passengerArrivals.clear();
	return result;
}
