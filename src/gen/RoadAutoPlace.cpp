#include "RoadAutoPlace.hpp"
#include "RoadPathfinder.hpp"

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

Array<int> buildPlanned(
	RoadNetwork& roads,
	const World& world,
	Vec3 startWorld, Vec3 goalWorld,
	const Array<int>& routeIds,
	const RoadEdge& templateEdge, bool followTerrain, float connectionRadius)
{
	// 始点終点を囲う範囲だけで簡易経路探索を行い、その結果を Planned 状態の道路列へ変換する。
	const float sx = static_cast<float>(startWorld.x);
	const float sz = static_cast<float>(startWorld.z);
	const float ex = static_cast<float>(goalWorld.x);
	const float ez = static_cast<float>(goalWorld.z);

	const float dist = std::sqrt((ex - sx) * (ex - sx) + (ez - sz) * (ez - sz));

	// セルサイズは MapGenerator::buildRoadSegment と同じスケーリング
	const float cellSize = (dist > 5000.0f) ? 120.0f
	                     : (dist > 2000.0f) ? 80.0f
	                     : RoadPathfinder::kDefaultCellSize;

	// バウンディング + 対角の 30% 余白
	const float margin = dist * 0.30f + cellSize * 2.0f;
	const float minX = Min(sx, ex) - margin;
	const float minZ = Min(sz, ez) - margin;
	const float maxX = Max(sx, ex) + margin;
	const float maxZ = Max(sz, ez) + margin;

	const int pfW = Max(2, static_cast<int>(Ceil((maxX - minX) / cellSize)));
	const int pfH = Max(2, static_cast<int>(Ceil((maxZ - minZ) / cellSize)));

	RoadPathfinder pf;
	Array<Vec3> wps;
	if (!followTerrain || dist < RoadPathfinder::kDefaultCellSize)
	{
		if (dist < 2.0f) { return {}; }
		wps = { startWorld, goalWorld };
	}
	else
	{
		pf.setup(world, Vec2{ minX, minZ }, pfW, pfH, cellSize);

		const Point gs = pf.worldToGrid(sx, sz);
		const Point ge = pf.worldToGrid(ex, ez);

		const Array<Point> path = pf.findPath(gs, ge);

		if (path.isEmpty() || path.size() < 2)
		{
			Console << U"[AutoPlace] 経路探索失敗: 到達不能な地点が選択されました";
			return {};
		}

		const int sampleStep = (cellSize > 60.0f) ? 2 : 3;
		wps = pf.samplePath(path, sampleStep);
	}
	wps.front() = startWorld;
	wps.back()  = goalWorld;

	// スタート・ゴールは既存ノード優先、近傍エッジがあれば分割して接続点を作る。
	const int startNodeId = resolveEndpointNode(roads, startWorld, Min(connectionRadius, dist * 0.25f));
	const int goalNodeId  = resolveEndpointNode(roads, goalWorld, Min(connectionRadius, dist * 0.25f));
	wps.front() = roads.getNode(startNodeId)->position;
	wps.back() = roads.getNode(goalNodeId)->position;

	const int templateLanes = static_cast<int>(templateEdge.lanes.size());
	const int numLanes = (templateLanes > 0) ? templateLanes : 2;

	Array<int> edgeIds;
	pf.pathToRoadEdges(wps, roads, templateEdge.roadType, numLanes, startNodeId, goalNodeId, &edgeIds);

	if (edgeIds.size() + 1 != wps.size())
	{
		for (const int id : edgeIds) { roads.removeEdge(id); }
		return {};
	}

	// 生成済みエッジへテンプレート断面と状態を適用し、編集前の計画道路として揃える。
	for (const int eid : edgeIds)
	{
		if (RoadEdge* edge = roads.getEdge(eid))
		{
			roads.applyEdgeTemplate(eid, templateEdge);
			edge->edgeState = EdgeState::Planned;
			roads.updateEdgeElevation(eid,world);
		}
	}

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
