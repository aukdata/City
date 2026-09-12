#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/gen/DistrictRoads.hpp"
#include "src/gen/SettlementPlan.hpp"
#include "src/gen/StreetProfile.hpp"
#include "src/gen/WaterCrossings.hpp"
#include "src/gen/StreetBlocks.hpp"
#include "src/railway/TrainManager.hpp"
#include "src/road/SignArtwork.hpp"
#include "src/road/RoadSign.hpp"

void registerUrbanMorphologyTests(TestRunner& runner)
{
	using namespace UrbanMorphology;
	runner.add(U"Morphology.StreetCrossSections",[](TestContext& context)
	{
		using namespace GeneratedStreet;
		TextWriter report{U"TestResults/street_profiles.txt"};
		for (int role=0;role<8;++role)
		{
			RoadEdge road; const auto profile=describe(static_cast<Role>(role)); apply(road,profile);
			int walkways=0, markings=0;
			float pavement=0;
			for (const auto& part : road.parts)
			{
				if (part.type==RoadPartType::Sidewalk) { ++walkways; }
				if (part.type==RoadPartType::Roadbed) { pavement+=part.offsetA_R-part.offsetA_L; }
				context.expect(part.offsetA_R>=part.offsetA_L,U"Road parts have positive cross-section widths");
			}
			for (const auto& lane : road.lanes)
			{
				markings+=(lane.lineLeft!=LineType::None)+(lane.lineRight!=LineType::None);
				context.expect(lane.offsetA_L>=-pavement*.5f-.01f && lane.offsetA_R<=pavement*.5f+.01f,U"Vehicle paths fit the actual asphalt surface");
			}
			if (role==static_cast<int>(Role::FarmAccess) || role==static_cast<int>(Role::Village) || role==static_cast<int>(Role::Local))
			{
				context.expect(walkways==0 && markings==0 && pavement<=5.5f,U"Minor access roads are narrow, undivided, and have no raised sidewalks");
			}
			if (role==static_cast<int>(Role::MainArterial)) { context.expect(road.lanes.size()==4 && walkways==2 && pavement>=12,U"Major urban roads carry four traffic lanes and two walkways"); }
			if (role==static_cast<int>(Role::Regional)) { context.expect(road.lanes.size()==2 && walkways==0 && markings>0,U"Regional routes have two marked lanes and shoulders rather than continuous city sidewalks"); }
			if (role==static_cast<int>(Role::Collector)) { context.expect(road.lanes.size()==2 && walkways==2,U"Urban collectors combine two lanes with pedestrian access"); }
			report<<U"role={} logicalPaths={} asphaltM={} sidewalks={} markings={} speed={}"_fmt(role,road.lanes.size(),pavement,walkways,markings,road.speedLimit);
		}
		RoadEdge oneWay; apply(oneWay,describe(Role::OneWay),true);
		context.expect(oneWay.lanes.size()==1 && oneWay.lanes.front().dir==LaneDir::Backward,U"A reverse one-way street has one actual directed traffic path");
	});
	runner.add(U"Morphology.TerrainConstrainedTown",[](TestContext& context)
	{
		World world; world.reserveChunks(); world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		for (int z=29;z<=34;++z)
		{
			for (int x=29;x<=34;++x)
			{
				Grid<float> heights(HEIGHT_CELLS+1,HEIGHT_CELLS+1);
				for (int row=0;row<=HEIGHT_CELLS;++row)
				{
					for (int col=0;col<=HEIGHT_CELLS;++col)
					{
						const double distance=Max(Abs(x*CHUNK_SIZE+col*16.0-32768),Abs(z*CHUNK_SIZE+row*16.0-32768));
						heights[{col,row}]=static_cast<float>(20+Max(0.0,distance-380)*.30);
					}
				}
				world.installChunkDirect(Point{x,z},HeightMapResult{heights,20,1000});
			}
		}
		MapGenerator::Settlement settlement; settlement.center={32768,32768}; settlement.kind=MapGenerator::SettlementKind::RegionalCity;
		settlement.plan=makePlan(Origin::Castle,0,Site{},42,true);
		RoadNetwork network; DistrictRoads::KaidoSegment kaido; kaido.passesThrough=true; kaido.dirAtCenter={1,0};
		DistrictRoads::generateSettlement(42,0,settlement,kaido,world,network);
		context.expect(settlement.plan.halfExtent.x<400 && !settlement.plan.frontageRoads,U"Town footprint contracts to the measured buildable plateau");
		for (const auto& edge : network.edges())
		{
			if (edge.id<0) { continue; }
			const auto curve=network.getBezier(edge.id);
			for (int sample=0;sample<=10;++sample)
			{
				const Vec3 p=curve->evaluate(sample*.1f);
				context.expect(Abs(p.x-32768)<400 && Abs(p.z-32768)<400,U"No city grid is cut into the steep surrounding terrain");
			}
		}
	});
	runner.add(U"Morphology.TerrainAndOrigin",[](TestContext& context)
	{
		const auto flat=inspectSite(Vec2{0,0},[](const Vec2&) { return 20.0; });
		const auto coastal=inspectSite(Vec2{0,0},[](const Vec2& p) { return p.y>320 ? -2.0 : 20.0; });
		context.expect(Abs(coastal.shoreDistance-320)<0.2,U"Waterfront distance is measured from terrain");
		context.expect(coastal.shoreDirection.dot(Vec2{0,1})>0.99,U"Waterfront orientation is measured");
		HashSet<int> cityOrigins, townOrigins;
		for (uint64 seed=0;seed<100;++seed)
		{
			cityOrigins.insert(static_cast<int>(chooseOrigin(0,flat,seed,false)));
			townOrigins.insert(static_cast<int>(chooseOrigin(1,flat,seed,true)));
			context.expect(chooseOrigin(0,flat,seed,false)!=Origin::Port,U"Inland terrain cannot produce a port");
			context.expect(chooseOrigin(1,coastal,seed,true)==Origin::Port,U"A suitable shoreline supplies the port opportunity");
		}
		context.expect(cityOrigins.size()>=3 && townOrigins.size()>=4,U"Settlement scale is independent of historical origin");
	});
	runner.add(U"Morphology.NucleiAndLandUse",[](TestContext& context)
	{
		Site site; site.shoreDistance=400;
		const auto castle=makePlan(Origin::Castle,0,site,42,true);
		context.expect(castle.station && castle.station->distanceFrom(castle.oldCore)>600,U"Station and old town are distinct growth nuclei");
		context.expect(sample(castle,*castle.station).district==District::Station,U"Station land use agrees with the real station anchor");
		context.expect(sample(castle,castle.oldCore).district==District::OldTown,U"Historic retail nucleus survives station growth");
		context.expect(sample(castle,castle.civic->center()).district==District::Civic,U"Historic civic land is reserved");
		const auto port=makePlan(Origin::Port,0,site,42,true);
		context.expect(port.halfExtent.x>port.halfExtent.y*2,U"Waterfront settlement grows along the shore");
		context.expect(sample(port,{0,port.halfExtent.y*.8}).district==District::Industry,U"Waterfront logistics occupy the water side");
		const auto post=makePlan(Origin::Post,1,site,42,false);
		context.expect(post.halfExtent.x>post.halfExtent.y*2.5,U"A post town follows its trade route");
		context.expect(sample(post,{0,0}).frontage<sample(castle,{0,700}).frontage,U"Merchant plots have narrower frontages than residential quarters");
		context.expect(sample(castle,{1200,1200}).district==District::Countryside,U"The urban footprint and countryside are distinct");
	});
	runner.add(U"Morphology.RuralForms",[](TestContext& context)
	{
		Site flat; Site valley; valley.relief=80;
		HashSet<int> forms;
		for (uint64 seed=0;seed<16;++seed)
		{
			forms.insert(static_cast<int>(makePlan(Origin::Rural,2,flat,seed,false).ruralForm));
			context.expect(makePlan(Origin::Rural,2,valley,seed,false).ruralForm==RuralForm::Valley,U"Relief selects a valley settlement rather than a broad field grid");
		}
		context.expectEqual(forms.size(),size_t{2},U"Flat farmland supports both clustered and dispersed settlement");
	});
	runner.add(U"Morphology.DuplicateCurveParameterization",[](TestContext& context)
	{
		RoadNetwork network;
		const int a=network.addNode({0,151.55,0}),b=network.addNode({9.8,153.75,-32.81});
		const int first=*network.addEdge(a,b,{-6.41,151.13,2.14},{1.99,152.51,-15.03},RoadType::Arterial,2);
		GeneratedStreet::apply(*network.getEdge(first),GeneratedStreet::describe(GeneratedStreet::Role::Regional));
		// addEdge rejects duplicate endpoints. Reproduce a post-split/imported edge
		// with addEdgeRaw, the same storage path used when restoring a network.
		RoadEdge duplicate=*network.getEdge(first);
		const int second=first+1;
		duplicate.id=second; duplicate.ctrlA={3.27,151.85,-10.94}; duplicate.ctrlB={6.54,153.10,-21.87};
		GeneratedStreet::apply(duplicate,GeneratedStreet::describe(GeneratedStreet::Role::Local));
		network.addEdgeRaw(duplicate);
		const int route=network.addRoute(RoadRouteKind::NationalRoute,U"集落街道",{second},151);
		context.expect(network.removeDuplicateEdges(42),U"The same physical corridor is consolidated even when its curve parameters differ");
		context.expect((network.getEdge(first)!=nullptr)!=(network.getEdge(second)!=nullptr),U"Exactly one usable road remains");
		context.expect(network.getRoute(route) && !network.getRoute(route)->edgeIds.isEmpty(),U"National-route membership survives consolidation");
	});
	runner.add(U"Morphology.ExistingBoundaryGateway", [](TestContext& context)
	{
		World world; world.reserveChunks(); world.setGenerationParams(42, WORLD_SIZE, WORLD_SIZE);
		for (int z = 30; z <= 33; ++z)
		{
			for (int x = 30; x <= 33; ++x)
			{
				world.installChunkDirect(Point{x, z}, HeightMapResult{Grid<float>(HEIGHT_CELLS+1, HEIGHT_CELLS+1, 20.0f), 20, 20});
			}
		}
		MapGenerator::Settlement settlement;
		settlement.center = {32768, 32768}; settlement.kind = MapGenerator::SettlementKind::RegionalCity;
		settlement.plan = makePlan(Origin::Castle, 0, Site{}, 42, false);
		RoadNetwork network; Array<int> nodes;
		for (const double offset : {-1400.0, -1100.0, 0.0, 1100.0, 1400.0})
		{
			nodes << network.addNode({32768+offset, 20, 32768});
		}
		for (size_t index = 1; index < nodes.size(); ++index)
		{
			const Vec3 a = network.getNode(nodes[index-1])->position, b = network.getNode(nodes[index])->position;
			network.addEdge(nodes[index-1], nodes[index], a+(b-a)/3, b-(b-a)/3, RoadType::Arterial, 2);
		}
		DistrictRoads::KaidoSegment kaido; kaido.passesThrough = true; kaido.dirAtCenter = {1, 0};
		DistrictRoads::generateSettlement(42, 0, settlement, kaido, world, network);
		HashSet<int> visited; Array<int> pending{nodes.front()}; visited.insert(nodes.front());
		for (size_t next = 0; next < pending.size(); ++next)
		{
			for (const auto& attachment : network.getNode(pending[next])->attachments)
			{
				const auto* edge = network.getEdge(attachment.edgeId);
				const int other = edge->nodeA == pending[next] ? edge->nodeB : edge->nodeA;
				if (visited.insert(other).second) { pending << other; }
			}
		}
		context.expect(visited.contains(nodes.back()), U"Existing nodes exactly on the clipping boundary reconnect both regional approaches");
		for (const auto& node : network.nodes())
		{
			if (node.id >= 0 && !node.attachments.isEmpty())
			{
				context.expect(visited.contains(node.id), U"No street component is isolated by a boundary-aligned source node");
			}
		}
	});
	runner.add(U"Morphology.CompactTempleGateway",[](TestContext& context)
	{
		World world; world.reserveChunks(); world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		for (int z=31;z<=32;++z)
		{
			for (int x=31;x<=32;++x) { world.installChunkDirect(Point{x,z},HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,20.0f),20,20}); }
		}
		MapGenerator::Settlement settlement; settlement.center={32768,32768}; settlement.kind=MapGenerator::SettlementKind::LocalTown;
		settlement.plan=makePlan(Origin::Temple,1,Site{},42,false); rescale(settlement.plan,.377);
		RoadNetwork network;
		const int a=network.addNode({32168,20,32778}),b=network.addNode({33368,20,32778});
		network.addEdge(a,b,{32568,20,32778},{32968,20,32778},RoadType::Arterial,2);
		DistrictRoads::KaidoSegment kaido; kaido.passesThrough=true; kaido.dirAtCenter={1,0};
		DistrictRoads::generateSettlement(42,0,settlement,kaido,world,network);
		HashSet<int> visited; Array<int> pending{a}; visited.insert(a);
		for (size_t next=0;next<pending.size();++next)
		{
			for (const auto& attachment : network.getNode(pending[next])->attachments)
			{
				const auto* edge=network.getEdge(attachment.edgeId); const int other=edge->nodeA==pending[next] ? edge->nodeB : edge->nodeA;
				if (visited.insert(other).second) { pending << other; }
			}
		}
		context.expect(visited.contains(b),U"A compact temple precinct does not disconnect the regional road at its closed side");
		for (const auto& node : network.nodes())
		{
			if (node.id>=0 && !node.attachments.isEmpty()) { context.expect(visited.contains(node.id),U"Corner gateways reconnect every street component"); }
		}
	});
	runner.add(U"Morphology.AllOriginsConnected",[](TestContext& context)
	{
		World world; world.reserveChunks(); world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		for (int z=29;z<=34;++z)
		{
			for (int x=29;x<=34;++x)
			{
				world.installChunkDirect(Point{x,z},HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,20.0f),20.0f,20.0f});
			}
		}
		TextWriter report{U"TestResults/morphology_diagnostics.txt"};
		HashSet<int> edgeCounts;
		for (int origin=0;origin<8;++origin)
		{
			for (const uint64 seed : {42ULL,20260316ULL})
			{
				MapGenerator::Settlement settlement; settlement.center={32768,32768}; settlement.radius=700;
				settlement.kind=origin==7 ? MapGenerator::SettlementKind::RuralSettlement : MapGenerator::SettlementKind::RegionalCity;
				Site site; site.shoreDistance=400;
				settlement.plan=makePlan(static_cast<Origin>(origin),static_cast<uint8>(settlement.kind),site,seed,origin!=7);
				RoadNetwork network;
				const int a=network.addNode({31168,20,32768}),b=network.addNode({34368,20,32768});
				network.addEdge(a,b,{32200,20,32768},{33200,20,32768},RoadType::Arterial,2);
				DistrictRoads::KaidoSegment kaido; kaido.passesThrough=true; kaido.dirAtCenter={1,0};
				DistrictRoads::generateSettlement(seed,0,settlement,kaido,world,network);
				HashSet<int> visited; int components=0, edges=0, deadEnds=0, civicStreets=0;
				for (const auto& node : network.nodes())
				{
					if (node.id<0 || node.attachments.isEmpty()) { continue; }
					if (node.attachments.size()==1 && node.id!=a && node.id!=b) { ++deadEnds; }
					if (visited.contains(node.id)) { continue; }
					++components; Array<int> pending{node.id}; visited.insert(node.id);
					for (size_t next=0;next<pending.size();++next)
					{
						for (const auto& attachment : network.getNode(pending[next])->attachments)
						{
							const auto* edge=network.getEdge(attachment.edgeId);
							const int other=edge->nodeA==pending[next] ? edge->nodeB : edge->nodeA;
							if (visited.insert(other).second) { pending << other; }
						}
					}
				}
				for (const auto& edge : network.edges())
				{
					if (edge.id<0) { continue; }
					++edges;
					const Vec3 middle=network.getBezier(edge.id)->evaluate(.5f);
					const Vec2 local{middle.x-settlement.center.x,middle.z-settlement.center.y};
					if (settlement.plan.civic && settlement.plan.civic->contains(local)) { ++civicStreets; }
					context.expect(edge.length>40,U"Planned streets do not leave tiny intersection fragments");
				}
				report<<U"origin={} seed={} edges={} components={} deadEnds={} civicStreets={}"_fmt(origin,seed,edges,components,deadEnds,civicStreets);
				context.expectEqual(components,1,U"All planned streets and regional entrances are connected: {}"_fmt(origin));
				context.expectEqual(civicStreets,0,U"The historic public site is not paved over");
				context.expect(edges>10,U"Every typology produces a substantive street network");
				edgeCounts.insert(edges);
			}
		}
		context.expect(edgeCounts.size()>=6,U"Origins produce distinct street structures, not labels on one template");
	});
	runner.add(U"Transport.WaterCrossing",[](TestContext& context)
	{
		RoadNetwork network;
		const auto height=[](double x,[[maybe_unused]] double z) { return Abs(x)<35 ? -4.0 : 10.0; };
		Array<int> ids;
		for (double x : {-400.0,-120.0,0.0,120.0,400.0}) { ids << network.addNode({x,height(x,0),0}); }
		for (size_t i=1;i<ids.size();++i)
		{
			const Vec3 a=network.getNode(ids[i-1])->position,b=network.getNode(ids[i])->position;
			network.addEdge(ids[i-1],ids[i],a+(b-a)/3,b-(b-a)/3,RoadType::Arterial,2);
		}
		const auto result=WaterCrossings::repair(network,height);
		context.expect(result.wetEdges>0,U"Detect actual wet sections before repairing");
		for (const auto& edge : network.edges())
		{
			if (edge.id<0) { continue; }
			const auto curve=network.getBezier(edge.id);
			for (int i=0;i<=100;++i)
			{
				const Vec3 point=curve->evaluate(i/100.0f);
				if (height(point.x,point.z)<0) { context.expect(edge.useElevation && point.y>=5.9,U"Water crossings have continuous bridge decks above water"); }
			}
		}
		context.expectNear(network.getNode(ids.front())->position.y,10,.001,U"Distant roads retain their original elevation");
	});
	runner.add(U"Transport.ConnectedTrainRoute",[](TestContext& context)
	{
		TrainNetwork network;
		const int a=network.addStation({0,10,0},U"始点"),b=network.addNode({100,10,0}),c=network.addStation({200,10,0},U"終点");
		const int first=network.addEdge(b,a,{67,10,0},{33,10,0},36),second=network.addEdge(b,c,{133,10,0},{167,10,0},36);
		const auto route=network.findRoute(a,c);
		context.expectEqual(route.size(),size_t{2},U"Intermediate track sections belong to the route");
		context.expectEqual(route.front(),first,U"Routes can start on a reversed edge");
		context.expectEqual(route.back(),second,U"Route reaches the actual destination");
		TrainSchedule schedule; schedule.id=0; schedule.headwaySec=3600; schedule.stops={StopEntry{a,1,0},StopEntry{c,10,0}}; network.addSchedule(schedule);
		TrainManager manager; manager.init(&network);
		Vec3 previous{0,10,0}; bool crossed=false,stopped=false;
		for (int i=0;i<400;++i)
		{
			manager.update(.1,i*.1);
			if (manager.trains().isEmpty()) { break; }
			const auto& train=manager.trains().front();
			context.expect(train.position.distanceFrom(previous)<1.1,U"Edge transfer never teleports a train");
			previous=train.position; crossed|=train.currentEdge==second;
			if (train.state==TrainState::WaitingStation) { stopped=true; context.expectNear(train.position.x,200,.05,U"Train stops at the destination node"); break; }
		}
		context.expect(crossed && stopped,U"Train follows both sections and brakes at its station");
	});
	runner.add(U"Morphology.ActualStreetBlocks",[](TestContext& context)
	{
		RoadNetwork network; Array<int> ids;
		for (const Vec3 point : {Vec3{0,10,0},Vec3{50,10,0},Vec3{100,10,0},Vec3{100,10,80},Vec3{50,10,80},Vec3{0,10,80}}) { ids << network.addNode(point); }
		const auto connect=[&](int a,int b)
		{
			const Vec3 start=network.getNode(ids[a])->position,end=network.getNode(ids[b])->position;
			network.addEdge(ids[a],ids[b],start+(end-start)/3,end-(end-start)/3,RoadType::LocalRoad,2);
		};
		for (int i=0;i<6;++i) { connect(i,(i+1)%6); } connect(1,4);
		const auto blocks=StreetBlocks::collect(network);
		context.expectEqual(blocks.size(),size_t{2},U"An alley divides a real block into two enclosed faces");
		for (const auto& block : blocks) { context.expectNear(block.area,4000,.1,U"Outer face is excluded from coverage"); }
	});
	runner.add(U"Transport.ReferenceSigns",[](TestContext& context)
	{
		const Font font{24,Typeface::Bold};
		for (const auto type : {RoadSignType::SpeedLimit,RoadSignType::OneWay,RoadSignType::CurveWarning})
		{
			RenderTexture target{256,256,ColorF{0,0}};
			{ const ScopedRenderTarget2D scope{target}; const ScopedRenderStates2D blend{BlendState::Opaque}; SignArtwork::draw(type,type==RoadSignType::SpeedLimit ? 40 : 1,font); Graphics2D::Flush(); }
			Image result; target.readAsImage(result); result.save(U"Screenshot/reference_sign_{}.png"_fmt(static_cast<int>(type)));
			context.expect(result[128][128].a>200,U"Sign face has visible artwork");
			const FilePath directory=FileSystem::CurrentDirectory();
			FileSystem::ChangeCurrentDirectory(directory+U"../../App/");
			const auto board=RoadSign::CreateBoardMesh(type);
			FileSystem::ChangeCurrentDirectory(directory);
			context.expect(!board.vertices.isEmpty(),U"Each sign has a physical board mesh");
		}
	});

}
