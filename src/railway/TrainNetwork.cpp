#include "TrainNetwork.hpp"
#include "RailTimetable.hpp"
#include <queue>

void TrainNetwork::registerNode(int id)
{
	if (m_nodeIndex.contains(id)) { return; }
	const auto* source = infrastructure().getNode(id); if (!source) { return; }
	TrackNode node; node.id = id; node.position = source->position;
	m_nodeIndex[id] = static_cast<int>(m_nodes.size()); m_nodes << std::move(node);
}

void TrainNetwork::synchronize()
{
	m_edgeIds.clear();
	// 共通網から撤去された駅・接続点は、表示や保存用の参照にも残さない。
	m_nodes.remove_if([&](const TrackNode& node) { return !infrastructure().getNode(node.id); });
	m_nodeIndex.clear();
	for (size_t i=0;i<m_nodes.size();++i) { m_nodeIndex[m_nodes[i].id]=static_cast<int>(i); m_nodes[i].edgeIds.clear(); }
	m_depots.remove_if([&](const RailDepot& depot)
	{
		return !getNode(depot.stationNodeId) || !getNode(depot.throatNodeId)
			|| depot.sidingNodes.any([&](int id) { return !getNode(id); });
	});
	for (const auto& edge : infrastructure().edges())
	{
		if (edge.id < 0 || !edge.hasRailLanes()) { continue; }
		m_edgeIds << edge.id;
		registerNode(edge.nodeA); registerNode(edge.nodeB);
		for (const int id : {edge.nodeA,edge.nodeB})
		{
			auto& node = m_nodes[m_nodeIndex.at(id)];
			node.position = infrastructure().getNode(id)->position; node.edgeIds << edge.id;
		}
	}
}

int TrainNetwork::addNode(Vec3 pos, TrackNodeType type, const String& name)
{
	const int id = infrastructure().addNode(pos);
	registerNode(id);
	auto* node = getNode(id); node->type = type; node->name = name;
	return id;
}

int TrainNetwork::addEdge(int nodeA, int nodeB, Vec3 ctrlA, Vec3 ctrlB, float speedLimit, bool doubleTrack)
{
	if (!getNode(nodeA) || !getNode(nodeB)) { return -1; }
	const auto id = infrastructure().addEdge(nodeA,nodeB,ctrlA,ctrlB);
	if (!id) { return -1; }
	auto* edge = infrastructure().getEdge(*id);
	TransportCrossSection::railway(*edge,doubleTrack);
	edge->speedLimit = speedLimit; edge->edgeState = EdgeState::Existing;
	edge->designGrade = true;
	// 接続は道路網が所有し、鉄道側は運行に必要な軌道だけを参照する。
	m_edgeIds << *id;
	getNode(nodeA)->edgeIds << *id; getNode(nodeB)->edgeIds << *id;
	infrastructure().updateNodeCutoffs(nodeA); infrastructure().updateNodeCutoffs(nodeB);
	infrastructure().rebuildLaneConnections(nodeA); infrastructure().rebuildLaneConnections(nodeB);
	return *id;
}

int TrainNetwork::addStation(Vec3 pos, const String& name)
{
	// 駅は通常ノード生成の薄いラッパーとして扱い、種別だけ明示して追加する。
	return addNode(pos, TrackNodeType::Station, name);
}

TrackNode* TrainNetwork::getNode(int id)
{
	const int i = nodeIndex(id);
	return i >= 0 ? &m_nodes[i] : nullptr;
}

const TrackNode* TrainNetwork::getNode(int id) const
{
	const int i = nodeIndex(id);
	return i >= 0 ? &m_nodes[i] : nullptr;
}

TrackEdge* TrainNetwork::getEdge(int id)
{
	auto* edge = infrastructure().getEdge(id);
	return edge && edge->hasRailLanes() ? edge : nullptr;
}
const TrackEdge* TrainNetwork::getEdge(int id) const
{
	const auto* edge = infrastructure().getEdge(id);
	return edge && edge->hasRailLanes() ? edge : nullptr;
}
Optional<CubicBezier> TrainNetwork::getBezier(int edgeId) const
{
	return getEdge(edgeId) ? infrastructure().getBezier(edgeId) : none;
}

bool TrainNetwork::canOccupy(int edgeId, int trainId, bool forward) const
{
	const auto* edge = getEdge(edgeId); if (!edge) { return false; }
	const int lane = TransportCrossSection::railLane(*edge, forward);
	if (lane < 0) { return false; }
	const int occupant = edge->lanes[lane].reservedBy;
	return (occupant < 0 || occupant == trainId)
		&& (!edge->lanes[lane].bidirectional || edge->occupiedBy < 0 || edge->occupiedBy == trainId);
}

bool TrainNetwork::tryOccupy(int edgeId, int trainId, bool forward)
{
	if (!canOccupy(edgeId, trainId, forward)) { return false; }
	auto* edge = getEdge(edgeId); const int lane = TransportCrossSection::railLane(*edge,forward);
	edge->lanes[lane].reservedBy = trainId;
	if (edge->lanes[lane].bidirectional) { edge->occupiedBy = trainId; }
	return true;
}
void TrainNetwork::releaseOccupy(int edgeId, int trainId)
{
	if (auto* edge = getEdge(edgeId))
	{
		for (auto& lane : edge->lanes) { if (lane.reservedBy == trainId) { lane.reservedBy = -1; } }
		if (edge->occupiedBy == trainId) { edge->occupiedBy = -1; }
	}
}

int TrainNetwork::addSchedule(TrainSchedule schedule)
{
	int nextId = 0;
	for (const auto& current : m_schedules) { nextId = Max(nextId, current.id + 1); }
	if (schedule.id < 0 || getSchedule(schedule.id)) { schedule.id = nextId; }
	m_schedules << std::move(schedule);
	return m_schedules.back().id;
}

TrainSchedule* TrainNetwork::getSchedule(int id)
{
	for (auto& schedule : m_schedules) { if (schedule.id == id) { return &schedule; } }
	return nullptr;
}
const TrainSchedule* TrainNetwork::getSchedule(int id) const
{
	for (const auto& schedule : m_schedules) { if (schedule.id == id) { return &schedule; } }
	return nullptr;
}
bool TrainNetwork::applySchedule(const TrainSchedule& schedule, String& error)
{
	error = RailTimetable::validate(*this, schedule);
	if (!error.isEmpty()) { return false; }
	if (auto* current = getSchedule(schedule.id))
	{
		const auto lastSpawn = current->lastSpawnAt;
		const bool reverse = current->reverseNext;
		*current = schedule;
		current->lastSpawnAt = lastSpawn;
		current->reverseNext = reverse;
	}
	else { addSchedule(schedule); }
	return true;
}

int TrainNetwork::nodeIndex(int id) const
{
	const auto found = m_nodeIndex.find(id);
	return found == m_nodeIndex.end() ? -1 : found->second;
}

Array<int> TrainNetwork::findRoute(int from,int to) const
{
	if (!getNode(from) || !getNode(to)) { return {}; }
	HashTable<int,double> distance; HashTable<int,int> parent;
	std::priority_queue<std::pair<double,int>,std::vector<std::pair<double,int>>,std::greater<>> pending;
	distance[from]=0; pending.emplace(0,from);
	while (!pending.empty())
	{
		const auto [cost,node]=pending.top(); pending.pop();
		if (node==to) { break; }
		if (cost>distance[node]) { continue; }
		for (const int id : getNode(node)->edgeIds)
		{
			const auto* edge=getEdge(id);
			if (!edge || !edge->electrified || TransportCrossSection::railLane(*edge,edge->nodeA==node)<0) { continue; }
			const int next=edge->nodeA==node ? edge->nodeB : edge->nodeA;
			const double candidate=cost+edge->length;
			if (!distance.contains(next) || candidate<distance[next])
			{
				distance[next]=candidate; parent[next]=id; pending.emplace(candidate,next);
			}
		}
	}
	if (!distance.contains(to)) { return {}; }
	Array<int> route;
	for (int node=to;node!=from;)
	{
		const int id=parent[node]; route << id;
		const auto* edge=getEdge(id); node=edge->nodeA==node ? edge->nodeB : edge->nodeA;
	}
	route.reverse(); return route;
}
