#include "TrainNetwork.hpp"
#include "RailTimetable.hpp"
#include "../road/RoadPreset.hpp"

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
	state[U"version"] = 2;
	state[U"shared"] = sharedInfrastructure();
	state[U"nodes"] = Array<JSON>{}; state[U"edges"] = Array<JSON>{};
	state[U"schedules"] = Array<JSON>{}; state[U"depots"] = Array<JSON>{};
	for (const auto& node : m_nodes)
	{
		JSON item; item[U"id"] = node.id; item[U"type"] = static_cast<int>(node.type); item[U"name"] = node.name;
		item[U"stationKind"] = static_cast<int>(node.stationKind);
		if (node.entrance) { item[U"entrance"] = vectorJson(*node.entrance); }
		if (!sharedInfrastructure()) { item[U"position"] = vectorJson(node.position); }
		state[U"nodes"].push_back(item);
	}
	for (const auto& edge : edges())
	{
		JSON item; item[U"id"] = edge.id;
		// 本体の形状・断面は roads.bin に一度だけ保存する。
		if (!sharedInfrastructure())
		{
			item[U"a"] = edge.nodeA; item[U"b"] = edge.nodeB;
			item[U"ctrlA"] = vectorJson(edge.ctrlA); item[U"ctrlB"] = vectorJson(edge.ctrlB);
			item[U"speed"] = edge.speedLimit; item[U"electric"] = edge.electrified; item[U"depot"] = edge.depotTrack;
			item[U"parts"] = Array<JSON>{}; item[U"lanes"] = Array<JSON>{};
			for (const auto& part : edge.parts) { item[U"parts"].push_back(RoadSectionJson::partToJson(part)); }
			for (const auto& lane : edge.lanes) { item[U"lanes"].push_back(RoadSectionJson::laneToJson(lane)); }
		}
		state[U"edges"].push_back(item);
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
	if (!state || state[U"version"].getOr<int>(0) != 2) { return false; }
	const bool shared = state[U"shared"].getOr<bool>(false);
	if (shared && !m_sharedRoads) { return false; }
	TrainNetwork restored;
	if (shared) { restored.m_sharedRoads = m_sharedRoads; }
	try
	{
		for (const auto& item : state[U"nodes"].arrayView())
		{
			const int id = item[U"id"].get<int>(), type = item[U"type"].get<int>();
			if (id < 0 || !InRange(type,0,3) || restored.getNode(id)) { return false; }
			if (shared)
			{
				if (!restored.infrastructure().getNode(id)) { return false; }
				restored.registerNode(id);
			}
			else
			{
				const Vec3 point = readVector(item[U"position"]);
				if (!finite(point) || restored.addNode(point) != id) { return false; }
			}
			auto* node = restored.getNode(id); node->type = static_cast<TrackNodeType>(type); node->name = item[U"name"].get<String>();
			const int kind=item[U"stationKind"].getOr<int>(0);
			if (!InRange(kind,0,2)) { return false; }
			node->stationKind=static_cast<StationKind>(kind);
			if (item.contains(U"entrance"))
			{
				const Vec3 entrance=readVector(item[U"entrance"]);
				if (!finite(entrance)) { return false; }
				node->entrance=entrance;
			}
		}
		for (const auto& item : state[U"edges"].arrayView())
		{
			const int id = item[U"id"].get<int>();
			if (shared)
			{
				const auto* edge = restored.getEdge(id);
				if (!edge || !restored.getNode(edge->nodeA) || !restored.getNode(edge->nodeB)) { return false; }
				continue;
			}
			const int a = item[U"a"].get<int>(), b = item[U"b"].get<int>();
			const Vec3 ctrlA = readVector(item[U"ctrlA"]), ctrlB = readVector(item[U"ctrlB"]);
			const float speed = item[U"speed"].get<float>();
			if (!restored.getNode(a) || !restored.getNode(b) || a == b || !finite(ctrlA) || !finite(ctrlB) || !std::isfinite(speed) || speed <= 0) { return false; }
			if (restored.addEdge(a,b,ctrlA,ctrlB,speed) != id) { return false; }
			auto* edge = restored.getEdge(id); edge->electrified = item[U"electric"].get<bool>(); edge->depotTrack = item[U"depot"].get<bool>();
			edge->parts.clear(); edge->lanes.clear();
			for (const auto& part : item[U"parts"].arrayView()) { edge->parts << RoadSectionJson::partFromJson(part); }
			for (const auto& lane : item[U"lanes"].arrayView()) { edge->lanes << RoadSectionJson::laneFromJson(lane); }
			if (!edge->hasRailLanes() || !edge->isRoadbedBuilt()) { return false; }
		}
		restored.synchronize();
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
			// 共通網は駅・線路を撤去できる。途切れたダイヤは修正用に保存し、発車時に経路を検査する。
			const String error = shared ? RailTimetable::validateSettings(schedule) : RailTimetable::validate(restored,schedule);
			if (!error.isEmpty()) { return false; }
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
