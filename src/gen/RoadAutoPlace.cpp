#include "GenerationSettings.hpp"
#include "RoadAutoPlace.hpp"
#include "RoadPathfinder.hpp"
#include "RoadDesignLimits.hpp"
#include "../debug/DebugLog.hpp"

namespace
{
	int resolveEndpointNode(RoadNetwork& roads, Vec3 worldPos, float radius)
	{
		const float endpointSnapRadius = radius;
		const float edgeSnapRadius = Min(radius, 15.0f);

		if (const auto nearNode = roads.findNodeNear(worldPos, endpointSnapRadius))
		{
			if (Abs(roads.getNode(*nearNode)->position.y-worldPos.y) <= 6.0) { return *nearNode; }
		}

		if (const auto edgeHit = roads.findEdgeNearDetailed(worldPos, edgeSnapRadius))
		{
			const auto curve = roads.getBezier(edgeHit->first);
			if (curve && Abs(curve->positionAt(edgeHit->second).y-worldPos.y) > 6.0) { return roads.addNode(worldPos); }
			if (const int nodeId = roads.splitEdgeAt(edgeHit->first, edgeHit->second);
			    nodeId >= 0)
			{
				return nodeId;
			}
		}

		return roads.addNode(worldPos);
	}
}

namespace RoadAutoPlace
{

Array<Vec3> findWaypoints(const World& world,Vec3 startWorld,Vec3 goalWorld,RoadType type)
{
	const auto result=RoadAlignment::find(world,startWorld,goalWorld,type);
	if(!result) { return {}; }
	Array<Vec3> points{result->curves.front().p0};
	for(const auto& curve:result->curves) { points << curve.p3; }
	return points;
}

Array<int> buildAlignment(RoadNetwork& roads,const World& world,Array<CubicBezier> curves,
	const RoadEdge& roadTemplate,float connectionRadius)
{
	if(curves.isEmpty()) { return {}; }
	const Vec3 start=curves.front().p0,goal=curves.back().p3;
	const float radius=Min(connectionRadius,static_cast<float>(start.distanceFrom(goal))*.25f);
	const int startNode=resolveEndpointNode(roads,start,radius),goalNode=resolveEndpointNode(roads,goal,radius);
	// 接続先の高さは固定する。探索で選んだ接線・曲率を再フィットで捨てない。
	const Vec3 startShift=roads.getNode(startNode)->position-start,goalShift=roads.getNode(goalNode)->position-goal;
	curves.front()=CubicBezier{start+startShift,curves.front().p1+startShift,curves.front().p2,curves.front().p3};
	curves.back()=CubicBezier{curves.back().p0,curves.back().p1,curves.back().p2+goalShift,goal+goalShift};
	for(const auto& curve:curves)
	{
		if(!RoadAlignment::respectsLimits(curve,roadTemplate.roadType)) { return {}; }
	}
	Array<int> ids;int previous=startNode;
	for(size_t i=0;i<curves.size();++i)
	{
		const auto& curve=curves[i];const int next=i+1==curves.size() ? goalNode : roads.addNode(curve.p3);
		const auto id=roads.addEdge(previous,next,curve.p1,curve.p2,roadTemplate.roadType,Max(1,static_cast<int>(roadTemplate.lanes.size())));
		if(!id) { for(int created:ids) { roads.removeEdge(created); }return {}; }
		roads.applyEdgeTemplate(*id,roadTemplate);auto* edge=roads.getEdge(*id);
		edge->edgeState=EdgeState::Planned;edge->designGrade=true;edge->length=curve.totalLength;
		roads.updateEdgeElevation(*id,world);roads.recomputeAutoSignsForEdge(*id);ids << *id;previous=next;
	}
	for(int id:ids) { const auto* edge=roads.getEdge(id);roads.rebuildNodeConnectivity(edge->nodeA,edge->nodeB); }
	return ids;
}

Array<int> buildWaypoints(RoadNetwork& roads,const World& world,Array<Vec3> points,
	const RoadEdge& roadTemplate,float connectionRadius)
{
	return buildAlignment(roads,world,RoadAlignment::fit(points),roadTemplate,connectionRadius);
}

Array<int> buildPlanned(
	RoadNetwork& roads,
	const World& world,
	Vec3 startWorld, Vec3 goalWorld,
	const Array<int>& routeIds,
	const RoadEdge& templateEdge, bool followTerrain, float connectionRadius)
{
	Array<CubicBezier> curves;
	if(followTerrain)
	{
		const auto result=RoadAlignment::find(world,startWorld,goalWorld,templateEdge.roadType);
		if(!result) { return {}; }curves=result->curves;
	}
	else { curves=RoadAlignment::fit({startWorld,goalWorld}); }
	Array<int> edgeIds=buildAlignment(roads,world,std::move(curves),templateEdge,connectionRadius);
	if (!edgeIds.isEmpty()) { roads.resolveIntersections(*std::min_element(edgeIds.begin(),edgeIds.end()),&edgeIds); }

	// 指定路線があれば新設エッジをまとめて所属させ、逆引きも更新する。
	if (!routeIds.isEmpty() && !edgeIds.isEmpty())
	{
		for (const int rid : routeIds)
		{
			RoadRoute* route = roads.getRoute(rid);
			if (!route)
			{
				continue;
			}
			for (const int eid : edgeIds)
			{
				route->edgeIds << eid;
			}
		}
		roads.rebuildEdgeRouteIndex();
	}

	return edgeIds;
}

Array<int> buildPreviewPlan(
	RoadNetwork& roads,
	const World& world,
	const Array<Vec3>& anchorPoints,
	const RoadEdge& templateEdge, bool followTerrain, float connectionRadius)
{
	if (anchorPoints.size() < 2) return {};

	Array<int> allEdgeIds;
	for (size_t i = 1; i < anchorPoints.size(); ++i)
	{
		const Array<int> segmentEdges = buildPlanned(
			roads,
			world,
			anchorPoints[i - 1],
			anchorPoints[i],
			{},
			templateEdge, followTerrain, connectionRadius);
		if (segmentEdges.isEmpty())
		{
			for (const int eid : allEdgeIds)
				roads.removeEdge(eid);
			return {};
		}
		allEdgeIds.append(segmentEdges);
	}
	return allEdgeIds;
}

} // namespace RoadAutoPlace
