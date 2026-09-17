#pragma once
#include "TrafficCommon.hpp"

/// @brief 自動発生と再出発で共有する、車線・接続・車間の条件。
namespace TrafficSpawn
{
	inline double vehicleLength(VehicleType type)
	{
		switch (type)
		{
		case VehicleType::KeiCar: return 3.4;
		case VehicleType::SmallTruck: return 6;
		case VehicleType::LargeTruck: return 12;
		case VehicleType::Bus: return 11;
		case VehicleType::Moped: case VehicleType::LightVehicle: return 1.9;
		default: return 4.5;
		}
	}

	inline bool laneOpen(const SimGraph::Edge& edge, int lane)
	{
		if (!edge.isRoadbedBuilt() || lane < 0 || lane >= static_cast<int>(edge.lanes.size())) { return false; }
		const auto& value = edge.lanes[lane];
		return (value.op == OpState::Open || value.op == OpState::Provisional)
			&& value.type != LaneType::KeepOut && value.type != LaneType::TrafficIsland
			&& value.type != LaneType::ParkingBay && value.type != LaneType::EmergencyStop;
	}

	/// @brief 有向の車線接続をたどり、孤立道路・逆走・到達不能なゴールを除外する。
	inline int reachableGoal(const SimGraph& graph, int origin, int lane)
	{
		int current = origin, goal = -1;
		HashSet<int> visited{origin};
		const int steps = Random(6, 20);
		for (int step = 0; step < steps; ++step)
		{
			const auto* edge = graph.getEdge(current);
			if (!edge || !laneOpen(*edge, lane)) { break; }
			const int exit = TrafficCommon::isForwardLane(*edge, lane) ? edge->nodeB : edge->nodeA;
			const auto* node = graph.getNode(exit);
			if (!node) { break; }
			Array<std::pair<int, int>> choices;
			for (const auto& connection : node->laneConnections)
			{
				if (connection.fromEdgeId != current || connection.fromLaneIndex != lane
					|| visited.contains(connection.toEdgeId)) { continue; }
				const auto* next = graph.getEdge(connection.toEdgeId);
				if (next && laneOpen(*next, connection.toLaneIndex))
				{
					choices << std::pair<int, int>{next->id, connection.toLaneIndex};
				}
			}
			if (choices.isEmpty()) { break; }
			const auto selected = choices[Random(0, static_cast<int>(choices.size())-1)];
			current = selected.first; lane = selected.second; goal = current;
			visited.insert(current);
		}
		return goal;
	}

	inline bool hasSpace(const Array<Vehicle>& vehicles, int edge, int lane, float arc, VehicleType type)
	{
		for (const auto& other : vehicles)
		{
			if (other.currentEdge != edge || other.location == VehicleLocation::OnConnection) { continue; }
			if (other.currentLane != lane && !(other.location == VehicleLocation::ChangingLane
				&& (other.laneFrom == lane || other.laneTo == lane))) { continue; }
			const double gap = (vehicleLength(type)+vehicleLength(other.type))*.5 + 4 + Max(5.0f, other.speed)*1.5;
			if (Abs(arc-other.arcPos) < gap) { return false; }
		}
		return true;
	}
}
