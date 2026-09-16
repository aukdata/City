#pragma once
#include "TrafficSpawn.hpp"

/// @brief 車線別に整列した交通のスナップショット。描画状態にかかわらず車間を保つ。
class LaneVehicleIndex
{
public:
	struct Occupant
	{
		int id;
		float arc, speed, length;
	};
	struct Neighbors
	{
		Optional<Occupant> front, rear;
	};
	static uint64 key(int edge, int lane)
	{
		return (static_cast<uint64>(static_cast<uint32>(edge)) << 32) | static_cast<uint32>(lane);
	}
	/// @brief フレーム中は位置を固定し、更新順序による追従の違いを防ぐ。
	void rebuild(const Array<Vehicle>& vehicles)
	{
		for (auto& [id, lane] : m_lanes) { lane.clear(); }
		for (auto& [id, lane] : m_connections) { lane.clear(); }
		for (auto& [id, indices] : m_edges) { indices.clear(); }
		for (auto& [id, indices] : m_nodes) { indices.clear(); }
		if (++m_frame % 600 == 0)
		{
			m_lanes.clear(); m_connections.clear(); m_edges.clear(); m_nodes.clear();
		}
		for (size_t i = 0; i < vehicles.size(); ++i)
		{
			const auto& vehicle = vehicles[i];
			if (vehicle.currentEdge < 0 || vehicle.tripCompleted) { continue; }
			append(vehicle);
			m_edges[vehicle.currentEdge] << i;
			if (vehicle.location == VehicleLocation::OnConnection) { m_nodes[vehicle.connectionNodeId] << i; }
		}
		for (auto& [id, lane] : m_lanes) { sort(lane); }
		for (auto& [id, lane] : m_connections) { sort(lane); }
	}
	/// @brief 発生・車線変更を同じフレームの後続判定にも予約する。
	void reserve(const Vehicle& vehicle, int lane)
	{
		auto& occupants = m_lanes[key(vehicle.currentEdge, lane)];
		const auto item = occupant(vehicle);
		occupants.insert(std::lower_bound(occupants.begin(), occupants.end(), item.arc,
			[](const Occupant& value, float arc) { return value.arc < arc; }), item);
	}
	/// @brief 交差点に入った個体を後続車から即座に見えるようにする。
	void reserveConnection(const Vehicle& vehicle)
	{
		auto& occupants = m_connections[key(vehicle.connectionNodeId, vehicle.connectionId)];
		const auto item = occupant(vehicle);
		occupants.insert(std::lower_bound(occupants.begin(), occupants.end(), item.arc,
			[](const Occupant& value, float arc) { return value.arc < arc; }), item);
	}
	[[nodiscard]] bool connectionEntryClear(int node, int connection, VehicleType type) const
	{
		const auto pair = neighbors(node, connection, 0, true, -1, true);
		return !pair.front || pair.front->arc > (pair.front->length + TrafficSpawn::vehicleLength(type)) * .5 + .5;
	}
	[[nodiscard]] Neighbors neighbors(int edge, int lane, float arc, bool forward, int selfId,
		bool connection = false) const
	{
		const auto& table = connection ? m_connections : m_lanes;
		const auto found = table.find(key(edge, lane));
		if (found == table.end()) { return {}; }
		const auto& values = found->second;
		auto next = std::lower_bound(values.begin(), values.end(), arc,
			[](const Occupant& value, float position) { return value.arc < position; });
		auto previous = next;
		Neighbors result;
		while (next != values.end() && next->id == selfId) { ++next; }
		if (next != values.end()) { (forward ? result.front : result.rear) = *next; }
		while (previous != values.begin())
		{
			--previous;
			if (previous->id != selfId) { (forward ? result.rear : result.front) = *previous; break; }
		}
		return result;
	}
	void measureGaps(int selfId, int edge, int lane, float arc, bool forward,
		float& front, float& rear) const
	{
		const auto pair = neighbors(edge, lane, arc, forward, selfId);
		front = pair.front ? Abs(pair.front->arc - arc) : 1e9f;
		rear = pair.rear ? Abs(pair.rear->arc - arc) : 1e9f;
	}
	[[nodiscard]] bool hasSpace(int edge, int lane, float arc, VehicleType type) const
	{
		const auto pair = neighbors(edge, lane, arc, true, -1);
		for (const auto& other : {pair.front, pair.rear})
		{
			if (other && Abs(other->arc - arc) < (TrafficSpawn::vehicleLength(type) + other->length) * .5 + 4 + Max(5.0f, other->speed) * 1.5) { return false; }
		}
		return true;
	}
	[[nodiscard]] const Array<size_t>& edgeVehicles(int edge) const { return indices(m_edges, edge); }
	[[nodiscard]] const Array<size_t>& nodeVehicles(int node) const { return indices(m_nodes, node); }

private:
	int m_frame = 0;
	HashTable<uint64, Array<Occupant>> m_lanes, m_connections;
	HashTable<int, Array<size_t>> m_edges, m_nodes;
	static const Array<size_t>& indices(const HashTable<int, Array<size_t>>& table, int id)
	{
		static const Array<size_t> empty;
		const auto found = table.find(id);
		return found == table.end() ? empty : found->second;
	}
	static Occupant occupant(const Vehicle& vehicle)
	{
		return {vehicle.id, vehicle.arcPos, vehicle.speed, static_cast<float>(TrafficSpawn::vehicleLength(vehicle.type))};
	}
	static void sort(Array<Occupant>& lane)
	{
		lane.sort_by([](const Occupant& a, const Occupant& b) { return a.arc < b.arc || (a.arc == b.arc && a.id < b.id); });
	}
	void append(const Vehicle& vehicle)
	{
		const auto item = occupant(vehicle);
		if (vehicle.location == VehicleLocation::OnConnection)
		{
			m_connections[key(vehicle.connectionNodeId, vehicle.connectionId)] << item;
			return;
		}
		m_lanes[key(vehicle.currentEdge, vehicle.currentLane)] << item;
		if (vehicle.location == VehicleLocation::ChangingLane && vehicle.laneTo != vehicle.currentLane)
		{
			m_lanes[key(vehicle.currentEdge, vehicle.laneTo)] << item;
		}
	}
};
