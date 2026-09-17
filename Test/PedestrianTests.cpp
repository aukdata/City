#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/pedestrian/PedestrianManager.hpp"
#include "src/render/PedestrianRenderer.hpp"
#include "src/gen/StreetProfile.hpp"
#include "src/gen/GenerationSettings.hpp"
#include "src/railway/RailTimetable.hpp"
#include "src/asset/AssetRegistrar.hpp"

namespace
{
struct AppDirectory
{
	FilePath previous = FileSystem::CurrentDirectory();
	AppDirectory()
	{
		FileSystem::ChangeCurrentDirectory(previous + U"../../App/");
		RegisterAssets();
	}
	~AppDirectory() { FileSystem::ChangeCurrentDirectory(previous); }
};
/// @brief 実際の敷地・歩道・車道・駅を備えた街区。移動処理は本体を呼ぶ。
struct Town
{
	World world;
	RoadNetwork roads;
	TrainNetwork railway;
	SimGraph graph;
	TrafficGraph routing;
	VehicleManager cars;
	TrainManager trains;
	PedestrianManager people;
	Array<int> edges;
	double now = 0;
	Town(int population, bool rail = false)
	{
		world.reserveChunks();
		world.installChunkDirect({0, 0}, HeightMapResult{Grid<float>(65, 65, 20), 20, 20});
		Array<int> nodes;
		for (const Vec3 point : {Vec3{120, 20, 120}, Vec3{880, 20, 120}, Vec3{880, 20, 880}, Vec3{120, 20, 880}})
		{
			nodes << roads.addNode(point);
		}
		for (int i = 0; i < 4; ++i)
		{
			const Vec3 a = roads.getNode(nodes[i])->position, b = roads.getNode(nodes[(i + 1) % 4])->position;
			const int id = *roads.addEdge(
				nodes[i], nodes[(i + 1) % 4], a.lerp(b, 1.0 / 3), a.lerp(b, 2.0 / 3), RoadType::LocalRoad, 2);
			edges << id;
			auto* road = roads.getEdge(id);
			GeneratedStreet::apply(*road, GeneratedStreet::describe(GeneratedStreet::Role::Local));
			road->edgeState = EdgeState::Open;
			for (int j = 0; j < 12; ++j)
			{
				const float fraction = (j + 1.f) / 13;
				const Vec3 point = a.lerp(b, fraction) + tangentToRight(b - a) * 18;
				const Point cell{static_cast<int>(point.x / 16), static_cast<int>(point.z / 16)};
				auto& building = world.getChunk({0, 0})->buildingGrid[cell];
				building.type = (j == 2 || j == 9) ? BuildingType::Parking : BuildingType::Detached;
				building.edgeId = id;
				building.edgeT = fraction;
				building.offsetX = static_cast<float>(point.x - (cell.x + .5) * 16);
				building.offsetZ = static_cast<float>(point.z - (cell.y + .5) * 16);
			}
		}
		for (const int node : nodes)
		{
			roads.updateNodeCutoffs(node);
			roads.rebuildLaneConnections(node);
		}
		railway.bind(&roads);
		if (rail)
		{
			const int a = railway.addStation({260, 20, 70}, U"西駅"), b = railway.addStation({740, 20, 70}, U"東駅");
			railway.addEdge(a, b, {420, 20, 70}, {580, 20, 70}, 60, true);
			auto schedule = RailTimetable::makeDefault(railway, a, b);
			schedule.headwaySec = 60;
			railway.addSchedule(schedule);
		}
		graph = SimGraph::build(roads);
		routing.rebuild(graph, 0, {});
		cars.init(graph, roads, &world);
		TrafficDemand demand;
		demand.targetVehicleCount = 0;
		cars.setTrafficDemand(demand);
		trains.init(&railway);
		trains.enablePassengerEvents();
		people.initialize(world, roads, cars.buildingAccess(), railway, population);
	}
	void step(double dt, Vec3 camera = {500, 20, 500}, bool traffic = true)
	{
		now += dt;
		if (traffic)
		{
			cars.update(dt, now, graph, roads, {});
			for (const auto& message : cars.collectRequests())
			{
				const auto* request = std::get_if<RouteRequest>(&message);
				if (!request)
				{
					continue;
				}
				const auto path = routing.dijkstra(
					routing.entryNodeId(request->startEdge, request->startLane), request->goalEdge, request->goalLane);
				RouteResponse response;
				response.vehicleId = request->vehicleId;
				response.found = path.found;
				int previous = request->startEdge;
				for (const int id : path.nodeIds)
				{
					const auto* node = routing.getLaneNode(id);
					if (!node || node->edgeId == previous)
					{
						continue;
					}
					const auto* edge = graph.getEdge(node->edgeId);
					response.waypoints << RouteWaypoint{
						edge->id, node->laneIndex, node->arcPos, edge->length, edge->length / 8};
					previous = edge->id;
				}
				cars.applyRouteResponse(response);
			}
			trains.update(dt, now);
		}
		people.update(dt, camera, world, roads, graph, cars, railway, trains);
	}
	size_t siteNear(Vec3 point) const
	{
		size_t best = 0;
		double distance = Math::Inf;
		for (size_t i = 0; i < people.network().sites().size(); ++i)
		{
			const double candidate = people.network().sites()[i].entrance.distanceFromSq(point);
			if (candidate < distance)
			{
				best = i;
				distance = candidate;
			}
		}
		return best;
	}
	void place(int id, size_t site)
	{
		auto& person = const_cast<Array<Pedestrian>&>(people.people())[id];
		const auto& home = people.network().sites()[site];
		person.origin = home.key;
		person.node = home.node;
		person.position = home.entrance;
		person.state = PedestrianState::Inside;
		person.readyAt = 1e9;
	}
};
} // namespace
void registerPedestrianTests(TestRunner& runner)
{
	runner.add(U"Pedestrian.WalkNetworkAndArrival", [](TestContext& context)
	{
		AppDirectory directory;
		Town town{1};
		context.expectEqual(town.people.network().sites().size(), 48, U"Every connected plot has a walking entrance");
		int crossings = 0;
		for (const auto& link : town.people.network().links())
		{
			crossings += link.crossingNode >= 0;
		}
		context.expect(crossings >= 4, U"Road crossings are distinct from sidewalks and corner links");
		const size_t from = town.siteNear({280, 20, 140}), to = town.siteNear({750, 20, 140});
		town.place(0, from);
		const auto destination = town.people.network().sites()[to].key;
		context.expect(town.people.beginTrip(0, destination, PedestrianTripMode::Walk),
			U"A walking trip starts at a plot entrance");
		bool onFoot = false;
		for (int frame = 0; frame < 1800 && town.people.stats().completed == 0; ++frame)
		{
			town.step(.5);
			onFoot |= town.people.stats().walking > 0;
		}
		context.expect(onFoot && town.people.stats().completed == 1 && town.people.people()[0].origin == destination,
			U"A real walking path reaches the destination plot");
		context.expectNear(town.people.people()[0].position.distanceFrom(town.people.network().sites()[to].entrance), 0,
			.01, U"The last walking segment ends at the entrance");
		const auto revision = town.people.network().revision();
		town.step(16, {}, false);
		context.expectEqual(town.people.network().revision(), revision,
			U"An unchanged access index does not rebuild on a refresh timer");
		const Vec3 position = town.people.people()[0].position;
		const double time = town.people.now();
		town.step(0);
		context.expect(
			town.people.now() == time && town.people.people()[0].position == position, U"Pause freezes residents");
	});
	runner.add(U"Pedestrian.ActualCarTransfer", [](TestContext& context)
	{
		AppDirectory directory;
		Town town{1};
		const size_t from = town.siteNear({260, 20, 140}), to = town.siteNear({860, 20, 700});
		town.place(0, from);
		const auto destination = town.people.network().sites()[to].key;
		town.people.beginTrip(0, destination, PedestrianTripMode::Car);
		bool riding = false, realCar = false;
		for (int frame = 0; frame < 25000 && town.people.stats().completed == 0; ++frame)
		{
			town.step(.1);
			riding |= town.people.stats().ridingCar > 0;
			for (const auto& car : town.cars.vehicles())
			{
				realCar |= car.passengerId == 0 && car.originBuilding >= 0 && car.destinationBuilding >= 0;
			}
		}
		const auto& person = town.people.people()[0];
		JSON report;
		report[U"state"] = static_cast<int>(person.state);
		report[U"boarded"] = town.people.stats().boardedCars;
		report[U"completed"] = town.people.stats().completed;
		report[U"time"] = town.now;
		report[U"cars"] = town.cars.vehicleCount();
		report.save(directory.previous + U"TestResults/pedestrian_car.json");
		context.expect(
			riding && realCar && town.people.stats().boardedCars == 1 && town.cars.populationStats().completed == 1,
			U"A waiting resident enters an actual traffic vehicle");
		context.expect(town.people.stats().completed == 1 && person.origin == destination,
			U"Car arrival starts the final walk to the destination");
		context.expect(town.cars.vehicles().none([](const Vehicle& car) { return car.passengerId == 0; }),
			U"The occupied car is parked when the resident alights");
	});
	runner.add(U"Pedestrian.ActualTrainTransferAndCapacity", [](TestContext& context)
	{
		AppDirectory directory;
		Town town{200, true};
		context.expectEqual(
			town.people.network().stations().size(), 2, U"Both real station entrances join the walking graph");
		const size_t from = town.siteNear({260, 20, 140}), to = town.siteNear({740, 20, 140});
		const auto destination = town.people.network().sites()[to].key;
		for (int id = 0; id < 200; ++id)
		{
			town.place(id, from);
			town.people.beginTrip(id, destination, PedestrianTripMode::Train);
		}
		bool riding = false;
		int peak = 0;
		bool duplicate = false;
		for (int frame = 0; frame < 14000 && town.people.stats().completed == 0; ++frame)
		{
			town.step(.1);
			riding |= town.people.stats().ridingTrain > 0;
			for (const auto& train : town.trains.trains())
			{
				peak = Max(peak, train.passengerCount);
				duplicate |= train.passengerCount > 160;
			}
		}
		JSON report;
		report[U"peakOccupancy"] = peak;
		report[U"boarded"] = town.people.stats().boardedTrains;
		report[U"completed"] = town.people.stats().completed;
		report[U"waiting"] = town.people.stats().waitingTrain;
		report[U"time"] = town.now;
		report.save(directory.previous + U"TestResults/pedestrian_train.json");
		context.expect(
			riding && town.people.stats().boardedTrains > 0 && peak > 0, U"Residents board a real scheduled train");
		context.expect(!duplicate, U"A two-car train never exceeds its 160-person capacity");
		context.expect(town.people.stats().completed > 0, U"Alighting passengers walk to their original destination");
	});
	runner.add(U"Pedestrian.DisabledServiceAndRoadChanges", [](TestContext& context)
	{
		AppDirectory directory;
		Town town{1, true};
		town.railway.schedules().front().enabled = false;
		town.people.invalidate();
		town.step(.1);
		const size_t from = town.siteNear({260, 20, 140}), to = town.siteNear({740, 20, 140});
		town.place(0, from);
		town.people.beginTrip(0, town.people.network().sites()[to].key, PedestrianTripMode::Train);
		context.expect(town.people.people()[0].mode == PedestrianTripMode::Walk,
			U"A suspended timetable is not offered as a transfer");
		town.roads.getEdge(town.edges[0])->edgeState = EdgeState::Planned;
		town.people.invalidate();
		town.step(.1);
		context.expect(town.people.network().sites().size() < 48,
			U"Closed or demolished roadbeds lose their pedestrian connections");
		for (const auto& person : town.people.people())
		{
			context.expect(std::isfinite(person.position.x + person.position.y + person.position.z),
				U"A rebuilt graph does not leave invalid positions");
		}
	});
	runner.add(U"Pedestrian.TenThousandDistanceBudget", [](TestContext& context)
	{
		AppDirectory directory;
		JSON report;
		for (const bool far : {false, true})
		{
			Town town{10000};
			const Vec3 camera = far ? Vec3{10000, 100, 10000} : Vec3{500, 20, 500};
			double totalMs = 0;
			int updates = 0, maximumPlans = 0;
			for (int frame = 0; frame < 1200; ++frame)
			{
				town.step(1.0 / 60, camera, false);
				maximumPlans = Max(maximumPlans, town.people.stats().planned);
				if (frame >= 700)
				{
					totalMs += town.people.stats().updateMs;
					updates += town.people.stats().nearUpdates + town.people.stats().farUpdates;
				}
			}
			const String name = far ? U"far" : U"near";
			report[name][U"averageUpdateMs"] = totalMs / 500;
			report[name][U"positionUpdates"] = updates;
			report[name][U"walking"] = town.people.stats().walking;
			context.expectEqual(
				town.people.people().size(), 10000, U"The population retains all ten thousand residents");
			context.expect(maximumPlans <= GenerationSettings::get().pedestrians_routesPerUpdate,
				U"Routing never exceeds the per-frame budget");
			context.expect(totalMs / 500 < 12, U"Ten thousand resident updates stay within a bounded CPU budget");
		}
		context.expect(
			report[U"far"][U"positionUpdates"].get<int>() < report[U"near"][U"positionUpdates"].get<int>() * .1,
			U"Distant walkers avoid at least 90 percent of detailed position updates");
		report.save(directory.previous + U"TestResults/pedestrian_scaling.json");
	});
	runner.add(U"Pedestrian.ParkingStockAndMergeClearance", [](TestContext& context)
	{
		AppDirectory directory;
		Town town{30};
		const size_t from = town.siteNear({260, 20, 140}), to = town.siteNear({860, 20, 700});
		const auto destination = town.people.network().sites()[to].key;
		for (int id = 0; id < 30; ++id)
		{
			town.place(id, from);
			town.people.beginTrip(id, destination, PedestrianTripMode::Car);
		}
		int waiting = 0;
		for (int frame = 0; frame < 5000; ++frame)
		{
			town.step(.2);
			waiting = Max(waiting, town.people.stats().waitingCar);
			// 完了後の新しい旅行を止め、片道で消費する駐車台数を数える。
			for (auto& person : const_cast<Array<Pedestrian>&>(town.people.people()))
			{
				if (person.state == PedestrianState::Inside)
				{
					person.readyAt = 1e9;
				}
			}
		}
		context.expect(waiting > 0 && town.people.stats().boardedCars > 1 && town.people.stats().boardedCars <= 12,
			U"Parking inventory and reserved destination spaces limit departures; excess demand waits");
		context.expect(town.cars.populationStats().completed == town.people.stats().boardedCars,
			U"Every admitted parking departure completes its real vehicle trip");
		JSON report;
		report[U"boarded"] = town.people.stats().boardedCars;
		report[U"peakWaiting"] = waiting;
		report[U"arrivedCars"] = town.cars.populationStats().completed;
		report.save(directory.previous + U"TestResults/pedestrian_parking.json");
	});
	runner.add(U"Pedestrian.CrossingYieldsAndCoarseStep", [](TestContext& context)
	{
		AppDirectory directory;
		Town town{1};
		const auto& links = town.people.network().links();
		size_t index = 0;
		while (index < links.size() && links[index].crossingNode < 0)
		{
			++index;
		}
		context.expect(index < links.size(), U"The fixture has a crossing");
		if (index == links.size())
		{
			return;
		}
		const auto& link = links[index];
		auto& person = const_cast<Array<Pedestrian>&>(town.people.people())[0];
		person.state = PedestrianState::Walking;
		person.node = link.a;
		person.position = town.people.network().nodes()[link.a].position;
		person.route.steps = {static_cast<int>(index) + 1};
		person.speed = 1.3f;
		person.sampledAt = -4;
		person.destination = town.people.network().sites().back().key;
		town.cars.spawnOnEdge(link.crossingEdge, town.graph, VehicleType::PassengerCar, link.crossingEdge);
		auto& car = const_cast<Array<Vehicle>&>(town.cars.vehicles()).front();
		const auto* road = town.roads.getEdge(link.crossingEdge);
		const bool atA = road->nodeA == link.crossingNode;
		car.arcPos = atA ? road->cutoffA + 10 : road->length - road->cutoffB - 10;
		car.speed = 8;
		const Vec3 initial = person.position;
		town.step(.1, {10000, 20, 10000}, false);
		context.expect(person.position == initial, U"A distant walker also yields to an approaching vehicle");
		car.speed = 0;
		person.sampledAt = -4;
		town.step(.1, {10000, 20, 10000}, false);
		context.expect(person.distance > 0 && person.distance < .14,
			U"Coarse elapsed time cannot teleport a walker across the carriageway");
		const auto before = person.distance;
		town.step(.1, {10000, 20, 10000}, false);
		context.expect(person.distance > before, U"A distant pedestrian already crossing continues every frame");
	});
	runner.add(U"Pedestrian.ReplacedPlotAndFailedVehicle", [](TestContext& context)
	{
		AppDirectory directory;
		Town town{1};
		const auto oldSite = town.people.network().sites().front();
		const int local = static_cast<int>(oldSite.key % (ZONE_CELLS * ZONE_CELLS));
		const Point cell{local % ZONE_CELLS, local / ZONE_CELLS}, replacement = cell + Point{1, 0};
		auto& buildings = town.world.getChunk({0, 0})->buildingGrid;
		context.expect(buildings[replacement].type == BuildingType::None, U"Replacement slot is vacant");
		buildings[replacement] = buildings[cell];
		buildings[replacement].offsetX -= 16;
		buildings[cell].type = BuildingType::None;
		const auto revision = town.people.network().revision();
		town.step(16);
		context.expect(town.people.network().revision() > revision && town.people.network().sites().size() == 48,
			U"Replacing a plot updates the graph even when the building count stays constant");
		context.expect(
			!town.people.network().siteIndex(oldSite.key), U"The demolished plot is no longer a destination");
		const size_t from = town.siteNear({260, 20, 140}), to = town.siteNear({860, 20, 700});
		town.place(0, from);
		town.people.beginTrip(0, town.people.network().sites()[to].key, PedestrianTripMode::Car);
		for (int frame = 0; frame < 2000 && town.people.stats().ridingCar == 0; ++frame)
		{
			town.step(.1);
		}
		context.expect(town.people.stats().ridingCar == 1, U"A resident boards before the route becomes unavailable");
		RouteResponse failure;
		failure.vehicleId = town.people.people()[0].carrier;
		failure.found = false;
		town.cars.applyRouteResponse(failure);
		town.step(.1);
		context.expect(town.people.stats().ridingCar == 0 && town.cars.vehicleCount() == 0,
			U"A failed vehicle route releases its passenger and does not leave a permanent rider");
		context.expect(
			town.people.stats().completed == 0, U"A failed car route never counts as arrival at the destination plot");
	});
	runner.add(U"Pedestrian.GpuLodAndOccupancyVisibility", [](TestContext& context)
	{
		AppDirectory directory;
		const Size size{640, 480};
		const RenderTexture target{size, TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes};
		PedestrianRenderer renderer;
		JSON report;
		const auto draw = [&](const Array<Pedestrian>& people, const BasicCamera3D& camera)
		{
			{
				const ScopedRenderTarget3D scope{target.clear(ColorF{.1, .2, .3})};
				const ScopedRenderStates3D states{DepthStencilState::DepthTestWrite, RasterizerState::SolidCullBack};
				Graphics3D::SetCameraTransform(camera);
				Graphics3D::SetSunDirection(Vec3{1, 1, -1}.normalized());
				Graphics3D::SetSunColor(ColorF{.8});
				Graphics3D::SetGlobalAmbientColor(ColorF{.4});
				renderer.render(people, 2, camera);
			}
			Graphics3D::Flush();
			Image image;
			target.readAsImage(image);
			return image;
		};
		Array<Pedestrian> people;
		for (int id = 0; id < 10000; ++id)
		{
			Pedestrian p;
			p.id = id;
			p.position = {static_cast<double>(id % 100) * 1.5, 0, static_cast<double>(id / 100) * 1.5};
			p.state = PedestrianState::Walking;
			people << p;
		}
		const BasicCamera3D farCamera{size, 45_deg, Vec3{75, 330, -120}, Vec3{75, 0, 75}};
		const auto farImage = draw(people, farCamera);
		const auto far = renderer.stats();
		int pixels = 0;
		for (const auto color : farImage)
		{
			pixels += color != farImage[0][0];
		}
		context.expect(far.submitted == 10000 && far.drawCalls <= 40 && far.detailed == 0,
			U"Ten thousand far pedestrians render in at most forty GPU batches");
		context.expect(pixels > 1000, U"GPU readback confirms visible crowd geometry");
		report[U"farDrawCalls"] = far.drawCalls;
		report[U"farTriangles"] = far.triangles;
		report[U"farPixels"] = pixels;
		people.resize(10);
		for (int id = 0; id < 10; ++id)
		{
			people[id].position = {id * .9, 0, 0};
		}
		const BasicCamera3D nearCamera{size, 45_deg, Vec3{4.5, 3, -10}, Vec3{4.5, .9, 0}};
		const auto nearImage = draw(people, nearCamera);
		pixels = 0;
		HashSet<uint32> colors;
		for (const auto color : nearImage)
		{
			if (color != nearImage[0][0])
			{
				++pixels;
				colors.insert(static_cast<uint32>(color.r) << 16 | static_cast<uint32>(color.g) << 8 | color.b);
			}
		}
		context.expect(renderer.stats().detailed == 10 && pixels > 1500 && colors.size() > 15,
			U"Close walkers have animated limbs and varied clothing and skin colors");
		nearImage.save(directory.previous + U"Screenshot/pedestrians_near.png");
		report[U"nearPixels"] = pixels;
		report[U"colors"] = colors.size();
		for (auto& person : people)
		{
			person.state = PedestrianState::RidingTrain;
		}
		draw(people, nearCamera);
		context.expect(renderer.stats().submitted == 0, U"Riders are not drawn simultaneously on the street");
		report.save(directory.previous + U"TestResults/pedestrian_gpu.json");
	});
}
