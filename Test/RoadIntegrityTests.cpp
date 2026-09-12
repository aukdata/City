#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/road/RoadNetwork.hpp"
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
