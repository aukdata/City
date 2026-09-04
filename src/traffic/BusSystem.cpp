#include "BusSystem.hpp"

int BusSystem::addStop(Vec3 position, String name, const RoadNetwork& network,
	float maximumSnapDistance)
{
	int bestEdgeId = -1;
	float bestArcPos = 0.0f;
	double bestDistance = maximumSnapDistance;
	Vec3 bestPosition = position;

	for (const auto& edge : network.edges())
	{
		if (edge.id < 0 || !edge.isRoadbedBuilt()) continue;
		const auto bezier = network.getBezier(edge.id);
		if (!bezier || bezier->totalLength <= 0.0f) continue;
		const int samples = Max(4, static_cast<int>(Ceil(bezier->totalLength / 20.0f)));
		for (int i = 0; i <= samples; ++i)
		{
			const float arcPos = bezier->totalLength * static_cast<float>(i) / samples;
			const Vec3 candidate = bezier->positionAt(arcPos);
			const double distance = Vec2{ candidate.x, candidate.z }
				.distanceFrom(Vec2{ position.x, position.z });
			if (distance >= bestDistance) continue;
			bestDistance = distance;
			bestEdgeId = edge.id;
			bestArcPos = arcPos;
			bestPosition = candidate;
		}
	}

	if (bestEdgeId < 0) return -1;
	BusStop stop;
	stop.id = m_nextStopId++;
	stop.position = bestPosition;
	stop.edgeId = bestEdgeId;
	stop.arcPos = bestArcPos;
	stop.name = name.isEmpty() ? U"バス停 {}"_fmt(stop.id + 1) : std::move(name);
	m_stops << std::move(stop);
	return m_stops.back().id;
}

int BusSystem::addRoute(const Array<int>& stopIds, float headwaySec)
{
	if (stopIds.size() < 2) return -1;
	for (const int stopId : stopIds)
	{
		if (!getStop(stopId)) return -1;
	}
	BusRoute route;
	route.id = m_nextRouteId++;
	route.stopIds = stopIds;
	route.headwaySec = Max(1.0f, headwaySec);
	route.lastSpawnAt = -route.headwaySec;
	m_routes << std::move(route);
	return m_routes.back().id;
}

void BusSystem::update(GameTime gameNow, const SimGraph& simGraph, VehicleManager& vehicles)
{
	for (auto& route : m_routes)
	{
		if (route.stopIds.size() < 2) continue;
		if ((gameNow - route.lastSpawnAt) < route.headwaySec) continue;
		Array<int> stopEdgeIds;
		stopEdgeIds.reserve(route.stopIds.size());
		for (const int stopId : route.stopIds)
		{
			const BusStop* stop = getStop(stopId);
			if (!stop || stop->edgeId < 0)
			{
				stopEdgeIds.clear();
				break;
			}
			stopEdgeIds << stop->edgeId;
		}
		if (stopEdgeIds.size() < 2) continue;
		if (vehicles.spawnBus(stopEdgeIds, route.id, simGraph) >= 0)
		{
			route.lastSpawnAt = gameNow;
		}
	}
}

const BusStop* BusSystem::getStop(int id) const
{
	for (const auto& stop : m_stops)
	{
		if (stop.id == id) return &stop;
	}
	return nullptr;
}
