#include "PedestrianNetwork.hpp"
#include "../railway/RailwaySite.hpp"
#include <queue>

namespace
{
constexpr double kSpatialCell = 200;
constexpr size_t kRouteCacheCapacity = 512;
Point spatialCell(Vec3 p)
{
	return {static_cast<int>(Floor(p.x / kSpatialCell)), static_cast<int>(Floor(p.z / kSpatialCell))};
}
bool parkingSite(BuildingType type)
{
	return type == BuildingType::Parking || type == BuildingType::RoadsideConvenience ||
		   type == BuildingType::RoadsideFuelStation;
}
} // namespace
int PedestrianNetwork::addNode(Vec3 point)
{
	const int id = static_cast<int>(m_nodes.size());
	m_nodes << Node{point};
	m_spatial[spatialCell(point)] << id;
	return id;
}
void PedestrianNetwork::addLink(Link link)
{
	if (link.a == link.b)
	{
		return;
	}
	if (link.roadShape < 0)
	{
		link.length = static_cast<float>(m_nodes[link.a].position.distanceFrom(m_nodes[link.b].position));
	}
	link.length = Max(.01f, link.length);
	const int id = static_cast<int>(m_links.size());
	m_links << link;
	m_nodes[link.a].links << id;
	m_nodes[link.b].links << id;
}
Vec3 PedestrianNetwork::onShape(int shape, int side, float arc) const
{
	const auto& data = m_shapes[shape];
	const float t = data.curve.tFromArcLength(arc);
	const double fraction = arc / Max(.01f, data.curve.totalLength);
	return data.curve.evaluate(t) +
		   tangentToRight(data.curve.tangent(t)) * Math::Lerp(data.offsets[side].x, data.offsets[side].y, fraction) +
		   Vec3{0, data.heights[side], 0};
}
Vec3 PedestrianNetwork::position(int step, float distance) const
{
	const auto& link = m_links[Abs(step) - 1];
	const double t = Clamp(static_cast<double>(distance) / link.length, 0.0, 1.0);
	const double forward = step > 0 ? t : 1 - t;
	if (link.roadShape >= 0)
	{
		return onShape(link.roadShape, link.side, static_cast<float>(Math::Lerp(link.arcA, link.arcB, forward)));
	}
	return m_nodes[link.a].position.lerp(m_nodes[link.b].position, forward);
}
Optional<int> PedestrianNetwork::nearestNode(Vec3 point, double maximum) const
{
	Optional<int> best;
	double distance = maximum * maximum;
	const Point cell = spatialCell(point);
	const int radius = static_cast<int>(Ceil(maximum / kSpatialCell));
	for (int z = -radius; z <= radius; ++z)
	{
		for (int x = -radius; x <= radius; ++x)
		{
			const auto found = m_spatial.find(cell + Point{x, z});
			if (found == m_spatial.end())
			{
				continue;
			}
			for (const int id : found->second)
			{
				const double candidate = m_nodes[id].position.distanceFromSq(point);
				if (candidate < distance)
				{
					distance = candidate;
					best = id;
				}
			}
		}
	}
	return best;
}
Optional<size_t> PedestrianNetwork::siteIndex(int64 key) const
{
	const auto found = m_siteIndices.find(key);
	return found == m_siteIndices.end() ? none : Optional<size_t>{found->second};
}
Optional<size_t> PedestrianNetwork::stationIndex(int id) const
{
	const auto found = m_stationIndices.find(id);
	return found == m_stationIndices.end() ? none : Optional<size_t>{found->second};
}
void PedestrianNetwork::rebuild(
	const World& world, const RoadNetwork& roads, const BuildingAccessIndex& access, const TrainNetwork& trains)
{
	++m_revision;
	m_nodes.clear();
	m_links.clear();
	m_shapes.clear();
	m_sites.clear();
	m_stations.clear();
	m_siteIndices.clear();
	m_stationIndices.clear();
	m_spatial.clear();
	m_cache.clear();
	m_cacheKeys.clear();
	m_cacheCursor = 0;
	struct Port
	{
		int walkNode, edge;
		double angle;
	};
	HashTable<int, Array<Port>> junctions;
	for (const auto& edge : roads.edges())
	{
		if (edge.id < 0 || !edge.isRoadbedBuilt() ||
			(edge.edgeState != EdgeState::Open && edge.edgeState != EdgeState::Existing) ||
			edge.roadType == RoadType::Expressway || edge.roadType == RoadType::Highway)
		{
			continue;
		}
		const auto curve = roads.getBezier(edge.id);
		if (!curve || curve->totalLength < 3)
		{
			continue;
		}
		Shape shape;
		shape.curve = *curve;
		std::array<bool, 2> available{};
		for (int side = 0; side < 2; ++side)
		{
			const double sign = side == 0 ? -1.0 : 1.0;
			for (const auto& part : edge.parts)
			{
				if (part.build != BuildState::Built || part.type != RoadPartType::Sidewalk || part.width() < .7f ||
					part.offsetL() * sign < 0)
				{
					continue;
				}
				shape.offsets[side] = {(part.offsetA_L + part.offsetA_R) * .5, (part.offsetB_L + part.offsetB_R) * .5};
				shape.heights[side] = .18;
				available[side] = true;
				break;
			}
			// 歩道のない生活道路は路肩を歩く。広い幹線・高速道路へ無理に徒歩経路を作らない。
			if (!available[side] && edge.hasRoadLanes() && edge.roadType == RoadType::LocalRoad)
			{
				double outerA = 0, outerB = 0;
				for (const auto& part : edge.parts)
				{
					if (part.type == RoadPartType::Roadbed || part.type == RoadPartType::Shoulder)
					{
						outerA = Max(outerA, sign * (side == 0 ? part.offsetA_L : part.offsetA_R));
						outerB = Max(outerB, sign * (side == 0 ? part.offsetB_L : part.offsetB_R));
					}
				}
				shape.offsets[side] = {sign * Max(.7, outerA - .35), sign * Max(.7, outerB - .35)};
				shape.heights[side] = .03;
				available[side] = true;
			}
		}
		if (!available[0] && !available[1])
		{
			continue;
		}
		const int shapeId = static_cast<int>(m_shapes.size());
		m_shapes << std::move(shape);
		const float start = Min(Max(1.5f, edge.cutoffA), edge.length * .2f),
					end = Max(start + .1f, edge.length - Min(Max(1.5f, edge.cutoffB), edge.length * .2f));
		for (int side = 0; side < 2; ++side)
		{
			if (!available[side])
			{
				continue;
			}
			struct Stop
			{
				float arc;
				Optional<BuildingAccessPoint> access;
			};
			Array<Stop> stops{{start, none}, {end, none}};
			// 長い無接道区間も、駅の出入口を近い歩道へ接続できる間隔に区切る。
			constexpr float kAccessInterval = 60;
			for (float arc = start + kAccessInterval; arc < end; arc += kAccessInterval)
			{
				stops << Stop{arc, none};
			}
			for (const auto& point : access.onEdge(edge.id))
			{
				const int laneSide = edge.lanes[point.lane].centerAt(point.arc / edge.length) < 0 ? 0 : 1;
				if (side != (available[laneSide] ? laneSide : 1 - laneSide))
				{
					continue;
				}
				stops << Stop{Clamp(point.arc, start, end), point};
			}
			stops.sort_by([](const Stop& a, const Stop& b) { return a.arc < b.arc; });
			int previous = -1, first = -1;
			float previousArc = start;
			for (const auto& stop : stops)
			{
				const Vec3 position = onShape(shapeId, side, stop.arc);
				const int node = previous >= 0 && Abs(stop.arc - previousArc) < .01 ? previous : addNode(position);
				if (first < 0)
				{
					first = node;
				}
				if (previous >= 0 && node != previous)
				{
					addLink({previous, node, shapeId, -1, -1, previousArc, stop.arc, stop.arc - previousArc, side});
				}
				if (stop.access)
				{
					const auto& point = *stop.access;
					const int chunkIndex = static_cast<int>(point.buildingKey / (ZONE_CELLS * ZONE_CELLS));
					const int cell = static_cast<int>(point.buildingKey % (ZONE_CELLS * ZONE_CELLS));
					const auto* chunk = world.getChunk({chunkIndex % WORLD_CHUNKS, chunkIndex / WORLD_CHUNKS});
					if (chunk)
					{
						const auto& building = chunk->buildingGrid[Point{cell % ZONE_CELLS, cell / ZONE_CELLS}];
						constexpr double kCell = static_cast<double>(CHUNK_SIZE) / ZONE_CELLS;
						Vec3 center{chunk->coord.x * CHUNK_SIZE + (cell % ZONE_CELLS + .5) * kCell + building.offsetX,
							position.y,
							chunk->coord.y * CHUNK_SIZE + (cell / ZONE_CELLS + .5) * kCell + building.offsetZ};
						const Vec3 toward = position - center;
						const double distance = toward.length();
						const Vec3 entrance =
							distance > .1
								? center +
									  toward * (Min(distance, buildingFootprintXZ(point.type) * .5 + .2) / distance)
								: center;
						const int siteNode = addNode(entrance);
						addLink({node, siteNode});
						m_siteIndices[point.buildingKey] = m_sites.size();
						m_sites << PedestrianSite{
							point.buildingKey, siteNode, entrance, point, parkingSite(point.type)};
					}
				}
				previous = node;
				previousArc = stop.arc;
			}
			for (const auto endpoint :
				std::array<std::pair<int, int>, 2>{{{edge.nodeA, first}, {edge.nodeB, previous}}})
			{
				const Vec3 direction = m_nodes[endpoint.second].position - roads.getNode(endpoint.first)->position;
				junctions[endpoint.first] << Port{endpoint.second, edge.id, Math::Atan2(direction.z, direction.x)};
			}
		}
	}
	for (auto& [node, ports] : junctions)
	{
		ports.sort_by([](const Port& a, const Port& b) { return a.angle < b.angle; });
		for (size_t i = 0; i < ports.size(); ++i)
		{
			if (ports.size() < 2 || (ports.size() == 2 && i == 1))
			{
				break;
			}
			const auto& a = ports[i];
			const auto& b = ports[(i + 1) % ports.size()];
			const bool crossing = a.edge == b.edge && roads.getEdge(a.edge)->hasRoadLanes();
			addLink({a.walkNode, b.walkNode, -1, crossing ? node : -1, crossing ? a.edge : -1});
		}
	}
	for (const auto& station : trains.nodes())
	{
		if (station.type != TrackNodeType::Station || station.edgeIds.isEmpty())
		{
			continue;
		}
		const auto frame = RailwaySite::stationFrame(trains, station.id);
		if (!frame)
		{
			continue;
		}
		const Vec3 platform = frame->point(-5.8, 1.08, 8);
		Vec3 entrance = frame->point(-12, 0, 8);
		entrance.y = world.sampleHeight(static_cast<float>(entrance.x), static_cast<float>(entrance.z)) + .03;
		const auto street = nearestNode(entrance, 350);
		if (!street)
		{
			continue;
		}
		const int doorway = addNode(entrance), wait = addNode(platform);
		addLink({*street, doorway});
		addLink({doorway, wait});
		m_stationIndices[station.id] = m_stations.size();
		m_stations << PedestrianStation{station.id, wait, platform};
	}
	components();
	m_costs.resize(m_nodes.size());
	m_parents.resize(m_nodes.size());
	m_marks.assign(m_nodes.size(), 0);
	m_search = 0;
}
void PedestrianNetwork::components()
{
	int component = 0;
	Array<int> pending;
	for (int root = 0; root < static_cast<int>(m_nodes.size()); ++root)
	{
		if (m_nodes[root].component >= 0)
		{
			continue;
		}
		pending.clear();
		pending << root;
		m_nodes[root].component = component;
		for (size_t i = 0; i < pending.size(); ++i)
		{
			for (const int id : m_nodes[pending[i]].links)
			{
				const auto& edge = m_links[id];
				const int next = edge.a == pending[i] ? edge.b : edge.a;
				if (m_nodes[next].component < 0)
				{
					m_nodes[next].component = component;
					pending << next;
				}
			}
		}
		++component;
	}
}
Optional<PedestrianNetwork::Route> PedestrianNetwork::route(int from, int to, int expansionLimit)
{
	m_lastExpansions = 0;
	if (!InRange(from, 0, static_cast<int>(m_nodes.size()) - 1) ||
		!InRange(to, 0, static_cast<int>(m_nodes.size()) - 1) || m_nodes[from].component != m_nodes[to].component)
	{
		return none;
	}
	const uint64 key = (static_cast<uint64>(from) << 32) | static_cast<uint32>(to);
	if (const auto found = m_cache.find(key); found != m_cache.end())
	{
		return found->second;
	}
	if (++m_search == 0)
	{
		m_marks.fill(0);
		++m_search;
	}
	std::priority_queue<std::pair<double, int>, std::vector<std::pair<double, int>>, std::greater<>> pending;
	m_marks[from] = m_search;
	m_costs[from] = 0;
	pending.emplace(0, from);
	bool reached = false;
	while (!pending.empty() && m_lastExpansions < expansionLimit)
	{
		const auto [estimate, node] = pending.top();
		pending.pop();
		if (estimate > m_costs[node] + m_nodes[node].position.distanceFrom(m_nodes[to].position) + .01)
		{
			continue;
		}
		if (node == to)
		{
			reached = true;
			break;
		}
		++m_lastExpansions;
		for (const int id : m_nodes[node].links)
		{
			const auto& link = m_links[id];
			const int next = link.a == node ? link.b : link.a;
			const double cost = m_costs[node] + link.length + (link.crossingNode >= 0 ? 4 : 0);
			if (m_marks[next] != m_search || cost < m_costs[next])
			{
				m_marks[next] = m_search;
				m_costs[next] = cost;
				m_parents[next] = link.a == node ? id + 1 : -id - 1;
				pending.emplace(cost + m_nodes[next].position.distanceFrom(m_nodes[to].position), next);
			}
		}
	}
	if (!reached)
	{
		return none;
	}
	Route result;
	for (int node = to; node != from;)
	{
		const int step = m_parents[node];
		const auto& link = m_links[Abs(step) - 1];
		result.steps << step;
		result.length += link.length;
		node = step > 0 ? link.a : link.b;
	}
	result.steps.reverse();
	if (m_cacheKeys.size() < kRouteCacheCapacity)
	{
		m_cacheKeys << key;
	}
	else
	{
		m_cache.erase(m_cacheKeys[m_cacheCursor]);
		m_cacheKeys[m_cacheCursor] = key;
		m_cacheCursor = (m_cacheCursor + 1) % kRouteCacheCapacity;
	}
	m_cache[key] = result;
	return result;
}
