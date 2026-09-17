#include "TrainNetwork.hpp"
#include "RailTimetable.hpp"
#include <queue>

int TrainNetwork::addNode(Vec3 pos, TrackNodeType type, const String& name)
{
	// ノードは連番 ID で所有し、駅や分岐点を同じ配列上で管理する。
	TrackNode node;
	node.id       = m_nextNodeId++;
	node.position = pos;
	node.type     = type;
	node.name     = name;
	m_nodes << std::move(node);
	return m_nodes.back().id;
}

int TrainNetwork::addEdge(int nodeA, int nodeB, Vec3 ctrlA, Vec3 ctrlB, float speedLimit)
{
	// エッジ追加時に曲線長と接続関係をまとめて確定し、以後の走行計算で再計算を避ける。
	TrackEdge edge;
	edge.id         = m_nextEdgeId++;
	edge.nodeA      = nodeA;
	edge.nodeB      = nodeB;
	edge.ctrlA      = ctrlA;
	edge.ctrlB      = ctrlB;
	edge.speedLimit = speedLimit;

	// 制御点から近似弧長を計算する
	const int ia = nodeIndex(nodeA);
	const int ib = nodeIndex(nodeB);
	const Vec3 posA = (ia >= 0) ? m_nodes[ia].position : Vec3{ 0, 0, 0 };
	const Vec3 posB = (ib >= 0) ? m_nodes[ib].position : Vec3{ 0, 0, 0 };
	const CubicBezier bez{ posA, ctrlA, ctrlB, posB };
	edge.length = bez.totalLength;

	// ノードにエッジを登録する
	if (ia >= 0) m_nodes[ia].edgeIds << edge.id;
	if (ib >= 0) m_nodes[ib].edgeIds << edge.id;

	m_edges << std::move(edge);
	return m_edges.back().id;
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
	const int i = edgeIndex(id);
	return i >= 0 ? &m_edges[i] : nullptr;
}

const TrackEdge* TrainNetwork::getEdge(int id) const
{
	const int i = edgeIndex(id);
	return i >= 0 ? &m_edges[i] : nullptr;
}

Optional<CubicBezier> TrainNetwork::getBezier(int edgeId) const
{
	// 線路形状は edge 単体では完結しないため、両端ノード位置と制御点から都度復元する。
	const int i = edgeIndex(edgeId);
	if (i < 0) return none;
	const TrackEdge& e = m_edges[i];

	const TrackNode* nA = getNode(e.nodeA);
	const TrackNode* nB = getNode(e.nodeB);
	if (!nA || !nB) return none;

	return CubicBezier{ nA->position, e.ctrlA, e.ctrlB, nB->position };
}

bool TrainNetwork::tryOccupy(int edgeId, int trainId)
{
	// 単線区間の衝突を避けるため、占有は空き区間か自列車の再取得だけを許可する。
	TrackEdge* e = getEdge(edgeId);
	if (!e) return false;
	if (e->occupiedBy >= 0 && e->occupiedBy != trainId) return false;
	e->occupiedBy = trainId;
	return true;
}

void TrainNetwork::releaseOccupy(int edgeId, int trainId)
{
	TrackEdge* e = getEdge(edgeId);
	if (e && e->occupiedBy == trainId)
		e->occupiedBy = -1;
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
	return id>=0 && id<static_cast<int>(m_nodes.size()) && m_nodes[id].id==id ? id : -1;
}

int TrainNetwork::edgeIndex(int id) const
{
	return id>=0 && id<static_cast<int>(m_edges.size()) && m_edges[id].id==id ? id : -1;
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
			const auto* edge=getEdge(id); const int next=edge->nodeA==node ? edge->nodeB : edge->nodeA;
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
