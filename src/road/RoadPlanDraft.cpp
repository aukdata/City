#include "RoadPlanDraft.hpp"
#include "TransportCrossSection.hpp"
#include "../gen/RoadAutoPlace.hpp"
#include "../gen/RoadAlignment.hpp"
#include "../debug/DebugLog.hpp"

bool RoadPlanDraft::place(Vec3 point, bool replaceEnd)
{
	const bool replacing = replaceEnd && m_state.points.size() >= 2;
	if (!m_state.points.isEmpty())
	{
		const Vec3 origin = m_state.points[replacing ? m_state.points.size() - 2 : m_state.points.size() - 1];
		if (Vec2{point.x - origin.x, point.z - origin.z}.length() < kMinimumSegment)
		{
			return false;
		}
	}
	remember();
	m_state.generated = false;
	m_state.curves.clear();
	if (replacing) { m_state.points.back() = point; }
	else { m_state.points << point; }
	return true;
}

void RoadPlanDraft::invalidate()
{
	m_preview = RoadNetwork{};
	m_previewEdges.clear();
}

void RoadPlanDraft::remember()
{
	constexpr size_t kHistoryLimit = 128;
	if (m_undo.size() >= kHistoryLimit) { m_undo.erase(m_undo.begin()); }
	m_undo << m_state;
	m_redo.clear();
	invalidate();
}

bool RoadPlanDraft::revise(Array<Vec3> points)
{
	if (points.size() < 2 || points == m_state.points) { return false; }
	for (size_t index = 1; index < points.size(); ++index)
	{
		const Vec3 delta = points[index]-points[index-1];
		if (!IsFinite(delta.x) || !IsFinite(delta.y) || !IsFinite(delta.z)
			|| Vec2{delta.x, delta.z}.length() < kMinimumSegment) { return false; }
	}
	Array<CubicBezier> curves;
	if (points.size() == m_state.points.size() && m_state.curves.size() + 1 == points.size())
	{
		// ドラッグした節点の両側で接線を保つ。既存の滑らかな経路を粗い点列から再推定しない。
		curves = m_state.curves;
		for (size_t index = 0; index < curves.size(); ++index)
		{
			const Vec3 startShift = points[index] - m_state.points[index];
			const Vec3 endShift = points[index + 1] - m_state.points[index + 1];
			const auto& curve = curves[index];
			curves[index] = CubicBezier{curve.p0 + startShift,curve.p1 + startShift,
				curve.p2 + endShift,curve.p3 + endShift};
		}
	}
	remember();
	m_state.points = std::move(points);
	m_state.curves = std::move(curves);
	return true;
}

bool RoadPlanDraft::generate(const World& world, const RoadEdge& roadTemplate)
{
	if (m_state.points.size() < 2) { return false; }
	const Stopwatch timer{StartImmediately::Yes};
	const auto result=RoadAlignment::find(world,m_state.points.front(),m_state.points.back(),roadTemplate.roadType,60000,roadTemplate.hasRailLanes() ? TransportMode::Rail : TransportMode::Road);
	if(!result)
	{
		invalidate();DBG_LOG(U"[RoadPlan] no alignment start={} end={} type={} ms={:.1f}"_fmt(m_state.points.front(),m_state.points.back(),static_cast<int>(roadTemplate.roadType),timer.msF()));return false;
	}
	Array<Vec3> points{result->curves.front().p0};for(const auto& curve:result->curves) { points << curve.p3; }
	remember();m_state=State{std::move(points),true,result->curves};
	DBG_LOG(U"[RoadPlan] alignment start={} end={} curves={} expanded={} cost={:.1f} ms={:.1f}"_fmt(m_state.points.front(),m_state.points.back(),m_state.curves.size(),result->expanded,result->cost,timer.msF()));
	return rebuild(world, roadTemplate);
}

bool RoadPlanDraft::undo()
{
	if (m_undo.isEmpty()) { return false; }
	m_redo << m_state;
	m_state = std::move(m_undo.back());
	m_undo.pop_back();
	invalidate();
	return true;
}

bool RoadPlanDraft::redo()
{
	if (m_redo.isEmpty()) { return false; }
	m_undo << m_state;
	m_state = std::move(m_redo.back());
	m_redo.pop_back();
	invalidate();
	return true;
}

void RoadPlanDraft::clear()
{
	m_state = State{};
	m_undo.clear();
	m_redo.clear();
	invalidate();
}

bool RoadPlanDraft::rebuild(const World& world, const RoadEdge& roadTemplate)
{
	m_preview = RoadNetwork{};
	m_previewEdges = RoadAutoPlace::buildAlignment(m_preview,world,m_state.curves.isEmpty() ? RoadAlignment::fit(m_state.points) : m_state.curves,roadTemplate,.25f);
	return valid();
}

Optional<RoadPlanDraft::Proposal> RoadPlanDraft::propose(
	const RoadNetwork& network, const World& world, const RoadEdge& roadTemplate) const
{
	if (!valid()) { return none; }
	// 呼び出し側の追加検査まで同じ仮の道路網を使い、二重コピーを避ける。
	Proposal proposal{ network, {} };
	proposal.edgeIds = RoadAutoPlace::buildAlignment(proposal.network,world,m_state.curves.isEmpty() ? RoadAlignment::fit(m_state.points) : m_state.curves,roadTemplate,.5f);
	if (proposal.edgeIds.isEmpty()) { return none; }
	proposal.network.resolveIntersections(*std::min_element(proposal.edgeIds.begin(),proposal.edgeIds.end()),&proposal.edgeIds);
	return proposal;
}

Array<int> RoadPlanDraft::apply(RoadNetwork& network, const World& world, const RoadEdge& roadTemplate) const
{
	auto proposal = propose(network, world, roadTemplate);
	if (!proposal) { return {}; }
	network = std::move(proposal->network);
	return std::move(proposal->edgeIds);
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
	for (const int id : m_previewEdges) { if (const auto* edge=m_preview.getEdge(id)) { result += edge->constructionEquivalentLength(); } }
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
	for(const auto& edge:network.edges()) { if(edge.id>=0) { appendEdge(network,edge.id); } }
}

void RoadPlanSnapIndex::appendEdge(const RoadNetwork& network,int edgeId)
{
	const auto* found=network.getEdge(edgeId);if(!found) { return; }const auto& edge=*found;
	const auto cell=[](double value){return static_cast<int>(Floor(value/kCellSize));};
		if (edge.id < 0) { return; }
		const auto* a = network.getNode(edge.nodeA);
		const auto* b = network.getNode(edge.nodeB);
		if (!a || !b) { return; }
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

RoadPlanSnapIndex::Hit RoadPlanSnapIndex::find(const RoadNetwork& network, Vec3 cursor, double radius,
	double heightTolerance, bool preferNodes, const std::function<bool(const RoadEdge&)>& accept) const
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
					if (!preferNodes || !node || Abs(node->position.y-cursor.y) > heightTolerance) { continue; }
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
		const auto* edge = network.getEdge(id);
		if (!edge || (accept && !accept(*edge)))
		{
			continue;
		}
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
		if (distanceAt(t) < bestDistance && Abs(p.y-cursor.y) <= heightTolerance)
		{
			bestDistance = distanceAt(t);
			result = Hit{p,true,t == 0 || t == 1,id,static_cast<float>(t)};
		}
	}
	return result;
}

RoadEdge RoadPlanDraft::makeRoadTemplate(int preset)
{
	RoadEdge road;
	if (InRange(preset,4,6))
	{
		TransportCrossSection::railway(road,true,preset==5,preset==6);
		road.speedLimit = preset==6 ? 40.0f : 80.0f; return road;
	}
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
