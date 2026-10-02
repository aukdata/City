#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/road/RoadPlanDraft.hpp"
#include "src/ui/RoadPlanToolbar.hpp"
#include "src/ui/RoadPlanInput.hpp"
#include "src/world/World.hpp"

namespace
{
	/// @brief Deterministic hardware-frame replay through the same input handler as GameScene.
	struct RoadPlanReplay
	{
		RoadPlanDraft draft;
		Optional<size_t> draggedPoint;
		Array<Vec3> dragPoints;

		RoadPlanInput::Result input(const RoadPlanInput::Frame& frame, bool enabled = true, bool returnEarly = false)
		{
			RoadPlanInput::Result result = RoadPlanInput::Result::None;
			RoadPlanInput::dispatchFrame(enabled, frame.pressed, draggedPoint, dragPoints, [&]
			{
				if (returnEarly)
				{
					return;
				}
				result = RoadPlanInput::update(draft, draggedPoint, dragPoints, frame,
					[](Vec3 point) { return Vec2{point.x,point.z}; });
			});
			return result;
		}

		RoadPlanInput::Result pointer(Vec3 point, bool down, bool pressed, bool overPanel = false)
		{
			return input({overPanel ? none : Optional<Vec3>{point}, {point.x,point.z}, down, pressed});
		}

		void placeEndpoints(Vec3 start, Vec3 end)
		{
			pointer(start, true, true);
			pointer(start, false, false);
			pointer(end, true, true);
			pointer(end, false, false);
		}
	};
}

void registerRoadPlanUxTests(TestRunner& runner)
{
	runner.add(U"RoadPlan.SkippedReleaseCancelsDragReplay", [](TestContext& context)
	{
		// Disabled dispatch models the minimap; early return models HUD/header/text input gates.
		for (const bool returnEarly : {false,true})
		{
			RoadPlanReplay replay;
			const Vec3 start{100,7,100}, end{200,7,100}, moved{250,7,180};
			replay.placeEndpoints(start, end);
			replay.pointer(end, true, true);
			replay.pointer(moved, false, true);
			replay.input({none, {}, false, false}, returnEarly, returnEarly);
			context.expect(!replay.draggedPoint && replay.dragPoints.isEmpty(),
				U"A release skipped by scene/UI input gates must cancel the pending drag");
			replay.pointer({350,7,240}, false, false);
			context.expect(replay.draft.points() == Array<Vec3>{start,end},
				U"Returning to ground after a skipped release must not commit at the new cursor location");
			replay.input({none, {}, false, false, RoadPlanInput::History::Undo});
			context.expect(replay.draft.points() == Array<Vec3>{start}, U"A skipped-release cancellation creates no history entry");
		}
	});
	runner.add(U"RoadPlan.SkippedHeldFramePreservesDragReplay", [](TestContext& context)
	{
		for (const bool returnEarly : {false,true})
		{
			RoadPlanReplay replay;
			const Vec3 start{100,7,100}, end{200,7,100}, moved{250,7,180};
			replay.placeEndpoints(start, end);
			replay.pointer(end, true, true);
			replay.pointer(moved, false, true);
			replay.input({none, {}, false, true}, returnEarly, returnEarly);
			context.expect(replay.draggedPoint && replay.dragPoints.size() == 2 && replay.dragPoints.back() == moved,
				U"A held pointer crossing a blocked surface keeps its captured endpoint");
			replay.pointer({300,7,210}, false, true);
			replay.pointer({310,7,220}, false, false);
			context.expect(replay.draft.points() == Array<Vec3>{start,{310,7,220}},
				U"Returning to ground before release continues and commits the original drag");
		}
	});
	runner.add(U"RoadPlan.EndpointPressHoldMoveReleaseReplay", [](TestContext& context)
	{
		RoadPlanReplay replay;
		const Vec3 start{100,7,100}, end{200,7,100}, moved{260,7,180}, released{280,7,190};
		replay.placeEndpoints(start, end);
		context.expectEqual(replay.draft.points().size(), 2, U"Two clicks place the draft endpoints");
		context.expect(!replay.draft.generated(), U"Endpoint dragging does not require Generate");
		replay.pointer(end, true, true);
		context.expect(replay.draggedPoint && *replay.draggedPoint == 1, U"Press hit-tests and captures the endpoint");
		replay.pointer(end, false, true);
		replay.pointer(moved, false, true);
		context.expect(replay.draft.points().back() == end, U"Held movement leaves committed points unchanged");
		context.expect(replay.dragPoints.size() == 2 && replay.dragPoints.back() == moved, U"Held movement updates the visible tentative endpoint");
		context.expect(replay.pointer(released, false, false) == RoadPlanInput::Result::Changed,
			U"Release commits the final cursor position and requests the scene rebuild");
		context.expect(replay.draft.points().back() == released, U"Release uses its own position, not the previous held frame");
		context.expect(!replay.draggedPoint && replay.dragPoints.isEmpty(), U"Release clears pointer capture and tentative geometry");
		replay.pointer({320,7,220}, false, false);
		context.expect(replay.draft.points().back() == released, U"A later idle cursor move cannot continue the drag");
		replay.input({none, {}, false, false, RoadPlanInput::History::Undo});
		context.expect(replay.draft.points() == Array<Vec3>{start,end}, U"One undo restores the entire drag, not an intermediate held frame");
		replay.input({none, {}, false, false, RoadPlanInput::History::Redo});
		context.expect(replay.draft.points() == Array<Vec3>{start,released}, U"One redo restores the released endpoint");
		replay.pointer(start, true, true);
		context.expect(replay.draggedPoint && *replay.draggedPoint == 0, U"The starting endpoint is draggable too");
		replay.pointer({80,7,130}, false, true);
		replay.pointer({80,7,130}, false, false);
		context.expect(replay.draft.points().front() == Vec3{80,7,130}, U"Start-point movement uses the same release path");
	});
	runner.add(U"RoadPlan.PanelReleaseCancelsDragReplay", [](TestContext& context)
	{
		RoadPlanReplay replay;
		const Vec3 start{100,7,100}, end{200,7,100}, moved{250,7,180};
		replay.placeEndpoints(start, end);
		replay.pointer(end, true, true);
		replay.pointer(moved, false, true);
		replay.pointer({600,7,400}, false, true, true);
		context.expect(replay.draggedPoint && replay.dragPoints.back() == moved, U"Crossing a panel while held keeps the last ground position");
		replay.pointer({270,7,190}, false, true);
		context.expect(replay.dragPoints.back() == Vec3{270,7,190}, U"Returning to ground while held resumes the same drag");
		replay.pointer({600,7,400}, false, false, true);
		context.expect(!replay.draggedPoint && replay.dragPoints.isEmpty(), U"Release over a panel cancels pointer capture");
		context.expect(replay.draft.points() == Array<Vec3>{start,end}, U"Panel release discards tentative changes");
		replay.pointer({300,7,200}, false, false);
		context.expect(replay.draft.points().back() == end, U"Returning to ground after cancellation cannot commit a stale drag");
		replay.pointer(end, true, true, true);
		context.expect(!replay.draggedPoint, U"A press over a panel cannot start an endpoint drag");
		replay.input({none, {}, false, false, RoadPlanInput::History::Undo});
		context.expect(replay.draft.points() == Array<Vec3>{start}, U"A cancelled drag creates no undo entry");
	});
	runner.add(U"RoadPlan.UndoDuringHeldDragReplay", [](TestContext& context)
	{
		RoadPlanReplay replay;
		const Vec3 start{100,7,100}, end{200,7,100}, moved{250,7,180};
		replay.placeEndpoints(start, end);
		replay.pointer(end, true, true);
		replay.pointer(moved, false, true);
		replay.input({moved, {moved.x,moved.z}, false, true, RoadPlanInput::History::Undo});
		context.expect(!replay.draggedPoint && replay.dragPoints.isEmpty(), U"Undo cancels an in-progress drag before editing history");
		context.expect(replay.draft.points() == Array<Vec3>{start}, U"Undo targets the last committed action, not tentative movement");
		replay.pointer(moved, false, true);
		replay.pointer(moved, false, false);
		context.expect(replay.draft.points() == Array<Vec3>{start}, U"Remaining hold and release cannot recreate the cancelled endpoint");
		replay.input({none, {}, false, false, RoadPlanInput::History::Redo});
		context.expect(replay.draft.points() == Array<Vec3>{start,end}, U"Redo restores only the committed endpoint placement");
	});
	runner.add(U"RoadPlan.UnchangedAndInvalidDragReplay", [](TestContext& context)
	{
		RoadPlanReplay replay;
		const Vec3 start{100,7,100}, end{200,7,100};
		replay.placeEndpoints(start, end);
		replay.pointer(end, true, false);
		context.expect(!replay.draggedPoint, U"A press and release sampled together cannot leave a latched drag");
		replay.pointer({280,7,190}, false, false);
		context.expect(replay.draft.points() == Array<Vec3>{start,end}, U"A same-frame click cannot move the endpoint on a later idle frame");
		replay.pointer(end, true, true);
		replay.pointer(end, false, true);
		context.expect(replay.pointer(end, false, false) == RoadPlanInput::Result::None, U"Clicking an endpoint without movement creates no revision");
		replay.pointer(end, true, true);
		replay.pointer({101,7,100}, false, true);
		context.expect(replay.pointer({101,7,100}, false, false) == RoadPlanInput::Result::None, U"A drag shorter than the minimum segment is rejected");
		context.expect(replay.draft.points() == Array<Vec3>{start,end}, U"Rejected drag retains the original geometry");
		context.expect(!replay.draggedPoint && replay.dragPoints.isEmpty(), U"Rejected drag still releases pointer capture");
		replay.input({none, {}, false, false, RoadPlanInput::History::Undo});
		context.expect(replay.draft.points() == Array<Vec3>{start}, U"Unchanged and invalid drags create no undo entries");
	});
	runner.add(U"RoadPlan.ScreenHandleHitReplay", [](TestContext& context)
	{
		RoadPlanReplay replay;
		const Vec3 start{100,7,100}, end{200,7,100};
		replay.placeEndpoints(start, end);
		replay.input({end, {212.1,100}, true, true});
		context.expect(!replay.draggedPoint, U"A press outside the 12-pixel handle radius cannot capture it");
		context.expectEqual(replay.draft.points().size(), 2, U"A missed handle cannot create a third endpoint");
		replay.pointer(end, false, false);
		replay.input({Vec3{500,7,500}, {211.9,100}, true, true});
		context.expect(replay.draggedPoint && *replay.draggedPoint == 1, U"Handle picking uses screen distance rather than the ground-ray distance");
		replay.pointer(end, false, false, true);
		context.expect(!replay.draggedPoint, U"Panel release also cancels a handle picked near its boundary");
	});

	runner.add(U"RoadPlan.ContinuousPlacementUndoRedo", [](TestContext& context)
	{
		RoadPlanDraft draft;
		draft.place({100,7,100}); draft.place({200,7,100}); draft.place({200,7,200});
		context.expectEqual(draft.points().size(),3,U"Third click extends the road instead of moving its end");
		draft.undo(); draft.redo();
		context.expect(draft.points().back() == Vec3{200,7,200},U"Redo restores the last segment");
		draft.place({250,7,200},true); draft.undo();
		context.expect(draft.points().back() == Vec3{200,7,200},U"Endpoint adjustment is undoable");
		draft.place({200,7,300});
		context.expect(!draft.canRedo(),U"New placement clears the abandoned redo branch");
		context.expect(!draft.place({200.5,7,300}),U"Accidental double click cannot create a tiny segment");
		const Vec3 constrained = RoadPlanDraft::constrainAngle({0,0,0},{100,0,91});
		context.expectNear(constrained.x,constrained.z,0.001,U"Shift alignment follows 45 degree directions");
	});
	runner.add(U"RoadPlan.ShortRoadAndTransactionalFailure", [](TestContext& context)
	{
		World world;
		RoadEdge road;
		road.lanes = RoadNetwork::buildDefaultLanes(2,RoadType::LocalRoad);
		RoadNetwork::buildDefaultParts(road);
		RoadNetwork network;
		RoadPlanDraft draft;
		draft.place({100,7,100}); draft.place({110,7,100});
		context.expect(draft.rebuild(world,road),U"10m roads are valid even within one pathfinder cell");
		context.expectNear(draft.length(),10,0.01,U"Preview measures the actual short road");
		context.expectEqual(network.nodes().size(),0,U"Preview does not modify the live network");
		const auto ids = draft.apply(network,world,road);
		context.expectEqual(ids.size(),1,U"A valid plan is added on save");
		const int nextNode = network.nextNodeId(), nextEdge = network.nextEdgeId();
		context.expect(draft.apply(network,world,road).isEmpty(),U"An overlapping duplicate fails instead of overwriting the road");
		context.expectEqual(network.nextNodeId(),nextNode,U"Failed save rolls back node allocations");
		context.expectEqual(network.nextEdgeId(),nextEdge,U"Failed save rolls back edge allocations");
		draft.clear();
		context.expectEqual(network.edges().size(),1,U"Discard cannot delete a saved road");
	});
	runner.add(U"RoadPlan.SnapLongEdgeAndHeight", [](TestContext& context)
	{
		RoadNetwork network;
		const int a = network.addNode({0,7,0}), b = network.addNode({2000,7,0});
		network.addEdge(a,b,{2000.0/3,7,0},{4000.0/3,7,0});
		RoadPlanSnapIndex index;
		index.rebuild(network);
		const auto hit = index.find(network,{953,7,5});
		context.expect(hit.connected && !hit.node,U"Snap finds the middle of a long edge between coarse samples");
		context.expectNear(hit.position.x,953,0.1,U"Snap position is refined on the curve");
		context.expectNear(hit.position.z,0,0.01,U"Snap lies on the road center");
		context.expect(!index.find(network,{953,30,5}).connected,U"A road on a different level is not a connection candidate");
		context.expect(index.find(network,{3,7,3}).node,U"Existing junctions take priority");
		const auto precise = network.findEdgeNearDetailed(hit.position,0.5f);
		context.expect(precise.has_value(),U"Commit resolver also finds the exact point on a long edge");
		World world;
		RoadPlanDraft draft;
		draft.place(hit.position); draft.place(hit.position+Vec3{0,0,35});
		const RoadEdge road=RoadPlanDraft::makeRoadTemplate(1);
		draft.rebuild(world,road);
		const auto added=draft.apply(network,world,road);
		context.expectEqual(added.size(),1,U"A branch can be saved onto the displayed snap point");
		if (!added.isEmpty())
		{
			const auto* edge=network.getEdge(added.front());
			context.expectEqual(network.getNode(edge->nodeA)->attachments.size(),3,U"Saving makes a connected T junction, not a disconnected nearby node");
		}
		index.rebuild(network);
		Stopwatch timer{StartImmediately::Yes};
		for (int i=0;i<1000;++i) { index.find(network,{static_cast<double>(i),7,5}); }
		TextWriter{U"TestResults/road_snap_timing.txt"}.write(U"1000 local snap queries: {:.3f}ms"_fmt(timer.msF()));
	});
	runner.add(U"RoadPlan.JoinPreservesExistingRouteAndPlan", [](TestContext& context)
	{
		RoadNetwork network;
		Array<int> nodes, edges;
		for (int i=0;i<4;++i) { nodes << network.addNode({100.0+i*100,7,100}); }
		for (int i=0;i<3;++i)
		{
			const Vec3 a=network.getNode(nodes[i])->position, b=network.getNode(nodes[i+1])->position;
			edges << *network.addEdge(nodes[i],nodes[i+1],a+(b-a)/3,a+(b-a)*2/3);
		}
		const int route=network.addRoute(RoadRouteKind::Named,U"既存通り",edges,0);
		RoadPlan plan; plan.name=U"既存計画"; plan.edgeIds=edges; plan.routeId=route;
		const int planId=network.addPlan(plan);
		World world;
		RoadPlanDraft draft;
		draft.place({250,7,100}); draft.place({250,7,145});
		const RoadEdge road=RoadPlanDraft::makeRoadTemplate(0);
		draft.rebuild(world,road);
		context.expect(!draft.apply(network,world,road).isEmpty(),U"Branch is added at the middle of the named road");
		const auto* savedRoute=network.getRoute(route);
		const auto* savedPlan=network.getPlan(planId);
		context.expect(savedRoute && savedRoute->edgeIds.size()==4,U"Connecting a branch preserves the original route and ordered split segments");
		context.expect(savedPlan && savedPlan->edgeIds.size()==4,U"Connecting a branch preserves the existing construction plan");
		if (savedPlan) { context.expectNear(savedPlan->totalLength,300,0.1,U"Connection does not shorten the original plan"); }
		const int reversed=network.addRoute(RoadRouteKind::Named,U"逆方向の通り",{edges[2],savedRoute->edgeIds[2]},0);
		const auto* reverseEdge=network.getEdge(edges[2]);
		const int midpoint=network.splitEdgeAt(edges[2],reverseEdge->length*0.5f);
		const auto* reversedRoute=network.getRoute(reversed);
		context.expect(midpoint>=0 && reversedRoute && reversedRoute->edgeIds.size()==3,U"Reversed route survives a split at its first edge");
		if (reversedRoute && reversedRoute->edgeIds.size()==3)
		{
			context.expect(network.getEdge(reversedRoute->edgeIds.front())->nodeB==nodes.back(),U"Split children retain the direction of route traversal");
		}
		RoadNetwork single;
		const int a=single.addNode({0,7,0}), b=single.addNode({100,7,0});
		const auto only=single.addEdge(a,b,{33,7,0},{67,7,0});
		const int singleRoute=single.addRoute(RoadRouteKind::Named,U"短い通り",{*only},0);
		single.splitEdgeAt(*only,50);
		context.expect(single.getRoute(singleRoute) && single.getRoute(singleRoute)->edgeIds.size()==2,U"A single-edge named route is not deleted during connection");
	});
	runner.add(U"RoadPlan.HierarchyPresets", [](TestContext& context)
	{
		for (int preset=0;preset<4;++preset)
		{
			const RoadEdge road=RoadPlanDraft::makeRoadTemplate(preset);
			context.expectEqual(road.lanes.size(),preset==2 ? 4 : (preset==3 ? 1 : 2),U"Road choice controls the actual lane count");
			context.expect(road.totalWidth()>0,U"Every choice has a matching cross section");
			if (preset==3) { context.expect(road.lanes.front().dir==LaneDir::Forward,U"One-way runs from the first click toward the last"); }
			if (preset==0) { context.expect(road.lanes.front().lineRight==LineType::None,U"Local road has no center line"); }
		}
	});
	runner.add(U"RoadPlan.ToolbarVisualReview", [](TestContext& context)
	{
		const Size size{1182,480};
		RenderTexture target{size,ColorF{0.05,0.07,0.09}};
		Font font{FontMethod::MSDF,14}, bold{FontMethod::MSDF,14,Typeface::Bold};
		{
			const ScopedRenderTarget2D scope{target};
			for (int i=0;i<3;++i)
			{
				const Transformer2D transform{Mat3x2::Translate(12+i*390,12)};
				Rect{0,0,374,450}.draw(ColorF{0.09,0.11,0.14});
				RoadPlanToolbar::State state;
				if (i>0) { state.points=3; state.generated=true; state.preset=2; state.valid=true; state.canUndo=true; state.length=1280; state.cost=1.92; state.constructionSeconds=154; state.width=24.28; state.funds=30; }
				if (i==2) { state.valid=false; state.canRedo=true; state.error=true; state.message=U"接続できません。終点を調整してください"; }
				RoadPlanToolbar::draw(font,bold,364,state);
				font(i==0 ? U"開始前" : (i==1 ? U"計画の確認" : U"失敗からの修正")).draw(8,432,ColorF{0.7});
			}
		}
		Graphics2D::Flush();
		Image screenshot;
		target.readAsImage(screenshot);
		context.expect(screenshot.save(U"Screenshot/road_plan_toolbar.png"),U"Save the actual shared toolbar for review before integrating it into the game");
	});
}
