#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/road/RoadConstructionStart.hpp"
#include "src/gen/SettlementDevelopment.hpp"
#include "src/gen/DistrictRoads.hpp"
#include "src/world/ZoneGrid.hpp"

namespace
{
	int plannedRoad(RoadNetwork& network, Vec3 from, Vec3 to)
	{
		const int a = network.addNode(from), b = network.addNode(to);
		const int id = *network.addEdge(a, b, from.lerp(to, 1.0 / 3), from.lerp(to, 2.0 / 3));
		network.getEdge(id)->edgeState = EdgeState::Planned;
		return id;
	}

	struct DevelopmentSnapshot
	{
		Array<double> values;
		String summary;
		int buildings = 0, fields = 0, tracks = 0;
	};

	DevelopmentSnapshot developTown(TestContext& context, uint8 scale)
	{
		World world;
		world.reserveChunks();
		world.setGenerationParams(42, WORLD_SIZE, WORLD_SIZE);
		for (int z = 31; z <= 33; ++z)
		{
			for (int x = 31; x <= 33; ++x)
			{
				world.installChunkDirect({x, z}, HeightMapResult{Grid<float>(HEIGHT_CELLS + 1, HEIGHT_CELLS + 1, 20), 20, 20});
			}
		}
		world.update({33280, 20, 33280});
		MapGenerator::Settlement town;
		town.center = {33280, 33280};
		town.kind = scale == 1 ? MapGenerator::SettlementKind::LocalTown : MapGenerator::SettlementKind::RuralSettlement;
		town.plan = UrbanMorphology::makePlan(scale == 1 ? UrbanMorphology::Origin::Castle : UrbanMorphology::Origin::Rural,
			scale, UrbanMorphology::Site{}, 42, false);
		RoadNetwork roads;
		// 本番と同じ前提: 地区道路の前に、農村と農道の接続先となる既存幹線がある。
		int previous = -1;
		for (int step = -2; step <= 2; ++step)
		{
			const Vec3 point{town.center.x + step * 640, 20, town.center.y};
			const int node = roads.addNode(point);
			if (previous >= 0)
			{
				const Vec3 from = roads.getNode(previous)->position;
				roads.addEdge(previous, node, from.lerp(point, 1.0 / 3), from.lerp(point, 2.0 / 3), RoadType::Arterial, 2);
			}
			previous = node;
		}
		const auto kaido = DistrictRoads::extractKaido(town, roads, 1000);
		context.expect(kaido.passesThrough && !kaido.edgeIds.isEmpty(), U"The generation fixture supplies the real arterial-road prerequisite");
		DistrictRoads::generateSettlement(42, 0, town, kaido, world, roads);
		const Array<MapGenerator::Settlement> towns{town};
		const TrainNetwork trains;
		SettlementDevelopment development{world, roads, trains, towns, 42};
		development.applyZonesGlobal();
		const auto validation = development.placeInitialBuildings();
		DevelopmentSnapshot snapshot;
		snapshot.summary = validation.summary;
		bool frontageValid = true, parcelsValid = true;
		for (int z = 31; z <= 33; ++z)
		{
			for (int x = 31; x <= 33; ++x)
			{
				const auto* chunk = world.getChunk({x, z});
				for (int row = 0; row < ZONE_CELLS; ++row)
				{
					for (int col = 0; col < ZONE_CELLS; ++col)
					{
						const auto& building = chunk->buildingGrid[{col, row}];
						snapshot.values << static_cast<double>(chunk->zoneMap[{col, row}]) << static_cast<double>(building.type);
						if (building.type == BuildingType::None) { continue; }
						++snapshot.buildings;
						snapshot.values << building.builtAt << building.angle << building.edgeId << building.edgeT << building.offsetX << building.offsetZ;
						frontageValid &= roads.getEdge(building.edgeId) != nullptr;
						Point coord;
						int cellX = -1, cellZ = -1;
						const Vec2 center = ZoneGrid::cellCenterXZ({x, z}, col, row);
						ZoneGrid::worldToZoneCell(static_cast<float>(center.x), static_cast<float>(center.y), coord, cellX, cellZ);
						frontageValid &= coord == Point{x, z} && cellX == col && cellZ == row;
					}
				}
				for (const auto& patch : chunk->landPatches)
				{
					snapshot.values << patch.id << static_cast<double>(patch.type) << patch.elevationOffset
						<< patch.materialVariant << static_cast<double>(patch.sourceParcelKey) << static_cast<double>(patch.polygon.size());
					for (const Vec2 point : patch.polygon) { snapshot.values << point.x << point.y; }
					parcelsValid &= Polygon::Validate(patch.polygon) == PolygonFailureType::OK;
					snapshot.fields += patch.type == LandPatchType::FarmField || patch.type == LandPatchType::PaddyField;
					context.expect(patch.type!=LandPatchType::FarmTrack,U"Access tracks are ordinary roads, not land patches");
				}
			}
		}
		context.expect(snapshot.buildings > 20 && frontageValid, U"Standalone development places homes on real roads and preserves shared cell coordinates");
		for (const auto& edge:roads.edges()) { snapshot.tracks+=edge.id>=0 && edge.farmAccess; }
		context.expect(snapshot.fields > 0 && snapshot.tracks > 0 && parcelsValid, U"The generated countryside has valid fields, access tracks and parcels");
		context.expect(validation.passed, U"The standalone production pipeline passes its frontage and parcel constraints: " + validation.summary);
		return snapshot;
	}
}

void registerRefactoringTests(TestRunner& runner)
{
	runner.add(U"Refactoring.ExistingConstructionAtomicBatch", [](TestContext& context)
	{
		RoadNetwork roads;
		const int first = plannedRoad(roads, {100, 20, 100}, {300, 20, 100});
		const int outsideRoute = plannedRoad(roads, {300, 20, 200}, {600, 20, 200});
		const int standalone = plannedRoad(roads, {100, 20, 300}, {500, 20, 300});
		const int open = plannedRoad(roads, {100, 20, 400}, {500, 20, 400});
		roads.getEdge(open)->edgeState = EdgeState::Open;
		roads.getEdge(outsideRoute)->useElevation = true;
		RoadPlan plan;
		plan.edgeIds = {first, outsideRoute};
		const int planId = roads.addPlan(plan);
		const int routeId = roads.addRoute(RoadRouteKind::Named, U"試験通り", {first, standalone, open});
		const auto selection = roads.getRoute(routeId)->edgeIds;
		const double cost = roads.getPlan(planId)->totalCost + roads.estimatePlanCost(roads.getEdge(standalone)->roadType, roads.getEdge(standalone)->length);
		context.expectNear(RoadConstructionStart::estimateCost(roads, selection), cost, 1e-10,
			U"A route quote includes the entire elevated plan plus its standalone road, excluding open roads");
		context.expect(!RoadConstructionStart::start(roads, selection, cost - .01, 123), U"Insufficient funds reject the whole batch");
		context.expect(roads.getPlan(planId)->state == PlanState::Planning && !roads.getPlan(planId)->constructionStart
			&& roads.getEdge(standalone)->edgeState == EdgeState::Planned && roads.getEdge(first)->constructionStartTime == 0,
			U"Refusal changes neither plan state, standalone state nor start dates");
		const auto receipt = RoadConstructionStart::start(roads, selection, cost, 123);
		context.expect(receipt.has_value(), U"Exact funds allow the complete batch to start");
		if (!receipt) { return; }
		context.expectNear(receipt->cost, cost, 1e-10, U"The charged amount is exactly the displayed quote");
		context.expectEqual(receipt->edgeIds.size(), 3, U"The road outside the selected route is included through its plan once");
		context.expectEqual(receipt->affectedNodeIds.size(), 6, U"All affected endpoints are returned for traffic and rendering updates");
		for (const int id : {first, outsideRoute, standalone})
		{
			context.expect(roads.getEdge(id)->edgeState == EdgeState::UnderConstruction && roads.getEdge(id)->constructionStartTime == 123,
				U"Plan members and standalone roads share the requested start time");
		}
		context.expect(roads.getEdge(open)->edgeState == EdgeState::Open && roads.getPlan(planId)->completionDate == 123 + roads.getPlan(planId)->constructionDuration,
			U"Open roads stay open and construction keeps its original duration");
		context.expect(!RoadConstructionStart::start(roads, selection, cost, 124)
			&& RoadConstructionStart::estimateCost(roads, selection) == 0, U"Repeated clicks cannot start or charge the work again");
	});
	runner.add(U"Refactoring.ConstructionEntrancesAgree", [](TestContext& context)
	{
		RoadNetwork original;
		const int first = plannedRoad(original, {0, 20, 0}, {200, 20, 0});
		const int last = plannedRoad(original, {0, 20, 100}, {400, 20, 100});
		RoadPlan plan;
		plan.edgeIds = {first, last};
		const int planId = original.addPlan(plan);
		const double cost = original.getPlan(planId)->totalCost;
		for (const Array<int>& selection : {Array<int>{first}, Array<int>{first, last}, Array<int>{last, first, last, -1}})
		{
			RoadNetwork roads = original;
			context.expectNear(RoadConstructionStart::estimateCost(roads, selection), cost, 1e-10,
				U"Edge, plan and route selection quote one plan regardless of duplicate IDs");
			const auto receipt = RoadConstructionStart::start(roads, selection, cost, 20);
			context.expect(receipt && receipt->edgeIds == plan.edgeIds && receipt->cost == cost,
				U"All construction entrances yield the same started edges and single charge");
		}
	});
	for (const uint8 scale : {uint8{1}, uint8{2}})
	{
		runner.add(scale == 1 ? U"Refactoring.CastleDevelopmentDeterminism" : U"Refactoring.RuralDevelopmentDeterminism", [scale](TestContext& context)
		{
			const auto first = developTown(context, scale);
			const auto repeated = developTown(context, scale);
			context.expect(first.values == repeated.values && first.summary == repeated.summary,
				U"Fresh worlds with the same seed produce identical zoning, homes, frontage and every parcel vertex");
			JSON report;
			report[U"buildings"] = first.buildings;
			report[U"fields"] = first.fields;
			report[U"tracks"] = first.tracks;
			report[U"validation"] = first.summary;
			report[U"deterministic"] = first.values == repeated.values;
			report.save(U"TestResults/development_{}.json"_fmt(scale));
		});
	}
}
