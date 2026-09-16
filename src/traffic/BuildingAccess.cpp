#include "BuildingAccess.hpp"
#include <bit>

Optional<BuildingAccessPoint> BuildingAccessIndex::project(const Chunk& chunk, Point cell,
	const RoadNetwork& roads, const SimGraph& graph)
{
	const auto& building = chunk.buildingGrid[cell];
	if (building.type == BuildingType::None || building.type == BuildingType::Farmland || building.type == BuildingType::ParkBuilding) { return none; }
	const auto* road = roads.getEdge(building.edgeId);
	const auto* edge = graph.getEdge(building.edgeId);
	if (!road || !edge || road->useElevation || !edge->isRoadbedBuilt()) { return none; }
	const auto curve = roads.getBezier(edge->id);
	if (!curve) { return none; }
	const float arc = Clamp(building.edgeT, 0.0f, 1.0f) * curve->totalLength;
	constexpr float kJunctionClearance = 10;
	if (arc < road->cutoffA + kJunctionClearance || arc > edge->length - road->cutoffB - kJunctionClearance) { return none; }
	const Vec3 center = curve->positionAt(arc), right = tangentToRight(curve->tangentAt(arc));
	constexpr double kCell = static_cast<double>(CHUNK_SIZE) / ZONE_CELLS;
	const Vec2 buildingCenter{chunk.coord.x * CHUNK_SIZE + (cell.x + .5) * kCell + building.offsetX,
		chunk.coord.y * CHUNK_SIZE + (cell.y + .5) * kCell + building.offsetZ};
	const double side = (buildingCenter - Vec2{center.x, center.z}).dot(Vec2{right.x, right.z});
	Optional<int> lane;
	double best = std::numeric_limits<double>::max();
	for (int i = 0; i < static_cast<int>(edge->lanes.size()); ++i)
	{
		if (!TrafficSpawn::laneOpen(*edge, i)) { continue; }
		const double distance = Abs(side - road->lanes[i].centerAt(building.edgeT));
		if (distance < best) { best = distance; lane = i; }
	}
	constexpr double kMaximumFrontageDistance = 80;
	if (!lane || best > kMaximumFrontageDistance) { return none; }
	const Vec3 position = center + right * road->lanes[*lane].centerAt(building.edgeT);
	const int chunkIndex = chunk.coord.y * WORLD_CHUNKS + chunk.coord.x;
	const int64 key = static_cast<int64>(chunkIndex) * ZONE_CELLS * ZONE_CELLS + cell.y * ZONE_CELLS + cell.x;
	return BuildingAccessPoint{key, edge->id, *lane, arc, {position.x, position.z}, building.type};
}

void BuildingAccessIndex::updateChunk(int index, const World& world, const RoadNetwork& roads,
	const SimGraph& graph, bool force)
{
	const auto* chunk = world.getChunk({index % WORLD_CHUNKS, index / WORLD_CHUNKS});
	if (!chunk) { return; }
	uint64 fingerprint = 14695981039346656037ULL;
	for (const auto& building : chunk->buildingGrid)
	{
		for (const uint32 value : {static_cast<uint32>(building.type), static_cast<uint32>(building.edgeId),
			std::bit_cast<uint32>(building.edgeT), std::bit_cast<uint32>(building.offsetX), std::bit_cast<uint32>(building.offsetZ)})
		{
			fingerprint = (fingerprint ^ value) * 1099511628211ULL;
		}
	}
	if (!force && m_fingerprints[index] == fingerprint) { return; }
	m_fingerprints[index] = fingerprint;
	for (const int edge : m_chunkEdges[index])
	{
		auto found = m_edges.find(edge);
		if (found == m_edges.end()) { continue; }
		m_size -= found->second.size();
		found->second.remove_if([index](const auto& point) { return point.buildingKey / (ZONE_CELLS * ZONE_CELLS) == index; });
		m_size += found->second.size();
		if (found->second.isEmpty()) { m_edges.erase(found); }
	}
	m_chunkEdges[index].clear();
	for (int row = 0; row < ZONE_CELLS; ++row)
	{
		for (int col = 0; col < ZONE_CELLS; ++col)
		{
			if (const auto point = project(*chunk, {col, row}, roads, graph))
			{
				m_edges[point->edgeId] << *point; ++m_size;
				if (!m_chunkEdges[index].contains(point->edgeId)) { m_chunkEdges[index] << point->edgeId; }
			}
		}
	}
}

void BuildingAccessIndex::rebuild(const World& world, const RoadNetwork& roads, const SimGraph& graph)
{
	m_edges.clear(); m_size = 0; m_cursor = 0;
	for (auto& edges : m_chunkEdges) { edges.clear(); }
	for (int index = 0; index < WORLD_CHUNKS * WORLD_CHUNKS; ++index) { updateChunk(index, world, roads, graph, true); }
}

void BuildingAccessIndex::refresh(const World& world, const RoadNetwork& roads, const SimGraph& graph)
{
	constexpr int kChunksPerFrame = 8;
	for (int i = 0; i < kChunksPerFrame; ++i)
	{
		updateChunk(m_cursor, world, roads, graph, false);
		m_cursor = (m_cursor + 1) % (WORLD_CHUNKS * WORLD_CHUNKS);
	}
}

const Array<BuildingAccessPoint>& BuildingAccessIndex::onEdge(int edge) const
{
	static const Array<BuildingAccessPoint> empty;
	const auto found = m_edges.find(edge);
	return found == m_edges.end() ? empty : found->second;
}
