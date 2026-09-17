#include "TrainNetwork.hpp"
#include "RailTimetable.hpp"

namespace
{
	JSON vectorJson(Vec3 point) { JSON value; value = Array<double>{point.x, point.y, point.z}; return value; }
	Vec3 readVector(const JSON& value)
	{
		return {value[0].get<double>(), value[1].get<double>(), value[2].get<double>()};
	}
	bool finite(Vec3 point) { return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z); }
}
JSON TrainNetwork::saveState() const
{
	JSON state;
	state[U"version"] = 1;
	state[U"nodes"] = Array<JSON>{}; state[U"edges"] = Array<JSON>{};
	state[U"schedules"] = Array<JSON>{}; state[U"depots"] = Array<JSON>{};
	for (const auto& node : m_nodes)
	{
		JSON item; item[U"position"] = vectorJson(node.position); item[U"type"] = static_cast<int>(node.type); item[U"name"] = node.name;
		state[U"nodes"][node.id] = item;
	}
	for (const auto& edge : m_edges)
	{
		JSON item; item[U"a"] = edge.nodeA; item[U"b"] = edge.nodeB;
		item[U"ctrlA"] = vectorJson(edge.ctrlA); item[U"ctrlB"] = vectorJson(edge.ctrlB);
		item[U"speed"] = edge.speedLimit; item[U"electric"] = edge.electrified; item[U"depot"] = edge.depotTrack;
		state[U"edges"][edge.id] = item;
	}
	for (size_t index = 0; index < m_schedules.size(); ++index)
	{
		const auto& schedule = m_schedules[index]; JSON item;
		item[U"id"] = schedule.id; item[U"name"] = schedule.name; item[U"enabled"] = schedule.enabled;
		item[U"first"] = schedule.firstDepartureMinute; item[U"last"] = schedule.lastDepartureMinute;
		item[U"interval"] = schedule.headwaySec; item[U"type"] = static_cast<int>(schedule.type);
		item[U"reverse"] = schedule.reverseNext; item[U"lastSpawn"] = schedule.lastSpawnAt;
		item[U"stops"] = Array<JSON>{};
		for (size_t stop = 0; stop < schedule.stops.size(); ++stop)
		{
			item[U"stops"][stop][U"station"] = schedule.stops[stop].stationNodeId;
			item[U"stops"][stop][U"dwell"] = schedule.stops[stop].dwellSec;
		}
		state[U"schedules"][index] = item;
	}
	for (size_t index = 0; index < m_depots.size(); ++index)
	{
		const auto& depot = m_depots[index]; JSON item;
		item[U"station"] = depot.stationNodeId; item[U"throat"] = depot.throatNodeId;
		item[U"sidings"] = depot.sidingNodes; item[U"name"] = depot.name;
		state[U"depots"][index] = item;
	}
	return state;
}
bool TrainNetwork::restoreState(const JSON& state)
{
	if (!state || state[U"version"].getOr<int>(0) != 1) { return false; }
	TrainNetwork restored;
	try
	{
		for (const auto& item : state[U"nodes"].arrayView())
		{
			const Vec3 point = readVector(item[U"position"]); const int type = item[U"type"].get<int>();
			if (!finite(point) || !InRange(type, 0, 3)) { return false; }
			restored.addNode(point, static_cast<TrackNodeType>(type), item[U"name"].get<String>());
		}
		for (const auto& item : state[U"edges"].arrayView())
		{
			const int a = item[U"a"].get<int>(), b = item[U"b"].get<int>();
			const Vec3 ctrlA = readVector(item[U"ctrlA"]), ctrlB = readVector(item[U"ctrlB"]);
			const float speed = item[U"speed"].get<float>();
			if (!restored.getNode(a) || !restored.getNode(b) || a == b || !finite(ctrlA) || !finite(ctrlB) || !std::isfinite(speed) || speed <= 0) { return false; }
			const int id = restored.addEdge(a, b, ctrlA, ctrlB, speed);
			auto* edge = restored.getEdge(id); edge->electrified = item[U"electric"].get<bool>(); edge->depotTrack = item[U"depot"].get<bool>();
		}
		for (const auto& item : state[U"schedules"].arrayView())
		{
			TrainSchedule schedule; schedule.id = item[U"id"].get<int>(); schedule.name = item[U"name"].get<String>();
			schedule.enabled = item[U"enabled"].get<bool>(); schedule.firstDepartureMinute = item[U"first"].get<int>();
			schedule.lastDepartureMinute = item[U"last"].get<int>(); schedule.headwaySec = item[U"interval"].get<float>();
			const int type = item[U"type"].get<int>();
			if (!InRange(type, 0, 4) || restored.getSchedule(schedule.id) || schedule.id < 0) { return false; }
			schedule.type = static_cast<TrainType>(type); schedule.reverseNext = item[U"reverse"].get<bool>();
			schedule.lastSpawnAt = item[U"lastSpawn"].get<double>();
			if (!std::isfinite(schedule.lastSpawnAt)) { return false; }
			for (const auto& stop : item[U"stops"].arrayView()) { schedule.stops << StopEntry{stop[U"station"].get<int>(), stop[U"dwell"].get<float>()}; }
			if (!RailTimetable::validate(restored, schedule).isEmpty()) { return false; }
			restored.addSchedule(schedule);
		}
		for (const auto& item : state[U"depots"].arrayView())
		{
			RailDepot depot; depot.stationNodeId = item[U"station"].get<int>(); depot.throatNodeId = item[U"throat"].get<int>();
			depot.name = item[U"name"].get<String>();
			for (const auto& id : item[U"sidings"].arrayView()) { depot.sidingNodes << id.get<int>(); }
			if (!restored.getNode(depot.stationNodeId) || !restored.getNode(depot.throatNodeId) || depot.sidingNodes.size() != 2) { return false; }
			for (const int id : depot.sidingNodes) { if (!restored.getNode(id) || restored.findRoute(depot.throatNodeId, id).isEmpty()) { return false; } }
			restored.m_depots << std::move(depot);
		}
	}
	catch (const Error&) { return false; }
	*this = std::move(restored);
	return true;
}
