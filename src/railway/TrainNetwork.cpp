#include "TrainNetwork.hpp"

int TrainNetwork::addNode(Vec3 pos, TrackNodeType type, const String& name)
{
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

void TrainNetwork::addSchedule(TrainSchedule schedule)
{
	m_schedules << std::move(schedule);
}

int TrainNetwork::nodeIndex(int id) const
{
	for (int i = 0; i < static_cast<int>(m_nodes.size()); ++i)
		if (m_nodes[i].id == id) return i;
	return -1;
}

int TrainNetwork::edgeIndex(int id) const
{
	for (int i = 0; i < static_cast<int>(m_edges.size()); ++i)
		if (m_edges[i].id == id) return i;
	return -1;
}
