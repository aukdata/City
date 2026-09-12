#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/road/RoadPlanDraft.hpp"
#include "src/ui/RoadPlanToolbar.hpp"
#include "src/world/World.hpp"

void registerRoadPlanUxTests(TestRunner& runner)
{
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
		context.expect(draft.rebuild(world,road,false),U"10m roads are valid even within one pathfinder cell");
		context.expectNear(draft.length(),10,0.01,U"Preview measures the actual short road");
		context.expectEqual(network.nodes().size(),0,U"Preview does not modify the live network");
		const auto ids = draft.apply(network,world,road,false);
		context.expectEqual(ids.size(),1,U"A valid plan is added on save");
		const int nextNode = network.nextNodeId(), nextEdge = network.nextEdgeId();
		context.expect(draft.apply(network,world,road,false).isEmpty(),U"An overlapping duplicate fails instead of overwriting the road");
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
		draft.rebuild(world,road,false);
		const auto added=draft.apply(network,world,road,false);
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
		draft.rebuild(world,road,false);
		context.expect(!draft.apply(network,world,road,false).isEmpty(),U"Branch is added at the middle of the named road");
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
				if (i>0) { state.points=3; state.preset=2; state.valid=true; state.canUndo=true; state.length=1280; state.cost=1.92; state.days=77; state.width=24.28; state.funds=30; }
				if (i==2) { state.valid=false; state.canRedo=true; state.error=true; state.message=U"接続できません。終点を調整してください"; }
				RoadPlanToolbar::draw(font,bold,364,state);
				font(i==0 ? U"開始前" : (i==1 ? U"計画の確認" : U"失敗からの修正")).draw(8,425,ColorF{0.7});
			}
		}
		Graphics2D::Flush();
		Image screenshot;
		target.readAsImage(screenshot);
		context.expect(screenshot.save(U"Screenshot/road_plan_toolbar.png"),U"Save the actual shared toolbar for review before integrating it into the game");
	});
}
