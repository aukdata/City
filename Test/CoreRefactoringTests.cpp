#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/debug/DebugLog.hpp"
#include "src/render/MeshGeometry.hpp"
#include "src/railway/TrainNetwork.hpp"
#include "src/railway/RailwaySite.hpp"
#include "src/save/RoadBinary.hpp"
#include "src/save/ChunkDevelopmentStorage.hpp"
#include "src/sim/ParallelJobs.hpp"
#include "src/sim/SimThread.hpp"
#include <fstream>
#include <stdexcept>

namespace
{
	/// @brief ファイル境界の検証に使うバイト列を、保存実装と独立して読む。
	std::string bytes(const std::string& path)
	{
		std::ifstream input{path, std::ios::binary};
		return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
	}

	void putBytes(const std::string& path, const std::string& data)
	{
		std::ofstream output{path, std::ios::binary | std::ios::trunc};
		output.write(data.data(), static_cast<std::streamsize>(data.size()));
	}

	RoadNetwork fixture()
	{
		RoadNetwork roads;
		const int a = roads.addNode({0, 20, 0}), b = roads.addNode({100, 20, 0}), c = roads.addNode({200, 25, 0});
		for (const auto pair : {std::pair{a, b}, std::pair{b, c}})
		{
			const Vec3 from = roads.getNode(pair.first)->position, to = roads.getNode(pair.second)->position;
			const int id = *roads.addEdge(pair.first, pair.second, from.lerp(to, 1.0 / 3), from.lerp(to, 2.0 / 3));
			roads.getEdge(id)->edgeState = EdgeState::Existing;
		}
		SignalPlacement signal;
		signal.signalDefId = U"test_signal";
		signal.phases << SignalPhaseDef{15, {0, 1}};
		roads.getNode(b)->signalPlacement = signal;
		roads.getNode(b)->getAttachment(0)->control = TrafficControl::Signal;
		RoadRoute route; route.id = 8; route.name = U"試験道路"; route.edgeIds = {0, 1};
		roads.addRouteRaw(route);
		RoadPlan plan; plan.id = 12; plan.name = U"保存計画"; plan.routeName = route.name; plan.edgeIds = {0, 1};
		plan.viaPoints = {{100, 20, 0}}; roads.addPlanRaw(plan);
		RoadObject object; object.parentEdgeId = 0; object.arcPos = 25; roads.addObject(object);
		RoadMarkingPlacement marking; marking.id = 9; marking.edgeId = 0;
		roads.addManualMarking(marking);
		return roads;
	}

	/// @brief タイムアウト内に届く応答を待つ。性能値は合否条件にしない。
	Optional<RouteResponse> response(SimThread& service, int vehicle)
	{
		const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds{2};
		while (std::chrono::steady_clock::now() < limit)
		{
			for (const auto& item : service.drainResponses())
			{
				if (const auto* route = std::get_if<RouteResponse>(&item); route && route->vehicleId == vehicle) { return *route; }
			}
			std::this_thread::sleep_for(std::chrono::milliseconds{1});
		}
		return none;
	}
}

void registerCoreRefactoringTests(TestRunner& runner)
{
	runner.add(U"CoreRefactoring.BinaryNodeTypesAndExtensions", [](TestContext& context)
	{
		auto roads = fixture();
		roads.getNode(1)->type = NodeType::Diverge;
		context.expect(RoadBinary::write(U"TestResults/core_chunk.bin", 2, 3, roads.nodes(), roads.edges()), U"Chunk writes");
		Array<RoadNode> nodes; Array<RoadEdge> edges;
		context.expect(RoadBinary::read(U"TestResults/core_chunk.bin", nodes, edges), U"Chunk reads");
		context.expectEqual(nodes.size(), 3, U"All nodes restored");
		if (nodes.size() == 3)
		{
			context.expect(nodes[1].type == NodeType::Diverge, U"Node kind survives decoding");
			context.expect(nodes[1].signalPlacement && nodes[1].signalPlacement->phases.size() == 1, U"Signal phases survive decoding");
		}
		context.expect(RoadBinary::writeGlobal(U"TestResults/core_global.bin", roads), U"Global writes");
		RoadNetwork loaded;
		context.expect(RoadBinary::readGlobal(U"TestResults/core_global.bin", loaded), U"Global reads through the same decoder");
		context.expect(loaded.getNode(1) && loaded.getNode(1)->type == NodeType::Diverge, U"Raw insertion does not replace the saved node kind");
		context.expectEqual(loaded.objects().size(), 1, U"Object extension restores");
		context.expectEqual(loaded.routes().size(), 1, U"Route extension restores");
		context.expectEqual(loaded.plans().size(), 1, U"Plan extension restores");
		context.expectEqual(loaded.manualMarkings().size(), 1, U"Marking extension restores");
	});

	runner.add(U"CoreRefactoring.BinaryEveryTruncationIsAtomic", [](TestContext& context)
	{
		const auto roads = fixture();
		context.expect(RoadBinary::writeGlobal(U"TestResults/core_complete.bin", roads), U"Complete fixture writes");
		const std::string complete = bytes("TestResults/core_complete.bin");
		context.expect(!complete.empty(), U"Complete file contains records");
		bool atomic = true;
		for (size_t length = 0; length < complete.size(); ++length)
		{
			putBytes("TestResults/core_cut.bin", complete.substr(0, length));
			RoadNetwork loaded;
			RoadNode original; original.id = 1000; original.position = {3, 4, 5}; loaded.addNodeRaw(original);
			atomic &= !RoadBinary::readGlobal(U"TestResults/core_cut.bin", loaded);
			atomic &= loaded.nodes().size() == 1 && loaded.edges().isEmpty() && loaded.nextNodeId() == 1001;
			atomic &= loaded.getNode(1000) && loaded.getNode(1000)->position == original.position;
		}
		context.expect(atomic, U"Every incomplete global snapshot is rejected without changing the destination");
		Array<RoadNode> nodes{RoadNode{}}; nodes[0].id = 55;
		Array<RoadEdge> edges{RoadEdge{}}; edges[0].id = 77;
		putBytes("TestResults/core_cut.bin", complete.substr(0, 80));
		nodes.resize(1); edges.resize(1);
		context.expect(!RoadBinary::read(U"TestResults/core_cut.bin", nodes, edges), U"Incomplete chunk rejected");
		context.expect(nodes.size() == 1 && nodes[0].id == 55 && edges.size() == 1 && edges[0].id == 77, U"Incomplete chunk leaves both output arrays unchanged");
	});

	runner.add(U"CoreRefactoring.BinaryStringLimit", [](TestContext& context)
	{
		RoadNetwork roads;
		RoadRoute route; route.id = 0; route.name = String(65535, U'a'); roads.addRouteRaw(route);
		context.expect(RoadBinary::writeGlobal(U"TestResults/core_string.bin", roads), U"Maximum uint16 byte length is representable");
		RoadNetwork loaded;
		context.expect(RoadBinary::readGlobal(U"TestResults/core_string.bin", loaded), U"Maximum length roundtrips");
		context.expectEqual(loaded.routes().front().name.size(), 65535, U"Maximum length is intact");
		const std::string previous = bytes("TestResults/core_string.bin");
		roads.getRoute(0)->name = String(21846, U'道');
		context.expect(!RoadBinary::writeGlobal(U"TestResults/core_string.bin", roads), U"Overflow is measured in UTF-8 bytes");
		context.expect(bytes("TestResults/core_string.bin") == previous, U"Overflow is rejected before truncating the destination");
	});

	runner.add(U"CoreRefactoring.SnapshotDeletionAndSharedFields", [](TestContext& context)
	{
		auto roads = fixture();
		auto incremental = SimGraph::build(roads);
		roads.removeEdge(0);
		incremental.updateAround({1}, roads);
		context.expect(!incremental.getEdge(0), U"Previous attachments locate removed edges");
		context.expect(incremental.getNode(0) && incremental.getNode(0)->edgeIds.isEmpty(), U"Other endpoint receives the removal");
		roads.getNode(2)->position = {230, 27, 20};
		roads.getEdge(1)->speedLimit = 37;
		incremental.updateAround({2}, roads);
		const auto complete = SimGraph::build(roads);
		const auto* edge = incremental.getEdge(1); const auto* expected = complete.getEdge(1);
		context.expect(edge && expected, U"Remaining edge exists in both snapshots");
		if (edge && expected)
		{
			context.expectNear(edge->speedLimit, expected->speedLimit, 0, U"Same speed field extraction");
			context.expectNear(edge->tangentAngleA, expected->tangentAngleA, 0, U"Same starting tangent calculation");
			context.expectNear(edge->tangentAngleB, expected->tangentAngleB, 0, U"Same ending tangent calculation");
		}
		roads.removeNode(2);
		context.expect(!roads.getEdge(1) && roads.getNode(1)->attachments.isEmpty(), U"Node removal leaves no dangling network edge");
		incremental.updateAround({2}, roads);
		context.expect(!incremental.getNode(2) && !incremental.getEdge(1), U"Deleted node removes its old incident edges");
	});

	runner.add(U"CoreRefactoring.SnapshotReparentingIncludesNewEndpoint", [](TestContext& context)
	{
		auto roads = fixture(); auto snapshot = SimGraph::build(roads);
		const int replacement = roads.addNode({250, 25, 40});
		roads.getEdge(1)->nodeB = replacement;
		roads.getNode(2)->removeEdge(1); roads.getNode(replacement)->addEdge(1);
		snapshot.updateAround({2}, roads);
		context.expect(snapshot.getEdge(1) && snapshot.getEdge(1)->nodeB == replacement, U"Previous endpoint discovers the reparented edge");
		const auto* target = snapshot.getNode(replacement);
		context.expect(target && target->edgeIds.includes(1), U"The new endpoint is included even when it is absent from the dirty list");
		context.expect(snapshot.getNode(2)->edgeIds.isEmpty(), U"Previous endpoint drops the edge");
	});

	runner.add(U"CoreRefactoring.InvalidRoadEndpointsPreserveIds", [](TestContext& context)
	{
		RoadNetwork roads; const int node = roads.addNode({0, 0, 0});
		context.expect(!roads.addEdge(node, 99, {}, {}), U"Missing endpoint rejected");
		context.expectEqual(roads.nextEdgeId(), 0, U"Rejected edge consumes no ID");
		context.expect(roads.getNode(node)->attachments.isEmpty(), U"Rejected edge adds no attachment");
	});

	runner.add(U"CoreRefactoring.RailwaySparseIdsAndAtomicRestore", [](TestContext& context)
	{
		TrainNetwork network;
		const int removed = network.addNode({0, 20, 0});
		const int a = network.addStation({100, 20, 0}, U"起点"), b = network.addStation({300, 20, 0}, U"終点");
		network.addEdge(removed, a, {33, 20, 0}, {66, 20, 0});
		const int retained = network.addEdge(a, b, {166, 20, 0}, {233, 20, 0});
		network.infrastructure().removeNode(removed); network.synchronize();
		const auto saved = network.saveState();
		TrainNetwork restored;
		context.expect(restored.restoreState(saved), U"Deleted node and edge IDs need not be contiguous");
		context.expect(restored.saveState().formatMinimum() == saved.formatMinimum(), U"Sparse IDs, geometry and station metadata survive restoring");
		context.expect(restored.getEdge(retained) && restored.infrastructure().nextEdgeId() == retained + 1, U"Restored IDs remain safe for further construction");
		JSON duplicate = saved; duplicate[U"edges"].push_back(saved[U"edges"][0]);
		context.expect(!restored.restoreState(duplicate), U"Duplicate edge IDs are rejected");
		duplicate[U"edges"][1][U"id"] = retained + 5;
		context.expect(!restored.restoreState(duplicate), U"Different IDs cannot bypass the parallel-edge construction rule");
		JSON exhausted = saved; exhausted[U"nodes"][0][U"id"] = std::numeric_limits<int>::max();
		context.expect(!restored.restoreState(exhausted), U"Restoring must not overflow the next-ID counter");
		context.expect(restored.saveState().formatMinimum() == saved.formatMinimum(), U"Invalid restore leaves the live graph unchanged");
	});

	runner.add(U"CoreRefactoring.RouteServiceRestartAndEmptyGraph", [](TestContext& context)
	{
		const auto roads = fixture();
		const auto graph = std::make_shared<const SimGraph>(SimGraph::build(roads));
		SimThread service;
		service.stop(); service.stop();
		service.start(graph);
		service.pushRequest(RouteRequest{10, 0, 0, 0, 0});
		const auto first = response(service, 10);
		context.expect(first && first->found, U"Initial graph answers a valid request");
		service.pushRequest(RouteRequest{11, 0, 0, 0, 0});
		service.start(nullptr);
		context.expect(service.drainResponses().isEmpty(), U"Restart discards old responses");
		service.pushRequest(RouteRequest{12, 0, 0, 0, 0});
		const auto empty = response(service, 12);
		context.expect(empty && !empty->found, U"Missing graph returns a failure instead of losing the request");
		service.start(graph);
		service.pushRequest(RouteRequest{13, 0, 0, 0, 0});
		const auto restarted = response(service, 13);
		context.expect(restarted && restarted->found, U"Restarted worker rebuilds its graph");
		service.stop(); service.stop();
		context.expect(service.drainResponses().isEmpty(), U"Stopped session leaves no response backlog");
	});

	runner.add(U"CoreRefactoring.ParallelJobsAndQueue", [](TestContext& context)
	{
		MessageQueue<int> queue;
		ParallelJobs::run(1000, [&](int index) { queue.push(index); }, 4);
		auto messages = queue.drain(); messages.sort();
		context.expectEqual(messages.size(), 1000, U"All independent tasks publish exactly once");
		bool ordered = messages.size() == 1000;
		for (size_t index = 0; index < messages.size(); ++index) { ordered &= messages[index] == static_cast<int>(index); }
		context.expect(ordered, U"Concurrent queue loses or duplicates no messages");
		context.expect(!queue.waitFor(std::chrono::milliseconds{1}), U"Drained queue times out");
		queue.push(1); context.expect(queue.waitFor(std::chrono::milliseconds{1}), U"Nonempty queue wakes");
		queue.clear(); context.expect(queue.drain().isEmpty(), U"Clear removes pending messages");
		int visits = 0; ParallelJobs::run(0, [&](int) { ++visits; }, 0);
		context.expectEqual(visits, 0, U"Empty range starts no task");
		ParallelJobs::run(1, [&](int) { ++visits; }, 0);
		context.expectEqual(visits, 1, U"Unknown hardware concurrency still executes tasks");
	});

	runner.add(U"CoreRefactoring.ParallelFailureJoinsBeforeRethrow", [](TestContext& context)
	{
		std::atomic<int> active{0}; bool caught = false;
		try
		{
			ParallelJobs::run(200, [&](int index)
			{
				++active;
				std::this_thread::sleep_for(std::chrono::milliseconds{1});
				--active;
				if (index == 3) { throw std::runtime_error{"worker failed"}; }
			}, 4);
		}
		catch (const std::runtime_error&) { caught = true; }
		context.expect(caught && active == 0, U"Worker exception is rethrown only after every worker has joined");
	});

	runner.add(U"CoreRefactoring.ConcurrentLogAndAppend", [](TestContext& context)
	{
		DebugLog::initialize(U"TestResults/core_log.txt");
		ParallelJobs::run(400, [](int index) { DBG_LOG(U"worker {}"_fmt(index)); }, 4);
		const auto snapshot = DebugLog::entries();
		context.expectEqual(snapshot.size(), DebugLog::MAX_LINES, U"Concurrent log retains a bounded history");
		DebugLog::clear(); context.expectEqual(snapshot.size(), DebugLog::MAX_LINES, U"Old drawing snapshot remains valid");
		context.expect(DebugLog::entries().isEmpty(), U"Clear only affects current history");
		DebugLog::shutdown();
		const std::string first = bytes("TestResults/core_log.txt");
		context.expectEqual(std::count(first.begin(), first.end(), '\n'), 402, U"Every concurrent line reaches the file");
		DebugLog::initialize(U"TestResults/core_log.txt", true); DBG_LOG(U"appended"); DebugLog::shutdown();
		const std::string appended = bytes("TestResults/core_log.txt");
		const std::string bom{"\xEF\xBB\xBF"};
		context.expect(appended.starts_with(bom) && appended.find(bom, 3) == std::string::npos, U"Append writes no embedded UTF-8 BOM");
		context.expect(appended.starts_with(first), U"Append preserves existing bytes");
	});

	runner.add(U"CoreRefactoring.NumericAssertionsRejectNonfinite", [](TestContext& context)
	{
		TestContext probe;
		const double nan = std::numeric_limits<double>::quiet_NaN();
		probe.expectNear(nan, 1, .1, U"nonfinite actual");
		probe.expectNear(1, nan, .1, U"nonfinite expected");
		probe.expectNear(1, 1, nan, U"nonfinite tolerance");
		probe.expectNear(1, 1, -.1, U"negative tolerance");
		context.expectEqual(probe.failures().size(), 4, U"Invalid numeric assertions must fail instead of accepting NaN comparisons");
	});


	runner.add(U"CoreRefactoring.ChunkDevelopmentExactLayout", [](TestContext& context)
	{
		Chunk original{{3, 5}}; original.isUrbanizationArea = true;
		original.zoneMap[{2, 7}] = ZoneType::Residential;
		original.zoneMap[{4, 9}] = ZoneType::Agriculture;
		original.buildingGrid[{2, 7}] = Building{BuildingType::UrbanHousePair, 123.5, .75f, 42, .33f, -4.25f, 7.5f};
		original.buildingGrid[{4, 9}] = Building{BuildingType::Farmland};
		LandPatch patch; patch.id = 9; patch.type = LandPatchType::GardenSoil;
		patch.elevationOffset = .015f; patch.materialVariant = 23; patch.sourceParcelKey = 1234567890123;
		patch.polygon = {{58000.00001, 58000.00001}, {58000.00002, 58000.00001}, {58000.00002, 58000.00002}};
		original.landPatches << patch;

		context.expect(ChunkDevelopmentStorage::write(U"TestResults/core_development.bin", original), U"Current zoning and buildings can be saved");
		Chunk restored{{3, 5}}; restored.heightMap[{0, 0}] = 17;
		context.expect(ChunkDevelopmentStorage::read(U"TestResults/core_development.bin", restored), U"Layout restores without generation");
		const auto& building = restored.buildingGrid[{2, 7}];
		context.expect(restored.isUrbanizationArea && restored.zoneMap[{2, 7}] == ZoneType::Residential
			&& restored.buildingGrid[{4, 9}].type == BuildingType::Farmland, U"Urban status, zones and farmland survive");
		context.expect(building.type == BuildingType::UrbanHousePair && building.edgeId == 42
			&& building.builtAt == 123.5 && building.angle == .75f && building.edgeT == .33f
			&& building.offsetX == -4.25f && building.offsetZ == 7.5f, U"Every building field restores exactly");

		context.expect(restored.landPatches.size() == 1 && restored.landPatches[0].polygon == patch.polygon
			&& restored.landPatches[0].sourceParcelKey == patch.sourceParcelKey
			&& restored.landPatches[0].materialVariant == 23 && restored.landPatches[0].elevationOffset == .015f,
			U"Land vertices and parcel metadata restore without float quantization");
		context.expect(restored.heightMap[{0, 0}] == 17, U"Development restoration leaves terrain intact");
		context.expect(ChunkDevelopmentStorage::write(U"TestResults/core_development_restored.bin", restored), U"Restored layout can be saved again");
		context.expect(bytes("TestResults/core_development.bin") == bytes("TestResults/core_development_restored.bin"), U"Save-load-save preserves every serialized byte");
	});

	runner.add(U"CoreRefactoring.ChunkDevelopmentFailureIsAtomic", [](TestContext& context)
	{
		Chunk original{{3, 5}}; original.isUrbanizationArea = true;
		original.zoneMap[{2, 7}] = ZoneType::Residential;
		original.buildingGrid[{2, 7}] = Building{BuildingType::Detached};
		original.buildingGrid[{4, 9}] = Building{BuildingType::Farmland};
		const bool written = ChunkDevelopmentStorage::write(U"TestResults/core_development.bin", original);
		context.expect(written, U"Atomicity fixture saves independently");
		if (!written) { return; }
		const auto complete = bytes("TestResults/core_development.bin");
		Chunk destination{{3, 5}}; destination.zoneMap[{0, 0}] = ZoneType::Industrial;
		destination.buildingGrid[{0, 0}] = Building{BuildingType::Factory};
		bool rejected = !complete.empty();
		for (size_t length = 0; length < complete.size(); ++length)
		{
			putBytes("TestResults/core_development_cut.bin", complete.substr(0, length));
			rejected &= !ChunkDevelopmentStorage::read(U"TestResults/core_development_cut.bin", destination);
			rejected &= destination.zoneMap[{0, 0}] == ZoneType::Industrial
				&& destination.buildingGrid[{0, 0}].type == BuildingType::Factory && !destination.isUrbanizationArea;
		}
		context.expect(rejected, U"Every truncation leaves the existing layout unchanged");
		std::string invalid = complete; invalid[15] = static_cast<char>(255);
		putBytes("TestResults/core_development_cut.bin", invalid);
		context.expect(!ChunkDevelopmentStorage::read(U"TestResults/core_development_cut.bin", destination), U"Invalid zone values are rejected");
		putBytes("TestResults/core_development_cut.bin", complete + "x");
		context.expect(!ChunkDevelopmentStorage::read(U"TestResults/core_development_cut.bin", destination), U"Unexpected trailing bytes are rejected");
		Chunk wrongCoordinate{{4, 5}};
		context.expect(!ChunkDevelopmentStorage::read(U"TestResults/core_development.bin", wrongCoordinate), U"A chunk cannot consume another chunk's layout");
		destination.buildingGrid[{0, 0}].angle = std::numeric_limits<float>::quiet_NaN();
		context.expect(!ChunkDevelopmentStorage::write(U"TestResults/core_development.bin", destination), U"Nonfinite building values cannot be published");
		context.expect(bytes("TestResults/core_development.bin") == complete, U"Write preflight preserves the previous file");
	});

	runner.add(U"CoreRefactoring.RoadCoordinatesRetainDoublePrecision", [](TestContext& context)
	{
		RoadNetwork roads;
		const Vec3 a{58000.000012345, 123.123456789, 61000.000012345};
		const Vec3 b{58100.987654321, 124.987654321, 61100.987654321};
		const int nodeA = roads.addNode(a), nodeB = roads.addNode(b);
		const Vec3 controlA = a + Vec3{33.333333333, .111111111, 10.111111111};
		const Vec3 controlB = b - Vec3{33.333333333, .111111111, 10.111111111};
		const auto edge = roads.addEdge(nodeA, nodeB, controlA, controlB, RoadType::LocalRoad, 2);
		context.expect(edge.has_value(), U"Precision fixture has a road");
		context.expect(RoadBinary::writeGlobal(U"TestResults/core_precision.bin", roads), U"Precise roads save");
		RoadNetwork restored;
		context.expect(RoadBinary::readGlobal(U"TestResults/core_precision.bin", restored), U"Precise roads restore");
		const auto* restoredNode = restored.getNode(nodeA); const auto* restoredEdge = edge ? restored.getEdge(*edge) : nullptr;
		context.expect(restoredNode && restoredEdge && restoredNode->position == a
			&& restoredEdge->ctrlA == controlA && restoredEdge->ctrlB == controlB, U"World coordinates retain every double bit");
	});


	runner.add(U"CoreRefactoring.RailReferenceOrderSurvivesSlotReuse", [](TestContext& context)
	{
		RoadNetwork roads;
		const int a = roads.addNode({0, 20, 100}), b = roads.addNode({100, 20, 100}), c = roads.addNode({200, 20, 100});
		const int first = *roads.addEdge(a, b, {33, 20, 100}, {66, 20, 100});
		const int second = *roads.addEdge(b, c, {133, 20, 100}, {166, 20, 100});
		roads.removeEdge(first); roads.removeEdge(second);
		TrainNetwork trains; trains.bind(&roads);
		const int station = trains.addStation({300, 20, 0}, U"順序検証");
		const int left = trains.addNode({100, 20, 0}), right = trains.addNode({500, 20, 0});
		trains.addEdge(station, left, {233, 20, 0}, {166, 20, 0});
		trains.addEdge(station, right, {366, 20, 0}, {433, 20, 0});
		const auto originalEdges = trains.getNode(station)->edgeIds;
		const auto originalFrame = RailwaySite::stationFrame(trains, station);
		trains.synchronize();
		const auto synchronizedFrame = RailwaySite::stationFrame(trains, station);
		context.expect(trains.getNode(station)->edgeIds == originalEdges, U"Freed storage slots do not reorder station connections");
		context.expect(originalFrame && synchronizedFrame && originalFrame->along == synchronizedFrame->along,
			U"Synchronizing does not reverse the station or its protected parcel footprint");
		TrainNetwork restored; restored.bind(&roads);
		context.expect(restored.restoreState(trains.saveState()), U"Shared rail metadata restores");
		const auto restoredFrame = RailwaySite::stationFrame(restored, station);
		context.expect(originalFrame && restoredFrame && originalFrame->along == restoredFrame->along,
			U"Saved station orientation survives restore");
	});


	runner.add(U"CoreRefactoring.BinaryTopologyPreservesStoredGeometry", [](TestContext& context)
	{
		auto roads = fixture();
		const int throughNode = roads.addNode({50, 20, 0});
		roads.getNode(throughNode)->addEdge(0);
		roads.getNode(throughNode)->getAttachment(0)->isThrough = true;
		roads.getNode(throughNode)->getAttachment(0)->lateralOffset = 2.5f;
		roads.getEdge(0)->cutoffA = 3.25f; roads.getEdge(0)->cutoffB = 7.5f;
		roads.getEdge(1)->parts.clear();
		// Saved movement identities must describe the current attachments and parts, as after any
		// real edit. Without lane parts the junction has no movements left for the fixture signal.
		roads.getNode(1)->signalPlacement.reset();
		for (const int id : {0, 1, 2, throughNode}) { roads.rebuildLaneConnections(id); }
		context.expect(RoadBinary::writeGlobal(U"TestResults/core_topology.bin", roads), U"Custom topology saves");
		RoadNetwork restored; restored.addNode({999, 0, 999});
		context.expect(RoadBinary::readGlobal(U"TestResults/core_topology.bin", restored), U"Global restore replaces an existing network");
		const auto* node = restored.getNode(throughNode); const auto* edge = restored.getEdge(0);
		const auto* attachment = node ? node->getAttachment(0) : nullptr;
		context.expect(attachment && attachment->isThrough && attachment->lateralOffset == 2.5f,
			U"Through attachments are not lost when raw edge insertion rebuilds endpoint attachments");
		context.expect(edge && edge->cutoffA == 3.25f && edge->cutoffB == 7.5f, U"Saved cutoffs are not recomputed while restoring");
		context.expect(restored.getEdge(1) && restored.getEdge(1)->parts.isEmpty(), U"Restoration invents no unsaved default parts");
		context.expect(restored.getNode(0) && restored.getNode(0)->position == roads.getNode(0)->position,
			U"Previous destination geometry does not leak into a successful restore");
	});

	runner.add(U"CoreRefactoring.MeshTransformAndIndexOffsets", [](TestContext& context)
	{
		MeshData box = MeshData::Box(Float3{2, 3, 4}, Float3{1, 2, 3});
		const auto original = box;
		MeshGeometry::rotateMeshY(box, 2, 4, static_cast<float>(Math::HalfPi));
		for (size_t index = 0; index < box.vertices.size(); ++index)
		{
			const auto& before = original.vertices[index]; const auto& after = box.vertices[index];
			context.expectNear(after.pos.x, 2 - (before.pos.z - 4), 1e-5, U"Position rotates around supplied center");
			context.expectNear(after.normal.x, -before.normal.z, 1e-5, U"Normal rotates with position");
		}
		MeshData combined = original;
		MeshGeometry::appendMeshData(combined, box);
		context.expectEqual(combined.vertices.size(), original.vertices.size() * 2, U"Mesh append retains all vertices");
		const auto& face = combined.indices[original.indices.size()];
		context.expectEqual(face.i0, box.indices[0].i0 + original.vertices.size(), U"Appended triangle indices are offset");
	});
}
