#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/gen/DistrictRoads.hpp"
#include "src/gen/AgriculturalLayout.hpp"
#include "src/gen/ParcelGeometry.hpp"
#include "src/render/WorldRenderer.hpp"

namespace
{
	void flatChunk(World& world,Point coord)
	{
		world.installChunkDirect(coord,HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,20.0f),20,20});
	}
}

void registerFringeAgricultureTests(TestRunner& runner)
{
	runner.add(U"Morphology.CastleTownTransition",[](TestContext& context)
	{
		World world; world.reserveChunks(); world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		for (int z=29;z<=34;++z)
		{
			for (int x=29;x<=34;++x) { flatChunk(world,{x,z}); }
		}
		MapGenerator::Settlement settlement; settlement.center={32768,32768}; settlement.kind=MapGenerator::SettlementKind::RegionalCity;
		settlement.plan=UrbanMorphology::makePlan(UrbanMorphology::Origin::Castle,0,UrbanMorphology::Site{},42,true);
		RoadNetwork network; DistrictRoads::KaidoSegment kaido; kaido.passesThrough=true; kaido.dirAtCenter={1,0};
		DistrictRoads::generateSettlement(42,0,settlement,kaido,world,network);
		context.expect(settlement.plan.fringeStreets.size()>=24,U"Several exterior neighborhoods continue the old town streets");
		HashSet<int> depths;
		int frontage=0,connections=0,outer=0;
		double maximumDepth=0,nearDensity=0,farDensity=1;
		JSON report;
		int index=0;
		for (const auto& edge : network.edges())
		{
			if (edge.id<0) { continue; }
			const auto* a=network.getNode(edge.nodeA); const auto* b=network.getNode(edge.nodeB);
			if (!a || !b) { continue; }
			report[U"roads"][index++]=Array<double>{a->position.x-32768,a->position.z-32768,b->position.x-32768,b->position.z-32768};
		}
		for (const auto& street : settlement.plan.fringeStreets)
		{
			const Vec2 middle=(street.begin+street.end)*.5;
			const auto land=UrbanMorphology::sample(settlement.plan,middle);
			context.expect(land.district==UrbanMorphology::District::Housing && land.occupancy<.84,U"Outer streets have lower-density housing rather than an empty rectangular mask");
			depths.insert(static_cast<int>(Max(Abs(middle.x)-1000,Abs(middle.y)-1000)/40));
			frontage+=land.occupancy>0;
			const double depth=Max(Abs(middle.x)-1000,Abs(middle.y)-1000);
			maximumDepth=Max(maximumDepth,depth); outer+=depth>500;
			if (depth<150) { nearDensity=Max(nearDensity,land.occupancy); }
			if (depth>500) { farDensity=Min(farDensity,land.occupancy); }
		}
		for (const auto& node : network.nodes())
		{
			if (node.id<0) { continue; }
			const double x=Abs(node.position.x-32768),z=Abs(node.position.z-32768);
			if ((Abs(x-1000)<.1 || Abs(z-1000)<.1) && node.attachments.size()>=4) { ++connections; }
		}
		context.expect(depths.size()>=3 && connections>=8 && frontage>0,U"The urban edge has different depths and real connections into the core");
		context.expect(UrbanMorphology::sample(settlement.plan,{1400,1400}).occupancy==0,U"Fringe growth does not turn into a larger filled rectangle");
		context.expect(maximumDepth>600 && outer>=3,U"Residential streets extend through several belts outside the city");
		context.expect(nearDensity>.6 && farDensity<.32,U"Housing occupancy tapers along the real fringe streets");
		report[U"maximumDepth"]=maximumDepth; report[U"outerFrontages"]=outer;
		report[U"nearDensity"]=nearDensity; report[U"farDensity"]=farDensity;
		report[U"frontage"]=frontage; report[U"depthBands"]=static_cast<int>(depths.size()); report[U"connections"]=connections;
		report.save(U"TestResults/castle_transition.json");
	});
	runner.add(U"Morphology.AgriculturalAccess",[](TestContext& context)
	{
		World world; world.reserveChunks(); world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		for (const Point coord : {Point{32,32},Point{35,32}})
		{
			flatChunk(world,coord); world.getChunk(coord)->zoneMap.fill(ZoneType::Agriculture);
		}
		RoadNetwork network;
		const Vec3 a{32768,20,32843},b{33792,20,32843};
		const int start=network.addNode(a,NodeType::Endpoint),end=network.addNode(b,NodeType::Endpoint);
		network.addEdge(start,end,a.lerp(b,1.0/3),a.lerp(b,2.0/3),RoadType::LocalRoad,2);
		const auto stats=AgriculturalLayout::generate(world,network,42);
		context.expect(stats.fields>=12 && stats.tracks>0 && stats.drains==stats.tracks*2,U"Farm plots have shared access tracks and a channel at every plot");
		context.expect(world.getChunk(Point{35,32})->landPatches.isEmpty(),U"An isolated agricultural island is not filled with unreachable fields");
		JSON report; int index=0,gateCount=0;
		for (const auto& patch : world.getChunk(Point{32,32})->landPatches)
		{
			JSON item; item[U"type"]=static_cast<int>(patch.type);
			int vertex=0;
			for (const auto& p : patch.polygon) { item[U"points"][vertex++]=Array<double>{p.x-32768,p.y-32768}; }
			report[U"patches"][index++]=item;
			if (patch.type==LandPatchType::FarmField || patch.type==LandPatchType::PaddyField)
			{
				context.expect(patch.polygon.size()>=4,U"Fields fit the spaces between actual farm roads");
				gateCount+=(patch.materialVariant & AgriculturalLayout::kManagedField)!=0;
			}
		}
		context.expect(gateCount>0,U"Managed plots preserve their bank entrance marker");
		report[U"fields"]=stats.fields; report[U"tracks"]=stats.tracks; report[U"disconnected"]=stats.disconnected;
		report.save(U"TestResults/agricultural_access.json");
	});
	runner.add(U"Morphology.TownAlignedAgriculture",[](TestContext& context)
	{
		World world; world.reserveChunks(); world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		for (int y=32;y<=33;++y) { for (int x=32;x<=33;++x) { flatChunk(world,{x,y}); world.getChunk(Point{x,y})->zoneMap.fill(ZoneType::Agriculture); } }
		Array<AgriculturalLayout::Frame> frames;
		for (int i=0;i<2;++i)
		{
			AgriculturalLayout::Frame frame;
			const double angle=i==0 ? Math::Pi/6 : -Math::Pi/12;
			frame.center={33024+i*1536,33792}; frame.origin={32768+i*2048,32768};
			frame.axisX={Cos(angle),Sin(angle)}; frame.axisZ={-Sin(angle),Cos(angle)};
			frame.plotSize={144,28}; frames << frame;
		}
		RoadNetwork network;
		const Vec3 a{32768,20,33600},b{34816,20,33600};
		const int start=network.addNode(a,NodeType::Endpoint),end=network.addNode(b,NodeType::Endpoint);
		network.addEdge(start,end,a.lerp(b,1.0/3),a.lerp(b,2.0/3),RoadType::LocalRoad,2);
		const auto stats=AgriculturalLayout::generate(world,network,42,frames);
		context.expect(stats.fields>30 && stats.tracks>0,U"Both village surroundings retain fields alongside real roads");
		Array<Polygon> fields;int irregular=0;Array<Vec2> homes;
		for (int y=32;y<=33;++y) { for (int x=32;x<=33;++x)
		{
			const auto* chunk=world.getChunk(Point{x,y});
			for (int row=0;row<ZONE_CELLS;++row) { for (int col=0;col<ZONE_CELLS;++col)
			{
				const auto& building=chunk->buildingGrid[{col,row}];if (building.type==BuildingType::RuralHouse) { homes << Vec2{(x*ZONE_CELLS+col+.5)*16+building.offsetX,(y*ZONE_CELLS+row+.5)*16+building.offsetZ}; }
			} }
			for (const auto& patch:chunk->landPatches)
			{
				context.expect(patch.type!=LandPatchType::FarmTrack,U"Farm access is a road, never a land patch");
				if (patch.type==LandPatchType::PaddyField || patch.type==LandPatchType::FarmField) { fields << Polygon{patch.polygon};irregular+=patch.polygon.size()>4; }
			}
		} }
		bool overlap=false,served=true;
		for (size_t i=0;i<fields.size();++i)
		{
			bool nearby=false;for (const Vec2 home:homes) { bool all=true;for (const auto point:fields[i].outer()) { all &= point.distanceFrom(home)<=500; }nearby |= all; }served &= nearby;
			for (size_t j=i+1;j<fields.size();++j) { overlap |= fields[i].intersects(fields[j]); }
		}
		context.expect(irregular>5 && !overlap,U"Irregular road-following fields do not overlap at chunk or village boundaries");
		context.expect(served && !homes.isEmpty(),U"Every part of every field has a rural house within 500 m");
		JSON report;report[U"fields"]=stats.fields;report[U"roads"]=stats.tracks;report[U"irregular"]=irregular;report[U"homes"]=homes.size();report[U"overlap"]=overlap;report[U"served"]=served;report.save(U"TestResults/town_aligned_agriculture.json");
	});
	runner.add(U"Morphology.SuburbanOrigins",[](TestContext& context)
	{
		World world; world.reserveChunks(); world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		for (int y=29;y<=34;++y) { for (int x=29;x<=34;++x) { flatChunk(world,{x,y}); } }
		for (const auto origin : {UrbanMorphology::Origin::Post,UrbanMorphology::Origin::Market,UrbanMorphology::Origin::Planned})
		{
			MapGenerator::Settlement settlement; settlement.center={32768,32768}; settlement.kind=MapGenerator::SettlementKind::RegionalCity;
			settlement.plan=UrbanMorphology::makePlan(origin,0,UrbanMorphology::Site{},42,false);
			RoadNetwork network; DistrictRoads::KaidoSegment kaido; kaido.passesThrough=true; kaido.dirAtCenter={1,0};
			DistrictRoads::generateSettlement(42,0,settlement,kaido,world,network);
			if (origin==UrbanMorphology::Origin::Planned)
			{
				context.expect(settlement.plan.fringeStreets.isEmpty() && settlement.plan.greenways.size()>=24,U"New towns preserve their planned green boundary and internal pedestrian network");
			}
			else
			{
				context.expect(settlement.plan.fringeStreets.size()>=24,U"Connected suburbs still develop around historic post and market towns");
			}
		}
	});

	runner.add(U"Morphology.FarmRoadTerrainAndDensity",[](TestContext& context)
	{
		World world;world.reserveChunks();world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		for (int z=31;z<=34;++z) { for (int x=31;x<=34;++x)
		{
			Grid<float> heights(HEIGHT_CELLS+1,HEIGHT_CELLS+1);
			for (int row=0;row<=HEIGHT_CELLS;++row) { for (int col=0;col<=HEIGHT_CELLS;++col) { const double wx=x*CHUNK_SIZE+col*16,wz=z*CHUNK_SIZE+row*16;heights[row][col]=static_cast<float>(80+.012*(wz-33792)+3*Sin(wx/180)*Cos(wz/240)); } }
			world.installChunkDirect({x,z},HeightMapResult{heights,40,120});world.getChunk({x,z})->zoneMap.fill(ZoneType::Agriculture);
		} }
		RoadNetwork roads;for (int row=0;row<7;++row)
		{
			const double z=32000+row*500;const Vec3 a{31800,world.sampleHeight(31800,static_cast<float>(z)),z},b{35700,world.sampleHeight(35700,static_cast<float>(z)),z};
			const int first=roads.addNode(a),last=roads.addNode(b);roads.addEdge(first,last,a.lerp(b,1.0/3),a.lerp(b,2.0/3),RoadType::LocalRoad,2);
		}
		AgriculturalLayout::Frame frame;frame.center={33792,33792};const auto stats=AgriculturalLayout::generate(world,roads,42,{frame});
		HashSet<int> reachable;for (const auto& edge:roads.edges()) { if (edge.id>=0 && !edge.farmAccess) { reachable.insert(edge.nodeA);reachable.insert(edge.nodeB); } }
		for (int pass=0;pass<20;++pass) { for (const auto& edge:roads.edges()) { if (edge.id>=0 && (reachable.contains(edge.nodeA) || reachable.contains(edge.nodeB))) { reachable.insert(edge.nodeA);reachable.insert(edge.nodeB); } } }
		bool connected=true,grounded=true;int curved=0;double maximumGrade=0,nearArea=0,farArea=0;
		for (const auto& edge:roads.edges())
		{
			if (edge.id<0 || !edge.farmAccess) { continue; }connected &= reachable.contains(edge.nodeA) && reachable.contains(edge.nodeB);grounded &= !edge.useElevation && !edge.tunnel;
			const auto curve=*roads.getBezier(edge.id);curved+=(curve.p1-curve.p0).cross(curve.p3-curve.p0).length()>2;
			Vec3 previous=curve.positionAt(0);previous.y=world.sampleHeight(static_cast<float>(previous.x),static_cast<float>(previous.z));
			for (int i=1;i<=16;++i) { Vec3 p=curve.positionAt(curve.totalLength*i/16);p.y=world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z));maximumGrade=Max(maximumGrade,Abs(p.y-previous.y)/Vec2{p.x-previous.x,p.z-previous.z}.length());previous=p; }
		}
		for (int z=31;z<=34;++z) { for (int x=31;x<=34;++x) { for (const auto& patch:world.getChunk({x,z})->landPatches)
		{
			if (patch.type!=LandPatchType::FarmField && patch.type!=LandPatchType::PaddyField) { continue; }const Polygon polygon{patch.polygon};const double distance=polygon.centroid().distanceFrom(frame.center);
			if (distance<700) { nearArea+=polygon.area(); }if (distance>1200 && distance<1900) { farArea+=polygon.area(); }
		} } }
		const double nearDensity=nearArea/(Math::Pi*700*700),farDensity=farArea/(Math::Pi*(1900*1900-1200*1200));
		context.expect(stats.fields>50 && connected && grounded,U"Actual farm roads form a drivable ground network connected to existing roads");
		context.expect(curved>5 && maximumGrade<.061,U"Farm branches bend with terrain without exceeding the farm-road gradient");
		context.expect(nearDensity>farDensity*1.3 && farArea>0,U"Field coverage decreases outward from the village, measured per unit area");
		JSON report;
		report[U"fields"] = stats.fields;
		report[U"curvedRoads"] = curved;
		report[U"connections"] = stats.connections;
		report[U"maximumGrade"] = maximumGrade;
		report[U"nearDensity"] = nearDensity;
		report[U"farDensity"] = farDensity;
		report[U"connected"] = connected;
		report.save(U"TestResults/farm_terrain_density.json");
	});

}
