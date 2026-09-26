#include "TestCases.hpp"
#include "src/gen/RailCostProfile.hpp"
#include "src/road/RoadGeometry.hpp"
#include "src/ui/WalkSurface.hpp"
#include "src/ui/Camera.hpp"
#include "src/gen/RiverNetwork.hpp"
#include "src/gen/RoadTerrainFit.hpp"
#include "src/gen/DistrictHierarchy.hpp"
#include "src/render/RailStructure.hpp"
#include "src/render/TunnelGeometry.hpp"
#include "src/ui/WorldMapView.hpp"
#include "src/gen/RoadAutoPlace.hpp"
#include "src/road/RoadConstruction.hpp"
#include "src/save/RoadBinary.hpp"
#include "TestRunner.hpp"
#include "src/gen/DistrictRoads.hpp"
#include "src/gen/SettlementPlan.hpp"
#include "src/gen/SettlementPlacement.hpp"
#include "src/gen/StreetProfile.hpp"
#include "src/gen/WaterCrossings.hpp"
#include "src/gen/StreetBlocks.hpp"
#include "src/railway/TrainManager.hpp"
#include "src/road/SignArtwork.hpp"
#include "src/road/RoadSign.hpp"

void registerUrbanMorphologyTests(TestRunner& runner)
{
	runner.add(U"Morphology.RegionalTerrainSurvey", [](TestContext& context)
	{
		TextWriter report{U"TestResults/regional_terrain.txt"};
		for (uint64 seed : {7, 42, 130, 2026})
		{
			World world; world.setGenerationParams(seed, WORLD_SIZE, WORLD_SIZE);
			int sea = 0, flat = 0, mountain = 0; double highest = -10000, lowest = 10000;
			for (int z = 0; z < 256; ++z) for (int x = 0; x < 256; ++x)
			{
				const float wx = (x + .5f) * 256, wz = (z + .5f) * 256;
				const double h = world.computeHeight(wx, wz);
				const double slope = Max(Abs(world.computeHeight(wx + 40, wz) - h), Abs(world.computeHeight(wx, wz + 40) - h)) / 40;
				sea += h < 0; flat += h > 3.2 && h < 300 && slope < .05; mountain += h > 600;
				highest = Max(highest, h); lowest = Min(lowest, h);
			}
			report.writeln(U"seed={} sea={} flat={} mountain={} min={} max={}"_fmt(seed, sea / 65536.0, flat / 65536.0, mountain / 65536.0, lowest, highest));
			context.expect(highest > 1200 && highest < 3777 && mountain > 65536 * .20,
				U"Mountain belts have substantial area and plausible regional summits");
			context.expect(sea > 65536 * .10 && sea < 65536 * .40 && flat > 65536 * .18, U"Bays coexist with enough dry, gently sloping land for towns and farms");
			const auto left = world.buildHeightMap({31, 31}), right = world.buildHeightMap({32, 31});
			for (int z = 0; z <= HEIGHT_CELLS; ++z)
			{
				context.expect(left.heightMap[{HEIGHT_CELLS, z}] == right.heightMap[{0, z}], U"Regional landforms have exactly matching chunk seams");
			}
			World repeat; repeat.setGenerationParams(seed, WORLD_SIZE, WORLD_SIZE);
			context.expect(world.computeHeight(30000, 32000) == repeat.computeHeight(30000, 32000), U"Regional structure is deterministic for a seed");
		}
	});

	using namespace UrbanMorphology;
	runner.add(U"Morphology.HinterlandPreservesVillages", [](TestContext& context)
	{
		using Kind = MapGenerator::SettlementKind;
		Array<SettlementPlacement::Candidate> candidates{{{6000, 6000}, 1.0f}};
		for (int z = 600; z < 12000; z += 480)
		{
			for (int x = 600; x < 12000; x += 480) { candidates << SettlementPlacement::Candidate{{x, z}, 0.7f}; }
		}
		const auto flat = [](Vec2) { return 20.0; };
		double sampleNearest=1e9,sampleFarthest=0;
		TextWriter report{U"TestResults/settlement_hinterland.txt"};
		for (uint64 seed : {7, 42, 130, 2026})
		{
			const auto settlements = SettlementPlacement::generate(seed, candidates, RectF{0, 0, 12000, 12000}, flat);
			const auto repeat = SettlementPlacement::generate(seed, candidates, RectF{0, 0, 12000, 12000}, flat);
			context.expectEqual(settlements.size(), repeat.size(), U"Seed and terrain give the same settlement count");
			context.expect(settlements.front().center == Vec2{6000, 6000}, U"The broad best site becomes the regional center");
			int nearbyVillages = 0, attachedVillages = 0;
			double nearest = 1e9, farthest = 0;
			for (size_t i = 0; i < settlements.size(); ++i)
			{
				const auto& settlement = settlements[i];
				context.expect(settlement.center == repeat[i].center && settlement.kind == repeat[i].kind
					&& settlement.serviceCenter == repeat[i].serviceCenter, U"Ordering, origins and catchments are deterministic");
				if (settlement.kind != Kind::RuralSettlement) { continue; }
				const double distance = settlement.center.distanceFrom(settlements.front().center);
				nearbyVillages += distance < 6000;
				nearest = Min(nearest, distance); farthest = Max(farthest, distance);
				context.expect(distance >= 3200, U"Farmland separates the castle-town fringe and independent village nuclei");
				context.expect(settlement.plan.origin == Origin::Rural && !settlement.plan.station,
					U"Nearby villages keep rural plots instead of turning into urban grids or station towns");
				if (settlement.serviceCenter)
				{
					++attachedVillages;
					context.expect(*settlement.serviceCenter < i && settlements[*settlement.serviceCenter].kind != Kind::RuralSettlement,
						U"A village refers to a real center, never itself or another dependent village");
					context.expect(settlement.accessDirection && Abs(settlement.accessDirection->length() - 1) < 1e-8,
						U"Village access retains the initial direction of the terrain corridor");
				}
				for (size_t j = 0; j < i; ++j)
				{
					if (settlements[j].kind == Kind::RuralSettlement)
					{
						context.expect(settlement.center.distanceFrom(settlements[j].center) >= 1400,
							U"Village farming areas are not packed against one another");
					}
				}
			}
			context.expect(nearbyVillages >= 2 && attachedVillages >= nearbyVillages,
				U"Quarter-density catchments retain independent villages within six kilometres");
			sampleNearest=Min(sampleNearest,nearest);sampleFarthest=Max(sampleFarthest,farthest);
			report << U"seed={} total={} nearbyVillages={} attached={} nearest={} farthest={}"_fmt(seed, settlements.size(), nearbyVillages, attachedVillages, nearest, farthest);
		}
		context.expect(sampleFarthest-sampleNearest>1500,U"The seed ensemble retains villages at different distances from the city");
	});
	runner.add(U"Morphology.HinterlandTerrainCatchments", [](TestContext& context)
	{
		using Kind = MapGenerator::SettlementKind;
		Array<SettlementPlacement::Candidate> candidates{{{3000, 6000}, 1.0f}, {{25000, 6000}, 1.0f}, {{12000, 6000}, 0.29f}};
		for (int z = 1000; z < 12000; z += 640)
		{
			for (int x = 1000; x < 30000; x += 640) { candidates << SettlementPlacement::Candidate{{x, z}, 0.25f}; }
		}
		const auto terrain = [](Vec2 p)
		{
			// A tall north/south ridge separates otherwise flat agricultural basins.
			return 20.0 + Max(0.0, 900.0 - Abs(p.x - 8500)) * 0.9;
		};
		int rightOwned = 0, acrossRidge = 0;
		for (uint64 seed=0;seed<16;++seed)
		{
			const auto settlements = SettlementPlacement::generate(seed, candidates, RectF{0, 0, 30000, 12000}, terrain);
			for (const auto& settlement : settlements)
			{
				context.expect(Abs(settlement.center.x - 8500) >= 900, U"Villages are not placed on the ridge even with candidate points supplied there");
				if (settlement.kind != Kind::RuralSettlement || !settlement.serviceCenter) { continue; }
				const auto& center = settlements[*settlement.serviceCenter];
				acrossRidge += (settlement.center.x < 8500) != (center.center.x < 8500);
				if (settlement.center.x > 10000 && settlement.center.x < 14000 && center.center.x > 20000) { ++rightOwned; }
			}
		}
		context.expect(acrossRidge == 0 && rightOwned > 0,
			U"A physically nearer center across a mountain loses to the accessible center in the same basin");
		const auto flooded = SettlementPlacement::generate(42, candidates, RectF{0, 0, 30000, 12000}, [](Vec2) { return -10.0; });
		context.expect(flooded.isEmpty(), U"Water-only terrain does not receive settlements");
		const auto inlandLake = SettlementPlacement::generate(42, candidates, RectF{0, 0, 30000, 12000},
			[](Vec2) { return 100.0; }, [](Vec2) { return 102.0; });
		context.expect(inlandLake.isEmpty(), U"Positive-altitude inland water is excluded using its actual surface level");
		const auto inlandRiver = SettlementPlacement::generate(42, candidates, RectF{0, 0, 30000, 12000},
			[](Vec2 p) { return Abs(p.x - 8500) < 220 ? 98.0 : 105.0; },
			[](Vec2 p) { return Abs(p.x - 8500) < 220 ? 101.0 : 0.0; });
		for (const auto& settlement : inlandRiver)
		{
			context.expect(Abs(settlement.center.x - 8500) >= 220, U"A river bed above sea level cannot host village nuclei");
		}
		const Vec2 village{0, 0};
		const Optional<Vec2> corridor{Vec2{1, 0}};
		context.expect(SettlementPlacement::roadAccessCost(village, {1400, 0}, corridor)
			< SettlementPlacement::roadAccessCost(village, {-1000, 0}, corridor),
			U"Shared roads in the center's corridor are preferred over a slightly nearer road pointing away");
		context.expect(SettlementPlacement::roadAccessCost(village, {-100, 0}, corridor)
			< SettlementPlacement::roadAccessCost(village, {1400, 0}, corridor),
			U"An immediately adjacent road still wins; access does not force long radial spokes");
	});

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
		const auto industrial=makePlan(Origin::Industrial,0,site,42,true);
		context.expect(industrial.industry.h>industrial.industry.w*2.5
			&& !industrial.industry.contains(industrial.oldCore)
			&& industrial.station && !industrial.industry.contains(*industrial.station),
			U"The industrial estate forms a long strip between the old town and the station");
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
		TrainSchedule schedule; schedule.id=0; schedule.headwaySec=1440; schedule.stops={StopEntry{a,1},StopEntry{c,10}}; network.addSchedule(schedule);
		TrainManager manager; manager.init(&network);
		Vec3 previous{0,10,0}; bool crossed=false,stopped=false;
		for (int i=0;i<400;++i)
		{
			manager.update(.1,i*.1);
			if (manager.trains().isEmpty()) { break; }
			const auto& train=manager.trains().front();
			context.expect(i==0 || train.position.distanceFrom(previous)<1.1,U"Edge transfer never teleports a train");
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

	runner.add(U"Map.KeyboardAndAllStreetScales",[](TestContext& context)
	{
		WorldMapView map; const Size size{960,640}; map.open({32768,32768}); map.zoom=1;
		const Vec2 before=map.center; map.panKeyboard({1,0},.1,size);
		context.expectNear((before-map.center).x*map.scale(size),42,.001,U"D pans east at 420 screen pixels per second");
		map.panKeyboard({-1,0},.1,size); context.expectNear(map.center.distanceFrom(before),0,.001,U"A reverses east movement");
		map.panKeyboard({0,-1},.1,size); context.expect(map.center.y<before.y,U"W pans north"); map.panKeyboard({0,1},.1,size);
		map.panKeyboard({1,1},.1,size); context.expectNear(map.center.distanceFrom(before)*map.scale(size),42,.001,U"Diagonal keys do not move faster");
		const Font font{14}; const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm};
		for (const double zoom : {1.0,4.0,16.0,128.0})
		{
			map.center=before;map.zoom=zoom; map.streets.clear();map.labels.clear();
			map.streets << WorldMapView::Stroke{{before-Vec2{16000,0},before+Vec2{16000,0}},RectF{before-Vec2{16000,20},32000,40},4,0};
			{ const ScopedRenderTarget2D scope{target.clear(ColorF{0})};map.draw(size,Texture{},font,{0,0}); } Graphics2D::Flush();
			Image image;target.readAsImage(image);const Point p=map.toScreen(before+Vec2{100/ map.scale(size),0},size).asPoint();
			context.expect(image[p.y][p.x].r>245,U"Local streets remain visible on the GPU at every tested zoom");
			image.save(U"Screenshot/map_scale_{}.png"_fmt(static_cast<int>(zoom)));
		}
	});
	runner.add(U"Rivers.DrainageAndWaterLevel",[](TestContext& context)
	{
		const auto terrain=[](double x,double z) { const double branch=Max(0.0,z-8192)*.55;return z*.004+Min(Abs(x-8192-branch),Abs(x-8192+branch))*.025-8; };
		RiverNetwork river;river.generate(16384,16384,terrain);
		context.expect(!river.reaches.isEmpty(),U"A convergent valley develops a river");
		int confluences=0;
		for (const auto& reach : river.reaches)
		{
			context.expect(reach.start.y>=reach.end.y,U"Every reach drains downhill toward its parent");
			const Vec3 p=(reach.start+reach.end)*.5;
			context.expect(river.carveHeight({p.x,p.z},terrain(p.x,p.z))<river.waterLevel({p.x,p.z}),U"Carved channel bed lies below its local water surface");
			for (const auto& next : river.reaches) { confluences+=reach.end.distanceFrom(next.end)<.01 && reach.start.distanceFrom(next.start)>1; }
		}
		context.expect(confluences>0,U"Tributaries merge into shared downstream reaches");
		RiverNetwork same;same.generate(16384,16384,terrain);context.expectEqual(same.reaches.size(),river.reaches.size(),U"Drainage is deterministic");
	});
	runner.add(U"Transport.RaisedRiverCrossing",[](TestContext& context)
	{
		RoadNetwork roads;const int a=roads.addNode({0,23,0}),b=roads.addNode({300,23,0});
		roads.addEdge(a,b,{100,23,0},{200,23,0},RoadType::Arterial,2);
		const auto ground=[](double x,double) { return Abs(x-150)<20 ? 18.0 : 23.0; };
		const auto water=[](double x,double) { return Abs(x-150)<20 ? 20.0 : 0.0; };
		const auto result=WaterCrossings::repair(roads,ground,water);context.expect(result.elevatedEdges>0,U"An inland river requires a bridge above its own water level");
		for (const auto& edge : roads.edges()) if (edge.id>=0)
		{
			const auto curve=roads.getBezier(edge.id);for (float arc=0;arc<curve->totalLength;arc+=2)
			{ const Vec3 p=curve->positionAt(arc); if (Abs(p.x-150)<20) context.expect(edge.useElevation && p.y>=25.9,U"River crossing clears the 20 m water surface"); }
		}
	});
	runner.add(U"Terrain.SmallRoadEarthworks",[](TestContext& context)
	{
		World world;world.reserveChunks();world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		for (int x=0;x<2;++x) { world.installChunkDirect({x,0},HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,20),20,20}); }
		RoadNetwork roads;const int a=roads.addNode({900,21,512}),b=roads.addNode({1150,21,512});
		const int edge=*roads.addEdge(a,b,{980,21,512},{1070,21,512},RoadType::LocalRoad,2);roads.getEdge(edge)->useElevation=true;
		context.expectEqual(RoadTerrainFit::apply(roads,world),1,U"One metre of fill does not require a viaduct");
		context.expect(!roads.getEdge(edge)->useElevation,U"Grid road becomes a terrain-following road");
		context.expect(world.sampleHeight(1024,512)>20.1,U"The actual shared terrain is graded toward the road");
		context.expectNear(world.getChunk({0,0})->heightMap[{HEIGHT_CELLS,32}],world.getChunk({1,0})->heightMap[{0,32}],.0001,U"Earthworks cannot split a chunk boundary");
	});
	runner.add(U"Rail.CostProfileAndClearance",[](TestContext& context)
	{
		Array<RailCostProfile::Sample> samples;
		for (int i=0;i<=60;++i) { const double ground=20+60*std::exp(-Square((i-30)/8.0));samples << RailCostProfile::Sample{{i*50.0,0},ground,-30,ground+25}; }
		const auto profile=RailCostProfile::solve(samples,20.3,20.7);
		context.expect(profile.feasible,U"A hill can be crossed using an affordable tunnel instead of a towering viaduct");
		if (profile.feasible)
		{
			context.expectNear(profile.heights.front(),20.3,.0001,U"Non-quantized endpoint elevation is preserved");context.expectNear(profile.heights.back(),20.7,.0001,U"Destination elevation is preserved");
			context.expect(samples[30].ground-profile.heights[30]>20,U"The optimizer chooses a mountain tunnel");
			for (size_t i=1;i<samples.size();++i) { context.expect(Abs(profile.heights[i]-profile.heights[i-1])<=.9+.00001,U"Vertical grade remains within 18 per mille"); }
		}
		context.expect(RailCostProfile::unitCost(60)>RailCostProfile::unitCost(10)*5,U"Very tall viaducts have a strong cost penalty");
	});
	runner.add(U"Tunnels.RoadPlanAndPersistence",[](TestContext& context)
	{
		World world;world.reserveChunks();world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		Grid<float> heights(HEIGHT_CELLS+1,HEIGHT_CELLS+1,20);
		for (int z=0;z<=HEIGHT_CELLS;++z) for (int x=0;x<=HEIGHT_CELLS;++x) { heights[{x,z}]=static_cast<float>(20+30*std::exp(-Square((x*16.0-512)/130))); }
		world.installChunkDirect({0,0},HeightMapResult{heights,20,50});
		RoadNetwork roads;RoadEdge roadTemplate;roadTemplate.roadType=RoadType::Arterial;
		const auto ids=RoadAutoPlace::buildPlanned(roads,world,{100,20,512},{900,20,512},{},roadTemplate,false,.5f);
		context.expect(!ids.isEmpty(),U"A fixed vertical alignment through a hill can be planned");
		for (const int id : ids) { context.expect(roads.getEdge(id)->tunnel && roads.getEdge(id)->useElevation,U"Road interior depth selects tunnel rendering"); }
		context.expect(RoadBinary::writeGlobal(U"TestResults/tunnel_roundtrip.bin",roads),U"Tunnel flags save");
		RoadNetwork restored;context.expect(RoadBinary::readGlobal(U"TestResults/tunnel_roundtrip.bin",restored),U"Tunnel flags reload");
		for (const int id : ids) { context.expect(restored.getEdge(id)->tunnel,U"The tunnel structure survives serialization"); }
		RoadConstruction::Progress progress;progress.tunnel=true;progress.stage=RoadConstruction::Stage::BaseCourse;
		context.expect(progress.name().includes(U"覆工"),U"Tunnel construction uses lining terminology");
	});
	runner.add(U"Rivers.LeveeRoadPersistence",[](TestContext& context)
	{
		RoadNetwork roads;
		const int start=roads.addNode({100,20,100}),end=roads.addNode({400,20,100});
		const int original=*roads.addEdge(start,end,{200,21,100},{300,21,100},RoadType::LocalRoad,2);
		auto* edge=roads.getEdge(original);
		edge->leveeRoad=true;edge->designGrade=true;edge->edgeState=EdgeState::Open;
		const int joint=roads.splitEdgeAtParameter(original,.5f);
		context.expect(joint>=0,U"A connected levee road can be divided at a new junction");
		int segments=0;
		for (const auto& part:roads.edges())
		{
			if (part.id<0) { continue; }
			++segments;
			context.expect(part.leveeRoad && part.designGrade,U"The earthwork role and designed height survive a split");
		}
		context.expect(segments==2,U"The new junction retains both road segments");
		context.expect(RoadBinary::writeGlobal(U"TestResults/levee_road.bin",roads),U"Levee roads can be saved");
		RoadNetwork loaded;
		context.expect(RoadBinary::readGlobal(U"TestResults/levee_road.bin",loaded),U"Levee roads can be loaded");
		int restored=0;
		for (const auto& part:loaded.edges()) { if (part.id>=0 && part.leveeRoad) { ++restored; } }
		context.expect(restored==2,U"Both connected levee segments remain marked after loading");
	});
	runner.add(U"Structures.UndersideAndTunnelReadback",[](TestContext& context)
	{
		const Size size{640,400}; const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};
		const MeshData deck=RailStructure::deck({0,8,-15},{0,8,15},{1,0,0},{1,0,0});
		int downward=0;for (const auto& v : deck.vertices) { downward+=v.normal.y<-.9; }context.expect(downward>=20,U"All slab and girder undersides have downward-facing surfaces");
		for (int variant=0;variant<2;++variant)
		{
			const Mesh mesh{variant==0 ? deck : TunnelGeometry::lining({0,0,-15},{0,0,15},{1,0,0},{1,0,0},4.2,6,true)};
			const BasicCamera3D camera{size,50_deg,variant==0 ? Vec3{0,1,-8} : Vec3{0,1.5,-18},variant==0 ? Vec3{0,8,4} : Vec3{0,3,8}};
			{ const ScopedRenderTarget3D rt{target.clear(ColorF{.02,.08,.2})};const ScopedRenderStates3D states{DepthStencilState::DepthTestWrite,RasterizerState::SolidCullBack};
				Graphics3D::SetCameraTransform(camera);Graphics3D::SetSunDirection(Vec3{0,-1,0});Graphics3D::SetGlobalAmbientColor(ColorF{.8});mesh.draw(ColorF{.7}); }
			Graphics3D::Flush();Image image;target.readAsImage(image);image.save(U"Screenshot/structure_interior_{}.png"_fmt(variant));
			int concrete=0;for (int y=60;y<330;++y) for (int x=100;x<540;++x) { const auto color=image[y][x];concrete+=color.r>70 && Abs(static_cast<int>(color.r)-color.b)<30; }
			context.expect(concrete>3000,U"Concrete surfaces are visible with backface culling from underneath and inside");
		}
		const PixelShader water=HLSL{U"../../App/shaders/hlsl/city_forward.hlsl",U"River_PS"};context.expect(static_cast<bool>(water),U"River material compiles on the production graphics API");
	});
	runner.add(U"Districts.HierarchyCoverage",[](TestContext& context)
	{
		World world;world.reserveChunks();world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		Array<MapGenerator::Settlement> towns;
		for (int i=0;i<5;++i) { MapGenerator::Settlement town;town.center={30000+i*600.0,32768};town.name=U"町{}"_fmt(i);town.kind=i==0 ? MapGenerator::SettlementKind::RegionalCity : MapGenerator::SettlementKind::RuralSettlement;town.radius=500;towns << town; }
		DistrictHierarchy hierarchy;hierarchy.generate(world,towns);context.expect(!hierarchy.boundaries.isEmpty(),U"Districts have explicit shared boundaries");
		for (int z=64;z<WORLD_SIZE;z+=512) for (int x=64;x<WORLD_SIZE;x+=512)
		{
			const Vec2 point{static_cast<double>(x),static_cast<double>(z)};const int city=hierarchy.at(point,0),area=hierarchy.at(point,1),leaf=hierarchy.at(point,2);
			context.expect(city>=0 && area>=0 && leaf>=0,U"Every map point has a complete address hierarchy");
			if (area>=0 && leaf>=0) { context.expect(hierarchy.areas[area].parent==city && hierarchy.areas[leaf].parent==area,U"No child boundary crosses its parent district"); }
		}
		context.expect(hierarchy.address(towns[2].center).includes(U"大字"),U"Rural settlement names use an oaza parent");
	});

	runner.add(U"Terrain.LevelRoadAcrossHillside",[](TestContext& context)
	{
		World world; world.reserveChunks(); world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		Grid<float> heights(HEIGHT_CELLS+1,HEIGHT_CELLS+1);
		for (int z=0;z<=HEIGHT_CELLS;++z) for (int x=0;x<=HEIGHT_CELLS;++x) { heights[{x,z}]=static_cast<float>(100+(z*16-512)*.6); }
		world.installChunkDirect({0,0},HeightMapResult{heights,-208,408});
		RoadNetwork roads; const int a=roads.addNode({200,100,512}),b=roads.addNode({800,100,512});
		const int id=*roads.addEdge(a,b,{400,100,512},{600,100,512},RoadType::Arterial,2);
		const auto mesh=RoadGeometry::roadbedSurface(*roads.getEdge(id),*roads.getBezier(id),world);
		double roll=0;
		for (size_t i=0;i+1<mesh.vertices.size();i+=2) { const Vec3 delta=Vec3{mesh.vertices[i+1].pos}-Vec3{mesh.vertices[i].pos}; roll=Max(roll,Abs(delta.y)/Max(.01,Vec2{delta.x,delta.z}.length())); }
		TextWriter{U"TestResults/hillside_roll.txt"}.write(U"crossSlope={}"_fmt(roll));
		context.expect(roll<=.025,U"Road crossfall stays under 2.5 percent on a 60 percent hillside");
	});
	runner.add(U"Transport.StreamOnLegalMountainGrade", [](TestContext& context)
	{
		RoadNetwork roads; Array<int> nodes;
		for (int i = 0; i <= 20; ++i) { nodes << roads.addNode({100 + i * 40.0, 20 + i * 3.2, 500}); }
		for (int i = 0; i < 20; ++i)
		{
			const Vec3 a = roads.getNode(nodes[i])->position, b = roads.getNode(nodes[i+1])->position;
			roads.addEdge(nodes[i], nodes[i+1], a.lerp(b, 1.0/3), a.lerp(b, 2.0/3), RoadType::LocalRoad, 2);
		}
		const auto level = [](double x) { return 20 + (x - 100) * .08; };
		const auto ground = [&](double x, double) { return level(x) - (Abs(x - 740) < 20 ? 8 : 0); };
		const auto water = [&](double x, double) { return Abs(x - 740) < 20 ? level(x) - 6 : 0; };
		WaterCrossings::repair(roads, ground, water);
		context.expectNear(roads.getNode(nodes.front())->position.y, 20, .01, U"An uphill bank and a legal eight-percent approach cannot lift the distant town");
		double lift = 0; for (int i = 0; i <= 20; ++i) { lift = Max(lift, roads.getNode(nodes[i])->position.y - (20 + i * 3.2)); }
		context.expect(lift < 5, U"Bridge adjustment stays near the stream and never inherits the whole upper hillside");
		JSON report; report[U"maximumLift"] = lift; report.save(U"TestResults/stream_grade.json");
	});

	runner.add(U"Transport.IncisedValleyApproach",[](TestContext& context)
	{
		const auto ground=[](double x,double) { return 60-Min(40.0,Max(0.0,180-Abs(x-500))*.4); };
		const auto water=[](double x,double) { return Abs(x-500)<70 ? 24.0 : 0.0; };
		RoadNetwork roads; Array<int> nodes;
		for (int i=0;i<=10;++i) { nodes<<roads.addNode({i*100.0,ground(i*100.0,0),0}); }
		for (int i=0;i<10;++i) { const Vec3 a=roads.getNode(nodes[i])->position,b=roads.getNode(nodes[i+1])->position; roads.addEdge(nodes[i],nodes[i+1],a.lerp(b,1.0/3),a.lerp(b,2.0/3),RoadType::Arterial,2); }
		WaterCrossings::repair(roads,ground,water); double slope=0,minimum=1e9;
		for (const auto& edge : roads.edges()) { if (edge.id<0) { continue; } const auto curve=roads.getBezier(edge.id); for (float arc=0;arc<curve->totalLength;arc+=2) { const Vec3 tangent=curve->tangentAt(arc),p=curve->positionAt(arc); slope=Max(slope,Abs(tangent.y)/Max(.001,Vec2{tangent.x,tangent.z}.length())); if (Abs(p.x-500)<80) { minimum=Min(minimum,p.y); } } }
		TextWriter report{U"TestResults/valley_approach.txt"}; report.writeln(U"maximumGrade={} bridgeLow={}"_fmt(slope,minimum));
		for (const auto& edge : roads.edges())
		{
			if (edge.id<0) { continue; }
			const Vec3 a=roads.getNode(edge.nodeA)->position,b=roads.getNode(edge.nodeB)->position;
			report.writeln(U"{} {} -> {} {} elevated={}"_fmt(a.x,a.y,b.x,b.y,edge.useElevation));
		}
		context.expect(slope<=.061,U"Bridge and approaches stay below six percent without copying the incised river bed");
		context.expect(minimum>=50,U"Bridge connects high banks without an unnecessary descent into the valley");
	});

	runner.add(U"Rivers.SmoothWideningAndVegetation",[](TestContext& context)
	{
		const auto height=[](double x,double z) { return z*.004+Abs(x-8192-500*Sin(z*.0005))*.03-8; };
		RiverNetwork river;river.generate(16384,16384,height);
		bool widened=false;double maxSegment=0;int wetTrees=0;
		for (const auto& reach : river.reaches)
		{
			context.expect(reach.endHalfWidth>=reach.halfWidth,U"Accumulated flow never narrows toward the mouth");
			widened|=reach.endHalfWidth>reach.halfWidth+.001;maxSegment=Max(maxSegment,Vec2{reach.end.x-reach.start.x,reach.end.z-reach.start.z}.length());
			const Vec2 point{reach.start.x,reach.start.z}; wetTrees+=river.vegetationAllowed(point,100,12);
		}
		context.expect(widened && maxSegment<18,U"Banks follow short curved stations with downstream widening");
		context.expectEqual(wetTrees,0,U"Even high-elevation river surfaces exclude tree crowns");
	});
	runner.add(U"Terrain.CutFillAndBridgeClassification",[](TestContext& context)
	{
		World world;world.reserveChunks();world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		world.installChunkDirect({0,0},HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,30),30,30});
		RoadNetwork roads;
		const auto add=[&](double z,double y,bool tunnel) { const int a=roads.addNode({100,y,z}),b=roads.addNode({900,y,z});const int id=*roads.addEdge(a,b,{366,y,z},{634,y,z},RoadType::Arterial,2);auto* edge=roads.getEdge(id);edge->useElevation=true;edge->tunnel=tunnel;return id; };
		const int fill=add(200,35,false),cut=add(400,24,false),bridge=add(600,45,false),tunnel=add(800,10,true);
		RoadTerrainFit::apply(roads,world);
		context.expect(!roads.getEdge(fill)->useElevation && !roads.getEdge(cut)->useElevation,U"Five metre fills and six metre cuttings are earthworks");
		context.expect(roads.getEdge(bridge)->useElevation && roads.getEdge(tunnel)->tunnel,U"Tall viaducts and covered tunnels keep their structure");
		context.expectNear(world.sampleHeight(512,200),35,.05,U"Embankment supports the complete roadbed");
		context.expectNear(world.sampleHeight(512,400),24,.05,U"Cutting removes the terrain from the roadbed");
		context.expectNear(world.sampleHeight(512,800),30,.01,U"Tunnel roof terrain is retained");
	});
	runner.add(U"Camera.TransportWalkingAndFastMap",[](TestContext& context)
	{
		World world;world.reserveChunks();world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		world.installChunkDirect({0,0},HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,20),20,20});
		RoadNetwork roads;TrainNetwork trains;WalkSurface surface;
		const auto road=[&](double y,double z,bool tunnel) { const int a=roads.addNode({100,y,z}),b=roads.addNode({900,y,z});const int id=*roads.addEdge(a,b,{366,y,z},{634,y,z},RoadType::Arterial,2);roads.getEdge(id)->useElevation=true;roads.getEdge(id)->tunnel=tunnel; };
		road(28,200,false);road(8,400,true);
		const int a=trains.addNode({100,5,600}),b=trains.addNode({900,5,600});trains.addEdge(a,b,{366,5,600},{634,5,600});
		context.expectNear(surface.resolve({500,28,200},world,roads,trains),28+kRoadSurfaceLift,.001,U"Walking follows bridge pavement");
		context.expectNear(surface.resolve({500,20,200},world,roads,trains),20,.001,U"Walking below a viaduct does not jump onto it");
		context.expectNear(surface.resolve({500,8,400},world,roads,trains),8+kRoadSurfaceLift,.001,U"Tunnel road surface wins over terrain overhead");
		context.expectNear(surface.resolve({500,5,600},world,roads,trains),5.15,.001,U"Railway tunnel is walkable below the mountain");
		GameCamera camera;camera.setWalkingState({200,8,400},static_cast<float>(Math::HalfPi));camera.setWalkSurface([&](Vec3 p) { return surface.resolve(p,world,roads,trains); });camera.walk({0,1},180,world);
		context.expectNear(camera.eyePosition().y,9.5+kRoadSurfaceLift,.001,U"Accelerated walking preserves tunnel eye height");
		WorldMapView map;const Size size{960,640};map.open({32768,32768});const Vec2 before=map.center;map.panKeyboard({1,1},.1,size,true);
		context.expectNear(map.center.distanceFrom(before)*map.scale(size),126,.001,U"Ctrl map movement triples speed without diagonal acceleration");
	});
	runner.add(U"Tunnels.BooleanPortalAndRoof",[](TestContext& context)
	{
		const Vec3 origin{0,20,0};const auto portal=TunnelGeometry::portal(origin,{0,0,1},4.2,6);
		const auto cutter=TunnelGeometry::cutter({0,20,-10},{0,20,10},4.2,6);const auto planes=MeshBoolean::planes(cutter);
		MeshBoolean::Face roof;
		for (const Vec3 p : {Vec3{-10,30,-5},Vec3{10,30,-5},Vec3{0,30,5}}) { roof<<Vertex3D{Float3{p},Float3{0,1,0},Float2{0,0}}; }
		context.expectEqual(MeshBoolean::subtractFace(roof,planes).size(),size_t{1},U"True three-dimensional subtraction preserves terrain above the tunnel crown");
		MeshBoolean::Face obstructing;
		for (const Vec3 p : {Vec3{-2,22,-1},Vec3{2,22,-1},Vec3{0,22,1}}) { obstructing<<Vertex3D{Float3{p},Float3{0,1,0},Float2{0,0}}; }
		context.expect(MeshBoolean::subtractFace(obstructing,planes).isEmpty(),U"Terrain inside the excavated tunnel is removed");
		const Size size{640,400};const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};
		for (const int side : {-1,1})
		{
			const BasicCamera3D camera{size,50_deg,{0,23,side*18.0},{0,23,0}};
			{ const ScopedRenderTarget3D rt{target.clear(ColorF{.02,.08,.2})};const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite,RasterizerState::SolidCullBack};Graphics3D::SetCameraTransform(camera);Graphics3D::SetGlobalAmbientColor(ColorF{.8});Mesh{portal}.draw(ColorF{.7}); }
			Graphics3D::Flush();Image pixels;target.readAsImage(pixels);pixels.save(U"Screenshot/boolean_portal_{}.png"_fmt(side));
			context.expect(pixels[200][320].b>pixels[200][320].r*2,U"The portal passage stays open from both directions");
			int concrete=0;for (int y=0;y<size.y;++y) for (int x=0;x<size.x;++x) { const auto c=pixels[y][x];concrete+=c.r>90 && Abs(static_cast<int>(c.r)-c.b)<25; }
			context.expect(concrete>5000,U"The boolean portal retains visible concrete and inner thickness");
		}
	});

	runner.add(U"Transport.CurvedBridgeGrade",[](TestContext& context)
	{
		RoadNetwork roads;const int a=roads.addNode({0,12,0}),b=roads.addNode({120,16,40});
		const int id=*roads.addEdge(a,b,{3,13.3,30},{110,14.7,30},RoadType::Arterial,2);roads.getEdge(id)->useElevation=true;
		WaterCrossings::repair(roads,[](double,double) { return -4.0; });
		double maximum=0;const auto curve=roads.getBezier(id);
		for (int i=0;i<=200;++i) { const Vec3 tangent=curve->tangent(i/200.0f);maximum=Max(maximum,Abs(tangent.y)/Max(.001,Vec2{tangent.x,tangent.z}.length())); }
		context.expect(maximum<=.05001,U"An already elevated curved bridge respects the actual spatial grade, including short control arms");
	});

	runner.add(U"Morphology.NarrowFacesMergeWithoutDisconnecting",[](TestContext& context)
	{
		RoadNetwork roads;Array<int> nodes;
		for (const Vec3 p : {Vec3{0,20,0},Vec3{100,20,0},Vec3{100,20,100},Vec3{0,20,100},Vec3{8,20,0},Vec3{8,20,100}}) { nodes<<roads.addNode(p); }
		const auto connect=[&](int a,int b) { const Vec3 p=roads.getNode(nodes[a])->position,q=roads.getNode(nodes[b])->position;roads.addEdge(nodes[a],nodes[b],p.lerp(q,1.0/3),p.lerp(q,2.0/3),RoadType::LocalRoad,2); };
		connect(0,4);connect(4,1);connect(1,2);connect(2,5);connect(5,3);connect(3,0);connect(4,5);
		context.expectEqual(StreetBlocks::mergeNarrowFaces(roads),1,U"A shortcut enclosing an eight metre sliver is merged into the usable block");
		context.expectEqual(StreetBlocks::collect(roads).size(),size_t{1},U"Removing the redundant shortcut retains the outer access loop");
	});

}
