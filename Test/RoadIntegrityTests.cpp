#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/road/RoadNetwork.hpp"
#include "src/sim/SimGraph.hpp"
#include "src/traffic/TrafficGraph.hpp"
#include "src/ui/RoadInspectorEdit.hpp"
#include "src/traffic/TrafficSpawn.hpp"
#include "src/save/RoadBinary.hpp"
#include "src/road/GuideSign.hpp"
#include "src/road/JunctionGeometry.hpp"
#include "src/render/RoadRenderer.hpp"
#include "src/render/WorldRenderer.hpp"
#include "src/world/World.hpp"
#include "src/gen/DistrictRoads.hpp"

namespace
{
	int addRoad(RoadNetwork& network, Vec3 a, Vec3 b, Vec3 c, Vec3 d)
	{
		const int first = network.addNode(a), last = network.addNode(d);
		const int id = *network.addEdge(first,last,b,c,RoadType::Arterial,4);
		network.getEdge(id)->edgeState = EdgeState::Open;
		return id;
	}
}

void registerRoadIntegrityTests(TestRunner& runner)
{
	runner.add(U"RoadIntegrity.InspectorSpeedRefreshesSimulation", [](TestContext& context)
	{
		RoadNetwork network;
		const int a = network.addNode({0, 0, 0}), b = network.addNode({100, 0, 0});
		const int id = *network.addEdge(a, b, {100.0 / 3, 0, 0}, {200.0 / 3, 0, 0});
		const int unrelatedId = addRoad(network, {0, 0, 100}, {30, 0, 100}, {70, 0, 100}, {100, 0, 100});
		auto* edge = network.getEdge(id);
		edge->edgeState = EdgeState::Open;
		edge->speedLimit = 30;
		network.getEdge(unrelatedId)->speedLimit = 50;
		SimGraph graph = SimGraph::build(network);
		TrafficGraph routes;
		routes.rebuild(graph, 0, {});
		int notifications = 0;
		// Refresh only when the production inspector helper invokes the same notification seam as GameScene.
		const auto notify = [&](int nodeA, int nodeB)
		{
			++notifications;
			context.expect(nodeA == a && nodeB == b, U"The inspector notifies the edited road's endpoints");
			graph = SimGraph::build(network);
			routes.rebuild(graph, 0, {});
		};
		const auto editSpeed = [&](float requestedSpeed)
		{
			return RoadInspectorEdit::editSpeedLimit(*edge, [&](float& value)
			{
				context.expect(&value == &edge->speedLimit, U"The numeric widget retains the road field's stable identity");
				value = requestedSpeed;
			}, notify);
		};
		const auto expectForwardCost = [&](double expected)
		{
			bool found = false;
			for (int lane = 0; lane < static_cast<int>(edge->lanes.size()); ++lane)
			{
				const auto* outgoing = routes.outgoingEdges(routes.entryNodeId(id, lane));
				if (!outgoing) { continue; }
				for (const auto& connection : *outgoing)
				{
					if (connection.type != GraphEdgeType::Forward) { continue; }
					found = true;
					context.expectNear(connection.cost, expected, .001, U"The 100m routing cost follows the inspector speed");
				}
			}
			context.expect(found, U"The production routing graph contains the edited road's forward cost");
		};
		context.expectNear(edge->length, 100, .001, U"The fixture is a 100m road");
		expectForwardCost(12);
		context.expect(!editSpeed(30), U"An unchanged inspector value is not dirty");
		context.expectEqual(notifications, 0, U"An unchanged value does not notify the network");
		context.expect(editSpeed(40), U"A changed speed invalidates the road display");
		context.expectNear(edge->speedLimit, 40, .001, U"The inspector commits the requested speed to the road");
		context.expectEqual(notifications, 1, U"A changed speed notifies the simulation exactly once");
		context.expectNear(graph.getEdge(id)->speedLimit, 40, .001, U"Live vehicle physics receives the new speed without a reload");
		expectForwardCost(9);
		context.expect(!editSpeed(40), U"Repeated input at the current speed is not dirty");
		context.expectEqual(notifications, 1, U"Repeated input does not redundantly refresh the simulation");
		context.expectNear(network.getEdge(unrelatedId)->speedLimit, 50, .001, U"The inspector leaves an unrelated road unchanged");
		context.expectNear(graph.getEdge(unrelatedId)->speedLimit, 50, .001, U"The unrelated simulation speed is preserved");
	});

	runner.add(U"RoadIntegrity.InspectorLaneEditsRefreshSimulation", [](TestContext& context)
	{
		RoadNetwork network;
		const int id = addRoad(network, {0, 0, 0}, {30, 0, 0}, {70, 0, 0}, {100, 0, 0});
		const int unrelatedId = addRoad(network, {0, 0, 100}, {30, 0, 100}, {70, 0, 100}, {100, 0, 100});
		auto* edge = network.getEdge(id);
		const auto originalLanes = edge->lanes;
		SimGraph graph = SimGraph::build(network);
		TrafficGraph routes; routes.rebuild(graph, 0, {});
		int notifications = 0;
		const auto notify = [&](int nodeA, int nodeB)
		{
			++notifications;
			context.expect(nodeA == edge->nodeA && nodeB == edge->nodeB, U"Section edits notify exactly the edited endpoints");
			graph.updateAround({nodeA, nodeB}, network);
			routes.rebuild(graph, 0, {});
		};
		const auto edit = [&](auto&& apply)
		{
			return RoadInspectorEdit::editSections(network, *edge, [&](RoadEdge& value)
			{
				context.expect(&value == edge, U"Section widgets retain the actual road field identities");
				return apply(value);
			}, notify);
		};
		context.expect(!edit([](RoadEdge&) { return false; }), U"An unchanged section is not dirty");
		context.expectEqual(notifications, 0, U"An unchanged section does not refresh the network");
		context.expect(edit([](RoadEdge& value) { value.lanes[0].op = OpState::Closed; return true; }), U"Closing a lane marks its section dirty");
		context.expectEqual(notifications, 1, U"Closing a lane refreshes the live network once");
		context.expect(graph.getEdge(id)->lanes[0].op == OpState::Closed, U"The live snapshot receives lane closure before reload");
		context.expect(!TrafficSpawn::laneOpen(*graph.getEdge(id), 0), U"Automatic traffic cannot spawn on the closed lane");
		context.expect(routes.entryNodeId(id, 0) < 0 && routes.exitNodeId(id, 0) < 0, U"Routing removes both nodes of the closed lane");
		context.expect(edit([](RoadEdge& value) { value.lanes[0].op = OpState::Open; return true; }), U"Reopening a lane marks its section dirty");
		context.expect(TrafficSpawn::laneOpen(*graph.getEdge(id), 0) && routes.entryNodeId(id, 0) >= 0, U"Reopening restores traffic eligibility and routing");
		context.expectEqual(notifications, 2, U"Reopening sends one additional update");
		context.expect(edit([](RoadEdge& value) { value.lanes[0].dir = LaneDir::Backward; return true; }), U"Direction changes use the section edit path");
		context.expect(graph.getEdge(id)->lanes[0].dir == LaneDir::Backward, U"The live snapshot receives the reversed direction");
		const auto* entry = routes.getLaneNode(routes.entryNodeId(id, 0));
		const auto* exit = routes.getLaneNode(routes.exitNodeId(id, 0));
		context.expect(entry && exit, U"The reversed lane remains represented in routing");
		if (entry && exit)
		{
			context.expectNear(entry->arcPos, edge->length, .001, U"A reversed lane enters at endpoint B");
			context.expectNear(exit->arcPos, 0, .001, U"A reversed lane exits at endpoint A");
		}
		const int appendedLane = static_cast<int>(edge->lanes.size());
		context.expect(edit([](RoadEdge& value) { Lane lane; lane.op = OpState::Open; value.lanes << lane; return true; }), U"Adding a lane uses the section edit path");
		context.expectEqual(graph.getEdge(id)->lanes.size(), edge->lanes.size(), U"The live snapshot receives the added lane count");
		context.expect(routes.entryNodeId(id, appendedLane) >= 0, U"The newly added open lane is immediately routable");
		context.expect(edit([&](RoadEdge& value) { value.lanes.remove_at(appendedLane); return true; }), U"Removing a lane uses the section edit path");
		context.expectEqual(graph.getEdge(id)->lanes.size(), edge->lanes.size(), U"The live snapshot removes the deleted lane");
		context.expect(routes.entryNodeId(id, appendedLane) < 0, U"The deleted lane is absent from routing");
		context.expect(edit([&](RoadEdge& value) { value.lanes = originalLanes; return true; }), U"The original lane structure can be restored");
		context.expectEqual(notifications, 6, U"Every changed section sends exactly one notification");
		context.expect(!edit([](RoadEdge&) { return false; }), U"Repeated idle frames do not mark the section dirty");
		context.expectEqual(notifications, 6, U"Idle frames do not send additional notifications");
		context.expect(graph.getEdge(id)->lanes[0].dir == originalLanes[0].dir, U"The restored direction reaches the live snapshot");
		context.expect(network.getEdge(unrelatedId)->lanes[0].op == OpState::Open
			&& graph.getEdge(unrelatedId)->lanes[0].dir == originalLanes[0].dir
			&& graph.getEdge(unrelatedId)->lanes.size() == originalLanes.size(), U"An unrelated road retains its lane state");
	});

	runner.add(U"RoadIntegrity.InspectorLaneEditPreservesSignalSnapshot", [](TestContext& context)
	{
		RoadNetwork network;
		const Vec3 center{500, 0, 500};
		const int junction = network.addNode(center);
		Array<int> arms;
		for (int arm = 0; arm < 2; ++arm)
		{
			const double angle = arm * Math::Pi;
			const Vec3 end = center + Vec3{Cos(angle) * 100, 0, Sin(angle) * 100};
			const int id = *network.addEdge(junction, network.addNode(end), center.lerp(end, 1.0 / 3), center.lerp(end, 2.0 / 3), RoadType::Arterial, 2);
			network.getEdge(id)->edgeState = EdgeState::Open;
			arms << id;
		}
		network.rebuildNodeConnectivity(junction, junction);
		auto* edge = network.getEdge(arms.front());
		auto* node = network.getNode(junction);
		context.expectEqual(node->laneConnections.size(), 2, U"The inspector fixture starts with two opposing joint movements");
		if (node->laneConnections.size() != 2) { return; }
		constexpr int kFirstMovementId = 2000;
		constexpr int kNextMovementId = 10000;
		constexpr float kAuthoredSeconds = 37;
		constexpr float kAllRedSeconds = 19;
		SignalPlacement authored{U"signal_3lamp"}; authored.yawOffset = .75f;
		SignalPhaseDef served, allRed; served.duration = kAuthoredSeconds; allRed.duration = kAllRedSeconds;
		for (size_t index = 0; index < node->laneConnections.size(); ++index)
		{
			node->laneConnections[index].id = kFirstMovementId + static_cast<int>(index) * 3;
			served.greenConnectionIds << node->laneConnections[index].id;
		}
		served.greenConnectionIds.reverse(); authored.phases = {served, allRed};
		node->signalPlacement = authored; node->nextConnectionId = kNextMovementId;
		node->getAttachment(edge->id)->control = TrafficControl::Stop;
		SimGraph graph = SimGraph::build(network);
		TrafficGraph routes; routes.rebuild(graph, 0, {});
		int notifications = 0;
		const auto notify = [&](int nodeA, int nodeB)
		{
			++notifications;
			graph.updateAround({nodeA, nodeB}, network);
			routes.rebuild(graph, 0, {});
		};
		const auto originalConnections = node->laneConnections;
		const auto touchesClosedLane = [&](const LaneConnection& connection)
		{
			return (connection.fromEdgeId == edge->id && connection.fromLaneIndex == 0)
				|| (connection.toEdgeId == edge->id && connection.toLaneIndex == 0);
		};
		context.expect(originalConnections.any(touchesClosedLane), U"The edited lane initially participates in the junction");
		context.expect(RoadInspectorEdit::editSections(network, *edge, [](RoadEdge& value)
		{
			value.lanes[0].op = OpState::Closed; return true;
		}, notify), U"Closing an inspector lane updates a connected road");
		context.expectEqual(notifications, 1, U"Connected lane closure notifies after rebuilding connectivity");
		context.expect(!node->laneConnections.any(touchesClosedLane), U"Closed lane movements are removed from the authored network");
		context.expect(!graph.getNode(junction)->laneConnections.any(touchesClosedLane), U"The live snapshot receives rebuilt junction connectivity");
		Array<int> expectedSurvivors;
		for (const int id : served.greenConnectionIds)
		{
			const auto it = std::find_if(originalConnections.begin(), originalConnections.end(), [&](const LaneConnection& connection) { return connection.id == id; });
			if (it != originalConnections.end() && !touchesClosedLane(*it)) { expectedSurvivors << id; }
		}
		context.expect(!expectedSurvivors.isEmpty(), U"Some authored green movements survive the closure");
		const auto verifyAuthored = [&]
		{
			context.expect(node->signalPlacement && node->signalPlacement->phases.size() >= 2, U"The authored green and all-red phases remain available");
			if (!node->signalPlacement || node->signalPlacement->phases.size() < 2) { return; }
			const auto& current = *node->signalPlacement;
			context.expectNear(current.yawOffset, authored.yawOffset, .001, U"Lane editing retains authored signal placement");
			context.expect(current.phases[0].duration == kAuthoredSeconds && current.phases[0].greenConnectionIds == expectedSurvivors, U"Surviving authored movements keep their IDs, order and timing");
			context.expect(current.phases[1].duration == kAllRedSeconds && current.phases[1].greenConnectionIds.isEmpty(), U"An intentional all-red phase retains its duration and position");
			context.expect(node->getAttachment(edge->id)->control == TrafficControl::Stop, U"Lane editing does not reset an authored approach control");
		};
		verifyAuthored();
		const auto roundTrip = [&](StringView suffix)
		{
			const FilePath path = U"TestResults/inspector_lanes_{}.bin"_fmt(suffix);
			const bool written = RoadBinary::writeGlobal(path, network);
			context.expect(written, U"The edited connected road saves with valid movement references");
			if (!written) { return; }
			RoadNetwork restored;
			const bool loaded = RoadBinary::readGlobal(path, restored, true);
			context.expect(loaded, U"Current v20 inspector edits reload without stale movement identities");
			if (!loaded) { return; }
			const auto* restoredNode = restored.getNode(junction);
			context.expect(restored.getEdge(edge->id)->lanes[0].op == edge->lanes[0].op, U"Reload retains the edited lane operation");
			context.expectEqual(restoredNode->nextConnectionId, node->nextConnectionId, U"Reload retains the movement allocation history");
			context.expectEqual(restoredNode->laneConnections.size(), node->laneConnections.size(), U"Reload retains the exact movement count");
			context.expect(restoredNode->signalPlacement && restoredNode->signalPlacement->phases.size() == node->signalPlacement->phases.size(), U"Reload retains the complete authored program");
			if (!restoredNode->signalPlacement) { return; }
			for (size_t phase = 0; phase < Min(restoredNode->signalPlacement->phases.size(), node->signalPlacement->phases.size()); ++phase)
			{
				const auto& before = node->signalPlacement->phases[phase];
				const auto& after = restoredNode->signalPlacement->phases[phase];
				context.expect(before.duration == after.duration && before.greenConnectionIds == after.greenConnectionIds, U"Reload retains exact phase timings and selected movement IDs");
			}
		};
		roundTrip(U"closed");
		context.expect(RoadInspectorEdit::editSections(network, *edge, [](RoadEdge& value)
		{
			value.lanes[0].op = OpState::Open; return true;
		}, notify), U"Reopening uses the same connected inspector edit path");
		verifyAuthored();
		context.expect(node->laneConnections.any(touchesClosedLane), U"Reopening restores the lane's junction movements");
		for (const auto& connection : node->laneConnections)
		{
			if (touchesClosedLane(connection))
			{
				context.expect(connection.id >= kNextMovementId, U"Reopened movements receive fresh IDs instead of retired authored identities");
			}
		}
		if (node->signalPlacement)
		{
			for (size_t phase = 2; phase < node->signalPlacement->phases.size(); ++phase)
			{
				for (const int id : node->signalPlacement->phases[phase].greenConnectionIds)
				{
					context.expect(id >= kNextMovementId, U"Additional green groups contain only newly created movements");
				}
			}
		}
		roundTrip(U"reopened");
	});

	runner.add(U"RoadIntegrity.EditRewiredSnapshotRefreshesNewEndpoint", [](TestContext& context)
	{
		RoadNetwork network;
		const int a=network.addNode({0,0,0}), b=network.addNode({100,0,0}), c=network.addNode({200,0,0});
		const int edge=*network.addEdge(a,b,{30,0,0},{70,0,0});
		SimGraph graph=SimGraph::build(network);
		network.getEdge(edge)->nodeB=c;
		network.getNode(c)->addEdge(edge);
		network.removeNode(b);
		graph.updateAround({b},network);
		context.expect(graph.getNode(b)==nullptr,U"The removed endpoint leaves the snapshot");
		context.expect(graph.getEdge(edge) && graph.getEdge(edge)->nodeB==c,U"The snapshot follows the retargeted road");
		context.expect(graph.getNode(c)->edgeIds.contains(edge),U"The new endpoint receives the road attachment");
	});

	runner.add(U"RoadIntegrity.EditNodeDeletionPreservesRewiredEdges", [](TestContext& context)
	{
		RoadNetwork network;
		const int a=network.addNode({0,0,0}), b=network.addNode({100,0,0}), c=network.addNode({200,0,0});
		const int edge=*network.addEdge(a,b,{30,0,0},{70,0,0});
		// Topology merges retarget roads before discarding the old node's attachments.
		network.getEdge(edge)->nodeB=c;
		network.getNode(c)->addEdge(edge);
		network.removeNode(b);
		context.expect(network.getNode(b)==nullptr,U"The replaced node is deleted");
		context.expect(network.getEdge(edge)!=nullptr,U"A road retargeted to a live node survives");
		context.expect(network.getNode(a)->getAttachment(edge) && network.getNode(c)->getAttachment(edge),U"Both retained endpoints stay connected");
	});
	runner.add(U"RoadIntegrity.EditSingleDirtyEndpointRefreshesNeighbors", [](TestContext& context)
	{
		RoadNetwork network;
		const int a=network.addNode({0,0,0}), b=network.addNode({100,0,0});
		const int edge=*network.addEdge(a,b,{30,0,0},{70,0,0});
		SimGraph graph=SimGraph::build(network);
		network.removeEdge(edge);
		graph.updateAround({a},network);
		context.expect(graph.getEdge(edge)==nullptr,U"Old connectivity identifies the removed road from one dirty endpoint");
		context.expect(graph.getNode(b)->edgeIds.empty(),U"The former opposite endpoint is refreshed too");
	});

	runner.add(U"RoadIntegrity.EditRemovalUpdatesSnapshot", [](TestContext& context)
	{
		RoadNetwork network;
		const int a=network.addNode({0,0,0}), b=network.addNode({100,0,0}), c=network.addNode({200,0,0});
		const int removed=*network.addEdge(a,b,{30,0,0},{70,0,0});
		network.addEdge(b,c,{130,0,0},{170,0,0});
		SimGraph graph=SimGraph::build(network);
		network.removeEdge(removed);
		graph.updateAround({a,b},network);
		context.expect(graph.getEdge(removed)==nullptr,U"The removed road is absent from the simulation snapshot");
		context.expectEqual(graph.edges.size(),SimGraph::build(network).edges.size(),U"Incremental and full snapshots contain the same roads");
	});
	runner.add(U"RoadIntegrity.EditSplitUpdatesSnapshot", [](TestContext& context)
	{
		RoadNetwork network;
		const int a=network.addNode({0,0,0}), b=network.addNode({100,0,0});
		const int removed=*network.addEdge(a,b,{30,0,0},{70,0,0});
		SimGraph graph=SimGraph::build(network);
		const int split=network.splitEdgeAt(removed,50);
		context.expect(split>=0,U"The road can be split");
		graph.updateAround({a,b,split},network);
		context.expect(graph.getEdge(removed)==nullptr,U"The original road is absent after splitting");
		context.expectEqual(graph.edges.size(),SimGraph::build(network).edges.size(),U"Split snapshots contain only the replacement roads");
	});
	runner.add(U"RoadIntegrity.EditNodeDeletionDetachesEdges", [](TestContext& context)
	{
		RoadNetwork network;
		const int a=network.addNode({0,0,0}), b=network.addNode({100,0,0}), c=network.addNode({200,0,0});
		network.addEdge(a,b,{30,0,0},{70,0,0});
		network.addEdge(b,c,{130,0,0},{170,0,0});
		network.removeNode(b);
		int dangling=0;
		for (const auto& edge:network.edges())
		{
			if (edge.id>=0 && (!network.getNode(edge.nodeA) || !network.getNode(edge.nodeB))) { ++dangling; }
		}
		context.expectEqual(dangling,0,U"Deleting a connected node leaves no roads with missing endpoints");
		context.expect(network.getNode(a)->attachments.empty() && network.getNode(c)->attachments.empty(),U"Surviving nodes detach the deleted roads");
	});
	runner.add(U"RoadIntegrity.EditSmoothingRefreshesLength", [](TestContext& context)
	{
		RoadNetwork network;
		const int a=network.addNode({0,0,0}), b=network.addNode({100,0,0}), c=network.addNode({100,0,100});
		network.addEdge(a,b,{30,0,0},{70,0,0});
		const int edge=*network.addEdge(b,c,{100,0,30},{100,0,70});
		network.smoothCurveAt(edge,b);
		const auto curve=network.getBezier(edge);
		context.expect(curve.has_value(),U"The smoothed road remains valid");
		if (curve) { context.expectNear(network.getEdge(edge)->length,curve->totalLength,.01,U"Cached length follows the changed curve"); }
	});

	runner.add(U"Generation.CastleTownClipsRegionalRoads", [](TestContext& context)
	{
		World world;
		world.reserveChunks();
		world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		for (int z=29;z<=34;++z) for (int x=29;x<=34;++x)
		{
			world.installChunkDirect(Point{x,z},HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,20.0f),20.0f,20.0f});
		}
		RoadNetwork network;
		const Vec3 origin{32768,20,32768};
		const auto regional = [&](Vec3 from,Vec3 to)
		{
			return addRoad(network,origin+from,origin+from+(to-from)/3,origin+from+(to-from)*2/3,origin+to);
		};
		const int cross = regional({-1600,0,800},{1600,0,900});
		regional({-800,0,-1600},{700,0,1600});
		const int localApproach = regional({-1400,0,400},{1600,0,600});
		network.getEdge(localApproach)->roadType = RoadType::LocalRoad;
		const int route = network.addRoute(RoadRouteKind::NationalRoute,U"城下街道",{cross},355);
		MapGenerator::Settlement settlement;
		settlement.center={origin.x,origin.z}; settlement.radius=700;
		settlement.kind=MapGenerator::SettlementKind::RegionalCity;
		DistrictRoads::KaidoSegment kaido; kaido.passesThrough=true; kaido.dirAtCenter={1,0};
		DistrictRoads::generateCastleTown(42,0,settlement,kaido,world,network);
		TextWriter report{U"TestResults/morphology_regional_connections.txt"};
		for (const auto& edge : network.edges())
		{
			if (edge.id<0) { continue; }
			const auto curve=network.getBezier(edge.id);
			for (int sample=1;sample<20;++sample)
			{
				const Vec3 p=curve->evaluate(sample/20.0f)-origin;
				if (Abs(p.x)>=999 || Abs(p.z)>=999) { continue; }
				const Vec3 direction=curve->p3-curve->p0;
				if (Min(Abs(direction.x),Abs(direction.z))>=0.01) { report<<U"edge={} from={} to={} point={}"_fmt(edge.id,curve->p0-origin,curve->p3-origin,p); }
				context.expect(Min(Abs(direction.x),Abs(direction.z))<0.01,U"No diagonal regional-road remnant crosses a planned town block");
			}
			context.expect(edge.length>40.0f,U"Boundary connections do not create tiny road shards");
		}
		HashSet<int> visited;
		int components=0;
		for (const auto& node : network.nodes())
		{
			if (node.id<0 || node.attachments.isEmpty() || visited.contains(node.id)) { continue; }
			++components;
			Array<int> pending{node.id}; visited.insert(node.id);
			for (size_t next=0;next<pending.size();++next) for (const int edgeId : network.getNode(pending[next])->edgeIds())
			{
				const auto* edge=network.getEdge(edgeId);
				const int other=edge->nodeA==pending[next]?edge->nodeB:edge->nodeA;
				if (visited.insert(other).second) { pending << other; }
			}
		}
		context.expectEqual(components,1,U"All regional entrances reconnect to the town");
		context.expect(network.getRoute(route) && !network.getRoute(route)->edgeIds.isEmpty(),U"Regional route survives town clipping");
	});

	runner.add(U"RoadIntegrity.RightAngleWidthTransition", [](TestContext& context)
	{
		RoadNetwork network;
		const int center = network.addNode({0,20,0});
		const int north = network.addNode({0,20,120}), east = network.addNode({120,20,0});
		const int a = *network.addEdge(north,center,{0,20,80},{0,20,40},RoadType::Arterial,2);
		const int b = *network.addEdge(center,east,{40,20,0},{80,20,0},RoadType::Arterial,4);
		network.getEdge(a)->edgeState = network.getEdge(b)->edgeState = EdgeState::Open;
		network.rebuildNodeConnectivity(center, north);
		const auto layout = JunctionGeometry::build(network,center);
		TextWriter report{ U"TestResults/corner_diagnostics.txt" };
		report << U"cutA={} cutB={} repaired={}"_fmt(network.getEdge(a)->cutoffB,network.getEdge(b)->cutoffA,layout.repaired);
		context.expect(network.getEdge(a)->cutoffB > network.getEdge(b)->totalWidth()*.5f+4.0f,U"The narrow approach leaves room for the wider road's inner corner");
		context.expect(!layout.repaired,U"A simple right angle has a valid junction outline without polygon repair");
	});
	runner.add(U"RoadIntegrity.GuideSignsAvoidJunctions", [](TestContext& context)
	{
		TextWriter report{ U"TestResults/sign_clearance.txt" };
		for (const double spacing : {80.0,500.0})
		{
		RoadNetwork network;
		Array<int> centers;
		for (int i=0;i<5;++i) { centers << network.addNode({i*spacing,20,0}); }
		const auto add = [&](int a,int b)
		{
			const Vec3 p=network.getNode(a)->position,q=network.getNode(b)->position;
			const int id=*network.addEdge(a,b,p+(q-p)/3,p+(q-p)*2/3,RoadType::Arterial,4);
			network.getEdge(id)->edgeState=EdgeState::Open;
			return id;
		};
		Array<int> routeEdges;
		for (int i=1;i<5;++i) { routeEdges << add(centers[i-1],centers[i]); }
		network.addRoute(RoadRouteKind::NationalRoute,U"国道101",routeEdges,101);
		for (int i=0;i<5;++i)
		{
			const int branch=network.addNode({i*spacing,20,150});
			add(centers[i],branch);
			network.addNamedDestination(branch,U"市街地",U"shigaichi",0);
		}
		double nearest=10000.0;
		int signs=0;
		for (const int id : routeEdges) for (const auto& sign : GuideSign::InferAutoForEdge(*network.getEdge(id),network))
		{
			++signs;
			const auto* edge=network.getEdge(sign.parentEdgeId);
			const Vec3 position=network.getBezier(edge->id)->positionAt(sign.nodeEndId==edge->nodeA?sign.arcOffset:edge->length-sign.arcOffset);
			for (const int nodeId : centers)
			{
				const auto* node=network.getNode(nodeId);
				if (node->attachments.size()<3) { continue; }
				nearest=Min(nearest,position.distanceFrom(node->position));
				context.expect(position.distanceFrom(node->position)>30.0,U"Automatic blue signs keep clear of every three-arm junction, including downstream junctions");
			}
		}
		if (spacing>100.0) { context.expect(signs>0,U"Blue signs are still generated where there is a safe approach"); }
		report << U"spacing={} signs={} nearestJunctionM={}"_fmt(spacing,signs,nearest);
		}
	});

	runner.add(U"RoadIntegrity.InheritedEndpointJoins", [](TestContext& context)
	{
		for (const bool reverse : {false,true})
		{
			RoadNetwork network;
			const int host=addRoad(network,{-100,0,0},{-100.0/3,0,0},{100.0/3,0,0},{100,0,0});
			network.getEdge(host)->designGrade=true;
			network.getEdge(host)->speedLimit=37;
			network.getEdge(host)->parts.front().defId=U"inherited_host_profile";
			const int route=network.addRoute(RoadRouteKind::NationalRoute,U"Endpoint host",{host},314);
			const int lane=reverse ? addRoad(network,{0,0,100},{0,0,200.0/3},{0,0,100.0/3},{0,0,0})
				: addRoad(network,{0,0,0},{0,0,100.0/3},{0,0,200.0/3},{0,0,100});
			const int endpoint=reverse ? network.getEdge(lane)->nodeB : network.getEdge(lane)->nodeA;
			Array<int> tracked{host};
			context.expect(network.resolveIntersections(-1,&tracked),U"An exact piece endpoint splits its intersected host");
			context.expectEqual(network.getNode(endpoint)->attachments.size(),size_t{3},U"The original endpoint is a shared T junction");
			context.expectEqual(tracked.size(),size_t{2},U"Tracked ownership follows both host descendants");
			const auto* saved=network.getRoute(route);
			context.expect(saved && saved->number==314 && saved->edgeIds.size()==2,U"Host route identity and order survive splitting");
			if (saved) for (const int id : saved->edgeIds)
			{
				const auto* edge=network.getEdge(id);
				context.expect(edge && edge->routeIds.contains(route) && edge->designGrade && edge->parts.front().defId==U"inherited_host_profile",U"Both descendants retain route/profile/design attributes");
				if (edge) { context.expectNear(edge->speedLimit,37,.001,U"Host speed remains unchanged"); }
			}
			context.expect(!network.resolveIntersections(),U"A resolved T junction is stable");
		}
	});
	runner.add(U"RoadIntegrity.CoincidentEndpoints", [](TestContext& context)
	{
		for (const bool parallel : {false,true})
		{
			RoadNetwork network;
			const int first=addRoad(network,{-100,0,0},{-200.0/3,0,0},{-100.0/3,0,0},{0,0,0});
			const int second=parallel ? addRoad(network,{0,0,0},{100.0/3,0,0},{200.0/3,0,0},{100,0,0})
				: addRoad(network,{0,0,0},{0,0,100.0/3},{0,0,200.0/3},{0,0,100});
			const int route=network.addRoute(RoadRouteKind::CityRoute,U"Joined old lane",{first,second},0);
			context.expect(network.resolveIntersections(),U"Coincident perpendicular and collinear endpoints are joined");
			context.expect(network.getEdge(first) && network.getEdge(second),U"Joining endpoints preserves edge identities");
			context.expectEqual(network.getEdge(first)->nodeB,network.getEdge(second)->nodeA,U"Duplicate endpoints become one real node");
			context.expect(network.getRoute(route)->edgeIds==Array<int>{first,second},U"Endpoint joining retains the route edge order");
			context.expect(!network.resolveIntersections(),U"Coincident endpoint repair is stable");
		}
	});
	runner.add(U"RoadIntegrity.GradeSeparatedEndpoints", [](TestContext& context)
	{
		for (const double height : {-8.0,8.0})
		{
			RoadNetwork network;
			addRoad(network,{-100,0,0},{-100.0/3,0,0},{100.0/3,0,0},{100,0,0});
			const int grade=addRoad(network,{0,height,0},{0,height,100.0/3},{0,height,200.0/3},{0,height,100});
			network.getEdge(grade)->useElevation=true;
			context.expect(!network.resolveIntersections(),U"A bridge or tunnel endpoint above/below the host remains grade separated");
			context.expectEqual(network.getNode(network.getEdge(grade)->nodeA)->attachments.size(),size_t{1},U"Grade separation never creates a false junction");
		}
	});
	runner.add(U"RoadIntegrity.DesignedAcuteRouteSurvivesCleanup", [](TestContext& context)
	{
		RoadNetwork network;
		const int junction=network.addNode({0,0,0}),wideEnd=network.addNode({200,0,0}),oldEnd=network.addNode({180,0,80});
		const int wide=*network.addEdge(junction,wideEnd,{200.0/3,0,0},{400.0/3,0,0},RoadType::Arterial,4);
		const int lane=*network.addEdge(junction,oldEnd,{60,0,80.0/3},{120,0,160.0/3},RoadType::LocalRoad,2);
		network.getEdge(lane)->designGrade=true;
		const int route=network.addRoute(RoadRouteKind::CityRoute,U"Old access lane",{lane},0);
		const auto original=*network.getBezier(lane);
		network.fixSharpAngles(45.0f);
		context.expect(network.getEdge(wide) && network.getEdge(lane),U"Sharp-angle cleanup does not erase designed route geometry");
		context.expect(network.getRoute(route)->edgeIds==Array<int>{lane},U"The older route keeps its edge reference");
		if (const auto curve=network.getBezier(lane)) { context.expect(curve->p1==original.p1 && curve->p2==original.p2,U"Validated old-lane control points remain unchanged"); }
	});
	runner.add(U"RoadIntegrity.EndpointCapacityAndRouteOnlyGuard", [](TestContext& context)
	{
		RoadNetwork network;
		const int host=addRoad(network,{-100,0,0},{-100.0/3,0,0},{100.0/3,0,0},{100,0,0});
		const int center=network.addNode({0,0,0});
		for (int i=0;i<5;++i)
		{
			const Vec3 end{-80.0+40*i,0,100}; const int node=network.addNode(end);
			network.addEdge(center,node,end/3,end*2/3,RoadType::LocalRoad,2);
		}
		context.expect(!network.resolveIntersections(),U"A T split exceeding six arms is refused atomically");
		context.expect(network.getEdge(host) && network.getNode(center)->attachments.size()==5,U"Capacity refusal preserves the host and five original arms");
		RoadNetwork routed;
		const int start=routed.addNode({0,0,0}),wide=routed.addNode({200,0,0}),end=routed.addNode({180,0,80});
		routed.addEdge(start,wide,{200.0/3,0,0},{400.0/3,0,0},RoadType::Arterial,4);
		const int lane=*routed.addEdge(start,end,{60,0,80.0/3},{120,0,160.0/3},RoadType::LocalRoad,2);
		const int route=routed.addRoute(RoadRouteKind::CityRoute,U"Routed ordinary lane",{lane},0);
		routed.fixSharpAngles(45.0f);
		context.expect(routed.getEdge(lane) && routed.getRoute(route)->edgeIds==Array<int>{lane},U"Route identity alone protects an ordinary lane from destructive cleanup");
	});
	runner.add(U"RoadIntegrity.EndpointJoinPreservesMetadata", [](TestContext& context)
	{
		RoadNetwork network;
		const int first=addRoad(network,{-100,0,0},{-200.0/3,0,0},{-100.0/3,0,0},{0,0,0});
		const int second=addRoad(network,{0,0,0},{0,0,100.0/3},{0,0,200.0/3},{0,0,100});
		const int removed=network.getEdge(second)->nodeA,kept=network.getEdge(first)->nodeB;
		RoadSignPlacement sign; sign.nodeEndId=removed; network.getEdge(second)->signs << sign;
		RoadMarkingPlacement marking; marking.nodeId=removed; marking.nodeEndId=removed; marking.edgeId=second;
		network.addManualMarking(marking); network.addNamedDestination(removed,U"Old core",U"Old core",2);
		context.expect(network.resolveIntersections(),U"A metadata-bearing duplicate endpoint can be joined safely");
		context.expectEqual(network.getEdge(second)->signs.front().nodeEndId,kept,U"Road-sign endpoint references follow the merge");
		context.expectEqual(network.manualMarkings().front().nodeId,kept,U"Manual marking node follows the merge");
		context.expectEqual(network.manualMarkings().front().nodeEndId,kept,U"Manual marking end follows the merge");
		context.expectEqual(network.namedDestinations().front().nodeId,kept,U"Named destination follows the merge");
	});
	runner.add(U"RoadIntegrity.AmbiguousEndpointJoinsAreAtomic", [](TestContext& context)
	{
		RoadNetwork network;
		const int a=network.addNode({0,0,0}),b=network.addNode({0,0,0}),c=network.addNode({100,0,0});
		const int first=*network.addEdge(a,c,{100.0/3,0,0},{200.0/3,0,0},RoadType::LocalRoad,2);
		const int second=*network.addEdge(b,c,{100.0/3,0,30},{200.0/3,0,30},RoadType::LocalRoad,2);
		const int d=network.addNode({0,0,-100});
		const int probe=*network.addEdge(a,d,{0,0,-100.0/3},{0,0,-200.0/3},RoadType::LocalRoad,2);
		const int route=network.addRoute(RoadRouteKind::CityRoute,U"Keep both old arms",{first,second},0);
		Array<int> tracked{probe};
		context.expect(!network.resolveIntersections(0,&tracked),U"A duplicate common-neighbor corridor is refused");
		context.expect(network.getNode(a) && network.getNode(b) && network.getEdge(first) && network.getEdge(second),U"Refusal preserves both original endpoints and arms");
		context.expect(network.getRoute(route)->edgeIds==Array<int>{first,second} && tracked==Array<int>{probe},U"Refusal preserves route and ownership references");
		RoadNetwork bowed;
		const int host=addRoad(bowed,{-100,0,0},{-100.0/3,0,0},{100.0/3,0,0},{100,0,0});
		const int end=bowed.addNode({0,0,0}),far=bowed.addNode({0,0,100});
		const int old=*bowed.addEdge(bowed.getEdge(host)->nodeA,end,{-80,0,-40},{-20,0,-40},RoadType::LocalRoad,2);
		const int spur=*bowed.addEdge(end,far,{0,0,100.0/3},{0,0,200.0/3},RoadType::LocalRoad,2);
		const auto before=*bowed.getBezier(old);
		context.expect(!bowed.resolveIntersections(),U"A bowed old arm is not silently substituted for a straight host descendant");
		context.expect(bowed.getEdge(host) && bowed.getEdge(old) && bowed.getEdge(spur),U"Ambiguous host split changes no edge IDs");
		context.expect(bowed.getBezier(old)->p1==before.p1 && bowed.getBezier(old)->p2==before.p2,U"Refusal preserves the distinct old geometry");
	});
	runner.add(U"RoadIntegrity.CurveControlPolygonIsNotPavement", [](TestContext& context)
	{
		RoadNetwork network;
		addRoad(network,{0,0,0},{0,0,100},{100,0,100},{100,0,0});
		addRoad(network,{-10,0,90},{-3,0,90},{3,0,90},{10,0,90});
		context.expect(!network.resolveIntersections(),U"A road above the 75m Bezier apex does not intersect its control polygon");
	});
	runner.add(U"RoadIntegrity.GradeSeparatedCrossing", [](TestContext& context)
	{
		RoadNetwork network;
		addRoad(network,{-100,0,0},{-33,0,0},{33,0,0},{100,0,0});
		const int bridge = addRoad(network,{0,8,-100},{0,8,-33},{0,8,33},{0,8,100});
		network.getEdge(bridge)->useElevation = true;
		context.expect(!network.resolveIntersections(),U"An 8m bridge crossing does not create a ground intersection");
	});
	runner.add(U"RoadIntegrity.CrossingPreservesCurveAndProfile", [](TestContext& context)
	{
		RoadNetwork network;
		const int curved = addRoad(network,{0,0,0},{0,0,100},{100,0,100},{100,0,0});
		network.getEdge(curved)->speedLimit = 37;
		network.getEdge(curved)->parts.front().defId = U"integrity_custom_sidewalk";
		addRoad(network,{50,0,30},{50,0,60},{50,0,90},{50,0,120});
		context.expect(network.resolveIntersections(),U"The actual curve crossing is split");
		int junction = -1, customPieces = 0;
		for (const auto& node : network.nodes()) if (node.id >= 0 && node.attachments.size() == 4) junction = node.id;
		context.expect(junction >= 0,U"Four arms share the same intersection node");
		if (junction >= 0) context.expectNear(network.getNode(junction)->position.z,75.0,.01,U"Junction lies on the Bezier, not the endpoint average");
		for (const auto& edge : network.edges())
		{
			if (edge.id < 0 || edge.parts.front().defId != U"integrity_custom_sidewalk") continue;
			++customPieces;
			context.expectNear(edge.speedLimit,37,.001,U"Splitting retains road attributes");
			const auto curve = network.getBezier(edge.id);
			context.expect(Abs(curve->evaluate(.5f).z-56.25)<.01,U"Both child curves retain the original shape");
		}
		context.expectEqual(customPieces,2,U"Both pieces retain the custom road cross section");
	});
	runner.add(U"RoadIntegrity.DesignedCorridorsRejectApproximateConsolidation", [](TestContext& context)
	{
		RoadNetwork network;
		const int first=addRoad(network,{0,0,0},{100,0,0},{200,0,0},{300,0,0});
		const int second=addRoad(network,{0,0,2},{100,0,2},{200,0,2},{300,0,2});
		network.getEdge(first)->designGrade=true; network.getEdge(second)->designGrade=true;
		const int route=network.addRoute(RoadRouteKind::CityRoute,U"Validated older corridor",{second},0);
		const auto before=*network.getBezier(second);
		context.expectEqual(network.consolidateOverlappingRoads(),0,U"Approximate saved-corridor repair does not shift physically designed geometry");
		context.expect(network.getEdge(first) && network.getEdge(second),U"Designed road identities survive the repair pass");
		context.expect(network.getRoute(route)->edgeIds==Array<int>{second},U"Designed route identity is not fragmented");
		if (const auto after=network.getBezier(second)) { context.expect(before.p0==after->p0 && before.p1==after->p1 && before.p2==after->p2 && before.p3==after->p3,U"The validated curve is unchanged"); }
	});
	runner.add(U"RoadIntegrity.OverlappingCorridorsRetainBranchesAndRoutes", [](TestContext& context)
	{
		RoadNetwork network;
		const int a=network.addNode({600,0,0}), b=network.addNode({84,0,1.18}), c=network.addNode({0,0,0});
		const auto add=[&](int first,int last)
		{
			const Vec3 p=network.getNode(first)->position,q=network.getNode(last)->position;
			const int id=*network.addEdge(first,last,p+(q-p)/3,p+(q-p)*2/3,RoadType::Arterial,4);
			network.getEdge(id)->edgeState=EdgeState::Open;return id;
		};
		const int first=add(a,b),second=add(a,c);
		addRoad(network,{360,0,0},{280,0,0},{200,0,0},{120,0,0});
		const int branchNode=network.addNode({84,0,100});add(b,branchNode);
		const int route=network.addRoute(RoadRouteKind::Named,U"重複する幹線",{second},0);
		context.expect(network.consolidateOverlappingRoads()>0,U"Saved overlapping geometry is consolidated");
		double length=0;
		for (const auto& edge : network.edges()) if (edge.id>=0) length+=network.getBezier(edge.id)->totalLength;
		context.expect(length>690 && length<710,U"600m of shared road and its 100m branch each exist once");
		context.expect(network.getNode(branchNode) && !network.getNode(branchNode)->attachments.isEmpty(),U"The existing branch remains connected");
		const auto* savedRoute=network.getRoute(route);
		context.expect(savedRoute && !savedRoute->edgeIds.isEmpty(),U"The arterial route survives consolidation");
		if (savedRoute) for (const int id : savedRoute->edgeIds) context.expect(network.getEdge(id)!=nullptr,U"Route references valid replacement edges");
		context.expectEqual(network.consolidateOverlappingRoads(),0,U"A second load leaves repaired geometry unchanged");
		(void)first;
	});
	runner.add(U"RoadIntegrity.PlannedAndSeparateParallelRoadsStaySeparate", [](TestContext& context)
	{
		RoadNetwork network;
		addRoad(network,{0,0,0},{100,0,0},{200,0,0},{300,0,0});
		addRoad(network,{0,0,32},{100,0,32},{200,0,32},{300,0,32});
		const int plan=addRoad(network,{0,0,1},{100,0,1},{200,0,1},{300,0,1});
		network.getEdge(plan)->edgeState=EdgeState::Planned;
		context.expectEqual(network.consolidateOverlappingRoads(),0,U"Separate carriageways and construction plans are preserved");
	});

	runner.add(U"RoadIntegrity.SplitTaperKeepsSharedCrossSection", [](TestContext& context)
	{
		RoadNetwork network;
		const int id=addRoad(network,{0,0,0},{100,0,0},{200,0,0},{300,0,0});
		auto* edge=network.getEdge(id);
		for (auto& part : edge->parts) { part.offsetB_L*=2;part.offsetB_R*=2; }
		for (auto& lane : edge->lanes) { lane.offsetB_L*=2;lane.offsetB_R*=2; }
		const float expected=edge->parts.front().offsetA_L*1.25f;
		const int split=network.splitEdgeAtParameter(id,.25f);
		context.expect(split>=0,U"Tapered road splits successfully");
		if (split<0) return;
		for (const int childId : network.getNode(split)->edgeIds())
		{
			const auto* child=network.getEdge(childId);
			const float offset=child->nodeA==split?child->parts.front().offsetA_L:child->parts.front().offsetB_L;
			context.expectNear(offset,expected,.001,U"Both sides of the split use the same interpolated width");
		}
	});
	runner.add(U"RoadIntegrity.AcuteForkMouthsHaveSeparateEnvelopes", [](TestContext& context)
	{
		RoadNetwork network;
		const int center=network.addNode({0,0,0});Array<int> branches;
		for (const Vec3 end : {Vec3{-180,0,0},Vec3{180,0,0},Vec3{165,0,50}})
		{
			const int node=network.addNode(end);
			branches << *network.addEdge(center,node,end/3,end*2/3,RoadType::Arterial,2);
		}
		const auto* a=network.getEdge(branches[1]);const auto* b=network.getEdge(branches[2]);
		const Vec3 mouthA=network.getBezier(a->id)->positionAt(a->cutoffA),mouthB=network.getBezier(b->id)->positionAt(b->cutoffA);
		context.expect(mouthA.distanceFrom(mouthB)>a->totalWidth(),U"Full-width road strips start after the acute fork separates");
	});
	runner.add(U"RoadIntegrity.SplitRetainsRoadObjects", [](TestContext& context)
	{
		RoadNetwork network;
		const int edge=addRoad(network,{0,0,0},{100,0,0},{200,0,0},{300,0,0});
		RoadObject object;object.parentEdgeId=edge;object.arcPos=225;object.lateralOffset=2;
		const int id=network.addObject(object);
		context.expect(network.splitEdgeAtParameter(edge,.25f)>=0,U"Road is split without demolition");
		const auto* retained=network.getObject(id);
		context.expect(retained!=nullptr,U"Attached road object retains its ID");
		if (retained)
		{
			const auto curve=network.getBezier(retained->parentEdgeId);
			context.expect(curve.has_value(),U"Road object references a valid child edge");
			if (curve) context.expectNear(curve->positionAt(retained->arcPos).x,225,.01,U"Object remains at the same world position after splitting");
		}
	});
	runner.add(U"RoadIntegrity.ShortOverlappingEndpoints", [](TestContext& context)
	{
		RoadNetwork network;
		addRoad(network,{412.35,6.09,56.36},{416.587,6.087,30.652},{420.825,6.083,4.949},{425.062,6.079,-20.758});
		addRoad(network,{420,6.187,20},{420,6.314,32.621},{420,6.441,45.242},{420,6.568,57.859});
		context.expect(network.consolidateOverlappingRoads()>0,U"A short road ending within another pavement is connected");
		RoadNetwork shortRoads;
		const int center=shortRoads.addNode({52,0,12});
		for (const Vec3 end : {Vec3{59.94,0,16.48},Vec3{60.09,0,17.01}})
		{
			const int node=shortRoads.addNode(end);const Vec3 start=shortRoads.getNode(center)->position;
			const int id=*shortRoads.addEdge(center,node,start+(end-start)/3,start+(end-start)*2/3,RoadType::Arterial,2);
			shortRoads.getEdge(id)->edgeState=EdgeState::Open;
		}
		context.expect(shortRoads.consolidateOverlappingRoads()>0,U"Coincident endpoints are joined even on roads shorter than 12m");
		int active=0;for (const auto& edge : shortRoads.edges()) active+=edge.id>=0;
		context.expectEqual(active,1,U"The short duplicate road is removed without removing the shared road");
	});
	runner.add(U"RoadIntegrity.MixedGradeJunctionVisibility", [](TestContext& context)
	{
		const FilePath directory = FileSystem::CurrentDirectory();
		struct Restore { FilePath path; ~Restore(){ FileSystem::ChangeCurrentDirectory(path); } } restore{directory};
		FileSystem::ChangeCurrentDirectory(directory+U"../../App/");
		World world;
		world.reserveChunks();
		world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		for (int z=31;z<=32;++z) for (int x=31;x<=32;++x)
		{
			Grid<float> heights(HEIGHT_CELLS+1,HEIGHT_CELLS+1,72.0f);
			world.installChunkDirect({x,z},HeightMapResult{heights,72,72});
		}
		RoadNetwork network;
		const Vec3 focus{32768,70,32768};
		const int center = network.addNode(focus);
		for (const Vec3 delta : {Vec3{-180,0,0},Vec3{180,10,0},Vec3{0,0,180}})
		{
			const int end = network.addNode(focus+delta);
			const int id = *network.addEdge(center,end,focus+delta/3,focus+delta*2/3,RoadType::Arterial,4);
			network.getEdge(id)->edgeState = EdgeState::Open;
			network.getEdge(id)->useElevation = delta.y>0;
		}
		world.update(focus);
		WorldRenderer terrain;
		terrain.setAsyncTerrain(false);
		RoadRenderer roads;
		context.expect(roads.loadAssets(),U"Road assets are available");
		const Size size{640,480};
		const BasicCamera3D camera{size,40_deg,focus+Vec3{0,100,-.1},focus};
		const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};
		Array<Image> captures;
		for (int pass=0;pass<2;++pass)
		{
			{
				const ScopedRenderTarget3D rt{target.clear(ColorF{.05,.35,.7})};
				const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
				Graphics3D::SetCameraTransform(camera);
				Graphics3D::SetSunDirection(Vec3{1,2,-1}.normalized());
				Graphics3D::SetGlobalAmbientColor(ColorF{.5});
				if (pass) terrain.render(world,network,camera);
				roads.render(network,world,ViewFrustum{camera,24000},camera.getEyePosition());
			}
			Graphics3D::Flush();
			Image capture;target.readAsImage(capture);captures << std::move(capture);
		}
		int checked=0, matched=0;
		for (int z=-3;z<=3;++z) for (int x=-6;x<=6;++x)
		{
			const auto projected=camera.worldToScreenPoint(Float3{focus+Vec3{static_cast<double>(x),2.0,static_cast<double>(z)}});
			const Point pixel{static_cast<int>(Round(projected.x)),static_cast<int>(Round(projected.y))};
			const Color a=captures[0][pixel],b=captures[1][pixel];
			++checked;matched += Abs(static_cast<int>(a.r)-b.r)+Abs(static_cast<int>(a.g)-b.g)+Abs(static_cast<int>(a.b)-b.b)<12;
		}
		TextWriter{directory+U"TestResults/road_integrity_visibility.txt"}.write(U"visible={}/{}"_fmt(matched,checked));
		context.expect(matched>=checked*.95,U"Terrain does not hide the junction where a bridge meets ground roads");
	});
}
