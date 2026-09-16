#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/traffic/LaneVehicleIndex.hpp"
#include "src/traffic/BuildingAccess.hpp"
#include "src/traffic/VehicleManager.hpp"
#include "src/gen/StreetProfile.hpp"
#include "src/asset/AssetRegistrar.hpp"
#include <chrono>

namespace
{
	struct AppDirectory
	{
		FilePath previous = FileSystem::CurrentDirectory();
		AppDirectory() { FileSystem::ChangeCurrentDirectory(previous + U"../../App/"); RegisterAssets(); }
		~AppDirectory() { FileSystem::ChangeCurrentDirectory(previous); }
	};
	/// @brief Resolve the same curb-lane destination as the production simulation thread.
	void resolveTrips(VehicleManager& manager, const SimGraph& graph, const TrafficGraph& routes)
	{
		for (const auto& message : manager.collectRequests())
		{
			const auto* request = std::get_if<RouteRequest>(&message);
			if (!request) { continue; }
			const auto path = routes.dijkstra(routes.entryNodeId(request->startEdge, request->startLane), request->goalEdge, request->goalLane);
			RouteResponse response; response.vehicleId = request->vehicleId; response.found = path.found;
			int previous = request->startEdge;
			for (const int id : path.nodeIds)
			{
				const auto* node = routes.getLaneNode(id);
				if (!node || node->edgeId == previous) { continue; }
				const auto* edge = graph.getEdge(node->edgeId);
				response.waypoints << RouteWaypoint{edge->id, node->laneIndex, node->arcPos, edge->length, edge->length / 8};
				previous = edge->id;
			}
			manager.applyRouteResponse(response);
		}
	}
}

void registerTrafficScalingTests(TestRunner& runner)
{
	runner.add(U"TrafficScaling.IndexAndCost", [](TestContext& context)
	{
		JSON report;
		for (const int count : {600, 6000, 12000})
		{
			Array<Vehicle> vehicles;
			for (int i = 0; i < count; ++i)
			{
				Vehicle car; car.id = i; car.currentEdge = i / 24; car.currentLane = i % 2;
				car.arcPos = static_cast<float>((i % 24) * 12); car.speed = 8;
				car.mode = i % 3 == 0 ? VehicleMode::Dormant : VehicleMode::Active;
				vehicles << car;
			}
			LaneVehicleIndex index;
			Stopwatch timer{StartImmediately::Yes};
			double checksum = 0;
			constexpr int kRepeats = 12;
			for (int repeat = 0; repeat < kRepeats; ++repeat)
			{
				index.rebuild(vehicles);
				for (const auto& car : vehicles)
				{
					float front, rear;
					index.measureGaps(car.id, car.currentEdge, car.currentLane, car.arcPos, true, front, rear);
					checksum += Min(front, 1000.0f) + Min(rear, 1000.0f);
				}
			}
			const double indexedMs = timer.msF() / kRepeats;
			timer.restart();
			for (const auto& car : vehicles)
			{
				float referenceFront, referenceRear, front, rear;
				TrafficCommon::measureGaps(vehicles, car.id, car.currentEdge, car.currentLane, car.arcPos, true, false, referenceFront, referenceRear);
				index.measureGaps(car.id, car.currentEdge, car.currentLane, car.arcPos, true, front, rear);
				context.expect(front == referenceFront && rear == referenceRear, U"Sorted queries match exhaustive neighbor searches, including invisible cars");
			}
			const double exhaustiveMs = timer.msF();
			report[Format(count)][U"indexedMs"] = indexedMs;
			report[Format(count)][U"exhaustiveMs"] = exhaustiveMs;
			report[Format(count)][U"checksum"] = checksum;
			if (count == 12000) { context.expect(indexedMs < exhaustiveMs * .4, U"Twelve thousand cars do not require quadratic neighbor scans"); }
		}
		Vehicle changing; changing.id = 7; changing.currentEdge = 1; changing.currentLane = 0;
		changing.laneTo = 1; changing.laneFrom = 0; changing.location = VehicleLocation::ChangingLane; changing.arcPos = 30;
		LaneVehicleIndex index; index.rebuild({changing});
		context.expect(index.neighbors(1, 1, 10, true, 8).front.has_value(), U"Lane change reserves the receiving lane");
		context.expect(!index.hasSpace(1, 1, 32, VehicleType::PassengerCar), U"Spawn cannot overlap a merging car");
		report.save(U"TestResults/traffic_scaling.json");
	});

	runner.add(U"TrafficScaling.BuildingTripsAndInvisibleQueues", [](TestContext& context)
	{
		AppDirectory directory;
		World world; world.reserveChunks();
		world.installChunkDirect({0, 0}, HeightMapResult{Grid<float>(65, 65, 20), 20, 20});
		RoadNetwork roads; Array<int> nodes;
		for (const Vec3 point : {Vec3{120,20,120}, Vec3{880,20,120}, Vec3{880,20,880}, Vec3{120,20,880}}) { nodes << roads.addNode(point); }
		for (int i = 0; i < 4; ++i)
		{
			const Vec3 a = roads.getNode(nodes[i])->position, b = roads.getNode(nodes[(i + 1) % 4])->position;
			const int id = *roads.addEdge(nodes[i], nodes[(i + 1) % 4], a.lerp(b, 1.0 / 3), a.lerp(b, 2.0 / 3), RoadType::LocalRoad, 2);
			auto* road = roads.getEdge(id); GeneratedStreet::apply(*road, GeneratedStreet::describe(GeneratedStreet::Role::Local)); road->edgeState = EdgeState::Open;
			for (int j = 0; j < 12; ++j)
			{
				const float fraction = (j + 1.0f) / 13;
				const Vec3 point = a.lerp(b, fraction) + tangentToRight(b - a) * 18;
				const Point cell{static_cast<int>(point.x / 16), static_cast<int>(point.z / 16)};
				auto& building = world.getChunk({0,0})->buildingGrid[cell]; building.type = BuildingType::Detached;
				building.edgeId = id; building.edgeT = fraction;
				building.offsetX = static_cast<float>(point.x - (cell.x + .5) * 16);
				building.offsetZ = static_cast<float>(point.z - (cell.y + .5) * 16);
			}
		}
		for (const int node : nodes) { roads.updateNodeCutoffs(node); roads.rebuildLaneConnections(node); }
		const auto graph = SimGraph::build(roads);
		TrafficGraph routes; routes.rebuild(graph, 0, {});
		VehicleManager manager; manager.init(graph, roads, &world);
		TrafficDemand demand; demand.targetVehicleCount = 28; manager.setTrafficDemand(demand);
		context.expectEqual(manager.buildingAccess().size(), size_t{48}, U"Every connected building has a curb-side access point");
		bool sawSpawn = false;
		for (int frame = 0; frame < 5000; ++frame)
		{
			manager.update(.05, frame * .05, graph, roads, {});
			resolveTrips(manager, graph, routes);
			for (const auto& car : manager.vehicles())
			{
				sawSpawn = true;
				context.expect(car.originBuilding >= 0 && car.destinationBuilding >= 0 && car.goalArc >= 0,
					U"Every normal trip retains real building endpoints while invisible");
				context.expect(car.originBuilding != car.destinationBuilding, U"Origin and destination buildings differ");
			}
		}
		context.expect(sawSpawn && manager.populationStats().completed > 3, U"Trips finish at building frontages and replenish outside the camera");
		context.expectEqual(manager.populationStats().routeFailures, 0, U"Building curb lanes are reachable without failed routing loops");
		JSON report; report[U"completed"] = manager.populationStats().completed; report[U"spawned"] = manager.populationStats().spawned;
		report[U"accessPoints"] = manager.buildingAccess().size(); report.save(directory.previous + U"TestResults/building_trips.json");
	});
	runner.add(U"TrafficScaling.TwelveThousandStoppedQueues", [](TestContext& context)
	{
		AppDirectory directory;
		RoadNetwork roads; Array<int> edges;
		for (int road = 0; road < 60; ++road)
		{
			const Vec3 a{100,20,100.0 + road*100}, b{2100,20,100.0 + road*100};
			const int id = *roads.addEdge(roads.addNode(a), roads.addNode(b), a.lerp(b,1.0/3), a.lerp(b,2.0/3), RoadType::LocalRoad, 1);
			auto* edge = roads.getEdge(id); edge->edgeState = EdgeState::Open; edge->lanes[0].dir = LaneDir::Forward;
			edges << id;
		}
		const auto graph = SimGraph::build(roads); VehicleManager manager; manager.init(graph, roads);
		for (const int edge : edges) { for (int index = 0; index < 200; ++index) { manager.spawnOnEdge(edge, graph, VehicleType::PassengerCar, edge); } }
		// Test fixture owns this non-const manager; control exact positions without a production debug API.
		auto& cars = const_cast<Array<Vehicle>&>(manager.vehicles());
		for (size_t i = 0; i < cars.size(); ++i)
		{
			auto& car = cars[i]; car.arcPos = 50 + static_cast<float>(i%200)*(static_cast<float>(TrafficSpawn::vehicleLength(car.type))+2.1f); car.speed = 0;
			car.routeRequested = true;
			if (i%200 == 199) { car.state = VehicleState::WaitingBusStop; car.busWaitRemaining = 3600; }
		}
		Array<double> times;
		for (int frame = 0; frame < 1800; ++frame)
		{
			const Stopwatch timer{StartImmediately::Yes}; manager.update(.05, frame*.05, graph, roads, {});
			if (frame > 30) { times << timer.msF(); }
		}
		context.expectEqual(manager.vehicleCount(), 12000, U"Invisible queues do not despawn or teleport");
		double minimumGap = Math::Inf; int stopped = 0;
		for (size_t i = 0; i < cars.size(); ++i)
		{
			stopped += cars[i].speed < .5;
			if (i%200 != 199) { minimumGap = Min(minimumGap, static_cast<double>(cars[i+1].arcPos - cars[i].arcPos) - TrafficSpawn::vehicleLength(cars[i].type)); }
			context.expect(cars[i].mode == VehicleMode::Dormant, U"The queue is entirely outside the visible set");
		}
		context.expect(minimumGap >= .1, U"All stopped queues retain bumper clearance");
		context.expect(stopped > 10000, U"A stationary lead vehicle propagates congestion upstream");
		times.sort(); JSON report; report[U"cars"] = manager.vehicleCount(); report[U"medianUpdateMs"] = times[times.size()/2];
		report[U"minimumBumperGap"] = minimumGap; report[U"stopped"] = stopped;
		report.save(directory.previous + U"TestResults/traffic_full_simulation.json");
	});

}
