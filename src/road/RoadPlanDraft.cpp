#include "RoadPlanDraft.hpp"
#include "../gen/RoadAutoPlace.hpp"

bool RoadPlanDraft::place(Vec3 point, bool replaceEnd)
{
	const bool replacing = replaceEnd && m_points.size() >= 2;
	if (!m_points.isEmpty())
	{
		const Vec3 origin = m_points[replacing ? m_points.size() - 2 : m_points.size() - 1];
		if (Vec2{point.x - origin.x, point.z - origin.z}.length() < kMinimumSegment)
		{
			return false;
		}
	}
	constexpr size_t kHistoryLimit = 128;
	if (m_undo.size() >= kHistoryLimit) { m_undo.erase(m_undo.begin()); }
	m_undo << m_points;
	m_redo.clear();
	if (replacing) { m_points.back() = point; }
	else { m_points << point; }
	return true;
}

bool RoadPlanDraft::undo()
{
	if (m_undo.isEmpty()) { return false; }
	m_redo << m_points;
	m_points = std::move(m_undo.back());
	m_undo.pop_back();
	return true;
}

bool RoadPlanDraft::redo()
{
	if (m_redo.isEmpty()) { return false; }
	m_undo << m_points;
	m_points = std::move(m_redo.back());
	m_redo.pop_back();
	return true;
}

void RoadPlanDraft::clear()
{
	m_points.clear();
	m_undo.clear();
	m_redo.clear();
	m_preview = RoadNetwork{};
	m_previewEdges.clear();
}

bool RoadPlanDraft::rebuild(const World& world, const RoadEdge& roadTemplate, bool followTerrain)
{
	m_preview = RoadNetwork{};
	m_previewEdges = RoadAutoPlace::buildPreviewPlan(m_preview, world, m_points, roadTemplate, followTerrain, 0.25f);
	return valid();
}

Array<int> RoadPlanDraft::apply(RoadNetwork& network, const World& world, const RoadEdge& roadTemplate, bool followTerrain) const
{
	if (!valid()) { return {}; }
	// スナップ先の道路分割も含めてトランザクションにし、失敗・破棄で既存の街を壊さない。
	RoadNetwork proposed = network;
	Array<int> edges = RoadAutoPlace::buildPreviewPlan(proposed, world, m_points, roadTemplate, followTerrain, 0.5f);
	if (edges.isEmpty()) { return {}; }
	network = std::move(proposed);
	return edges;
}

double RoadPlanDraft::length() const
{
	double result = 0;
	for (const int id : m_previewEdges)
	{
		if (const auto* edge = m_preview.getEdge(id)) { result += edge->length; }
	}
	return result;
}

double RoadPlanDraft::constructionEquivalentLength() const
{
	double result=0;
	for (const int id : m_previewEdges) { if (const auto* edge=m_preview.getEdge(id)) { result+=edge->length*(edge->tunnel ? 6 : edge->useElevation ? 3 : 1); } }
	return result;
}

Vec3 RoadPlanDraft::constrainAngle(Vec3 origin, Vec3 cursor)
{
	const Vec2 delta{ cursor.x - origin.x, cursor.z - origin.z };
	constexpr double kAngleStep = Math::Pi / 4.0;
	const double angle = Round(Atan2(delta.y, delta.x) / kAngleStep) * kAngleStep;
	return Vec3{origin.x + Cos(angle) * delta.length(), cursor.y, origin.z + Sin(angle) * delta.length()};
}

void RoadPlanSnapIndex::rebuild(const RoadNetwork& network)
{
	m_nodes.clear();
	m_edges.clear();
	const auto cell = [](double value) { return static_cast<int>(Floor(value / kCellSize)); };
	for (const auto& node : network.nodes())
	{
		if (node.id >= 0) { m_nodes[Point{cell(node.position.x),cell(node.position.z)}] << node.id; }
	}
	for (const auto& edge : network.edges())
	{
		if (edge.id < 0) { continue; }
		const auto* a = network.getNode(edge.nodeA);
		const auto* b = network.getNode(edge.nodeB);
		if (!a || !b) { continue; }
		double minX = a->position.x, maxX = minX, minZ = a->position.z, maxZ = minZ;
		for (const Vec3 p : {b->position,edge.ctrlA,edge.ctrlB})
		{
			minX = Min(minX,p.x); maxX = Max(maxX,p.x);
			minZ = Min(minZ,p.z); maxZ = Max(maxZ,p.z);
		}
		for (int z = cell(minZ); z <= cell(maxZ); ++z)
		{
			for (int x = cell(minX); x <= cell(maxX); ++x) { m_edges[Point{x,z}] << edge.id; }
		}
	}
}

RoadPlanSnapIndex::Hit RoadPlanSnapIndex::find(const RoadNetwork& network, Vec3 cursor, double radius) const
{
	Hit result{ cursor };
	double bestDistance = radius * radius;
	Array<int> nearbyEdges;
	const auto cell = [](double value) { return static_cast<int>(Floor(value / kCellSize)); };
	for (int z = cell(cursor.z-radius); z <= cell(cursor.z+radius); ++z)
	{
		for (int x = cell(cursor.x-radius); x <= cell(cursor.x+radius); ++x)
		{
			if (const auto it = m_nodes.find(Point{x,z}); it != m_nodes.end())
			{
				for (const int id : it->second)
				{
					const auto* node = network.getNode(id);
					if (!node || Abs(node->position.y-cursor.y) > 6.0) { continue; }
					const double distance = Vec2{node->position.x-cursor.x,node->position.z-cursor.z}.lengthSq();
					if (distance <= bestDistance) { bestDistance = distance; result = Hit{node->position,true,true}; }
				}
			}
			if (const auto it = m_edges.find(Point{x,z}); it != m_edges.end()) { nearbyEdges.append(it->second); }
		}
	}
	if (result.connected) { return result; }
	nearbyEdges.sort();
	nearbyEdges.erase(std::unique(nearbyEdges.begin(), nearbyEdges.end()), nearbyEdges.end());
	for (const int id : nearbyEdges)
	{
		const auto curve = network.getBezier(id);
		if (!curve) { continue; }
		constexpr int kSamples = 32;
		const auto distanceAt = [&](double t)
		{
			const Vec3 p = curve->evaluate(static_cast<float>(t));
			return Vec2{p.x-cursor.x,p.z-cursor.z}.lengthSq();
		};
		double nearest = 0, distance = distanceAt(0);
		for (int i = 1; i <= kSamples; ++i)
		{
			const double t = static_cast<double>(i)/kSamples;
			if (const double value = distanceAt(t); value < distance) { distance = value; nearest = t; }
		}
		double low = Max(0.0,nearest-1.0/kSamples), high = Min(1.0,nearest+1.0/kSamples);
		for (int i = 0; i < 18; ++i)
		{
			const double first = low+(high-low)/3, second = high-(high-low)/3;
			if (distanceAt(first) < distanceAt(second)) { high = second; } else { low = first; }
		}
		double t = (low+high)*0.5;
		if (t < 0.012) { t = 0; } else if (t > 0.988) { t = 1; }
		const Vec3 p = curve->evaluate(static_cast<float>(t));
		if (distanceAt(t) < bestDistance && Abs(p.y-cursor.y) <= 6.0)
		{
			bestDistance = distanceAt(t);
			result = Hit{p,true,t == 0 || t == 1};
		}
	}
	return result;
}

RoadEdge RoadPlanDraft::makeRoadTemplate(int preset)
{
	RoadEdge road;
	road.roadType = (preset == 1 || preset == 2) ? RoadType::Arterial : RoadType::LocalRoad;
	road.speedLimit = preset == 2 ? 50.0f : (preset == 1 ? 40.0f : 30.0f);
	road.lanes = RoadNetwork::buildDefaultLanes(preset == 2 ? 4 : (preset == 3 ? 1 : 2), road.roadType);
	RoadNetwork::buildDefaultParts(road);
	if (preset == 0)
	{
		for (auto& lane : road.lanes) { lane.lineLeft = LineType::None; lane.lineRight = LineType::None; }
	}
	return road;
}
