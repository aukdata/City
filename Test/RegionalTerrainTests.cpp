#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/world/World.hpp"
#include "src/gen/RailwayAlignment.hpp"
#include "src/railway/RailTimetable.hpp"

namespace
{
	constexpr int kCells=256;
	constexpr double kStep=static_cast<double>(WORLD_SIZE)/kCells;

	/// @brief 実際の高さを測り、中央の適地・外周の海山・平野の起伏と海岸線長を記録する。
	JSON surveyRegion(uint64 seed)
	{
		World world; world.setGenerationParams(seed,WORLD_SIZE,WORLD_SIZE);
		Grid<float> heights(kCells+1,kCells+1);
		BinaryWriter raw{U"TestResults/terrain_{}.f32"_fmt(seed)};
		for (int z=0;z<=kCells;++z) { for (int x=0;x<=kCells;++x)
		{
			const float height=world.computeHeight(static_cast<float>(x*kStep),static_cast<float>(z*kStep));
			heights[{x,z}]=height; raw.write(height);
		} }
		Grid<uint8> ocean(kCells+1,kCells+1,0);
		Array<Point> pending;
		for (int z=0;z<=kCells;++z) { for (int x=0;x<=kCells;++x)
		{
			if ((x==0 || x==kCells || z==0 || z==kCells) && heights[{x,z}]<0)
			{
				ocean[{x,z}]=1; pending << Point{x,z};
			}
		} }
		for (size_t i=0;i<pending.size();++i)
		{
			for (const Point direction : {Point{-1,0},Point{1,0},Point{0,-1},Point{0,1}})
			{
				const Point next=pending[i]+direction;
				if (next.x<0 || next.y<0 || next.x>kCells || next.y>kCells || ocean[next] || heights[next]>=0) { continue; }
				ocean[next]=1; pending << next;
			}
		}
		int core=0,coreDry=0,coreWater=0,coreMountain=0,edge=0,edgeBarrier=0,lake=0;
		Array<double> relief;
		for (int z=2;z<kCells-1;++z) { for (int x=2;x<kCells-1;++x)
		{
			const double h=heights[{x,z}];
			const double slope=Max(Abs(heights[{x+1,z}]-heights[{x-1,z}]),Abs(heights[{x,z+1}]-heights[{x,z-1}]))/(2*kStep);
			if (x>=64 && x<=192 && z>=64 && z<=192)
			{
				++core; coreDry+=h>3.2 && h<150 && slope<.035; coreWater+=h<0; coreMountain+=h>600;
				lake+=h<0 && !ocean[{x,z}] && world.getBiome(static_cast<float>(x*kStep),static_cast<float>(z*kStep))==BiomeType::Lake;
			}
			if (x<32 || x>224 || z<32 || z>224) { ++edge; edgeBarrier+=h<0 || h>300; }
			if (h>8 && h<150 && slope<.02)
			{
				double low=h,high=h;
				for (const Point offset : {Point{-2,0},Point{2,0},Point{0,-2},Point{0,2}})
				{
					const double sample=heights[Point{x,z}+offset]; low=Min(low,sample); high=Max(high,sample);
				}
				if (low>3.2 && high<180) { relief << high-low; }
			}
		} }
		double shoreLength=0; Vec2 lower{1e30,1e30},upper{-1e30,-1e30};
		for (int z=0;z<kCells;++z) { for (int x=0;x<kCells;++x)
		{
			const Array<Point> corners{{x,z},{x+1,z},{x+1,z+1},{x,z+1}};
			Array<Vec2> crossings;
			for (size_t side=0;side<4;++side)
			{
				const Point a=corners[side],b=corners[(side+1)%4];
				const double first=heights[a],last=heights[b];
				if ((first<0)==(last<0) || !(ocean[a] || ocean[b])) { continue; }
				const double t=first/(first-last);
				const Vec2 point=(Vec2{a.x,a.y}+(Vec2{b.x,b.y}-Vec2{a.x,a.y})*t)*kStep;
				crossings << point;
				lower.x=Min(lower.x,point.x); lower.y=Min(lower.y,point.y); upper.x=Max(upper.x,point.x); upper.y=Max(upper.y,point.y);
			}
			for (size_t i=1;i<crossings.size();i+=2) { shoreLength+=crossings[i-1].distanceFrom(crossings[i]); }
		} }
		relief.sort();
		JSON result; result[U"seed"]=seed;
		result[U"upliftedCells"]=world.terrainEvolution().uplifted;
		result[U"erodedCells"]=world.terrainEvolution().eroded;
		result[U"depositedCells"]=world.terrainEvolution().deposited;
		result[U"upliftMetres"]=world.terrainEvolution().upliftMetres;
		result[U"erosionMetres"]=world.terrainEvolution().erosionMetres;
		result[U"depositionMetres"]=world.terrainEvolution().depositionMetres;
		result[U"coreDry"]=static_cast<double>(coreDry)/core;
		result[U"coreWater"]=static_cast<double>(coreWater)/core; result[U"coreMountain"]=static_cast<double>(coreMountain)/core;
		result[U"edgeBarrier"]=static_cast<double>(edgeBarrier)/edge;
		result[U"plainRelief1024"]=relief.isEmpty() ? 0 : relief[relief.size()/2];
		result[U"shoreLengthKm"]=shoreLength/1000;
		result[U"shoreSinuosity"]=shoreLength/Max(1.0,upper.distanceFrom(lower));
		result[U"inlandLakeKm2"]=lake*kStep*kStep/1000000;
		return result;
	}
}

void registerRegionalTerrainTests(TestRunner& runner)
{
	runner.add(U"Terrain.UnreachableStationKeepsUsableRailway",[](TestContext& context)
	{
		World world; world.reserveChunks(); world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		for (int z=0;z<16;++z) { for (int x=0;x<16;++x)
		{
			Grid<float> heights(HEIGHT_CELLS+1,HEIGHT_CELLS+1,20);
			for (int row=0;row<=HEIGHT_CELLS;++row) { for (int col=0;col<=HEIGHT_CELLS;++col)
			{
				const Vec2 point{x*CHUNK_SIZE+col*16.0,z*CHUNK_SIZE+row*16.0};
				heights[{col,row}]=static_cast<float>(20+1800*Max(0.0,1-point.distanceFrom(Vec2{4000,4000})/300));
			} }
			world.installChunkDirect({x,z},HeightMapResult{heights,20,1820});
		} }
		Array<MapGenerator::Settlement> towns;
		for (int index=0;index<3;++index)
		{
			MapGenerator::Settlement town;
			town.center={index==0 ? 4000.0 : 6000.0+index*1000,4000};
			town.gridAxisX={1,0}; town.gridAxisZ={0,1}; town.plan.station=Vec2{0,0};
			town.name=U"駅候補{}"_fmt(index); towns << town;
		}
		TrainNetwork network;
		RailwayAlignment::generate(network,world,towns);
		Array<int> stations;
		for (const auto& node : network.nodes()) { if (node.type==TrackNodeType::Station) { stations << node.id; } }
		context.expectEqual(stations.size(),size_t{2},U"An unreachable first candidate does not prevent the two lowland stations from being built");
		context.expectEqual(network.schedules().size(),size_t{1},U"The feasible railway receives a default timetable");
		if (stations.size()==2) { context.expect(!network.findRoute(stations[0],stations[1]).isEmpty(),U"Built stations are connected, without an isolated phantom station on the mountain"); }
		for (const auto& schedule : network.schedules()) { context.expect(RailTimetable::validate(network,schedule).isEmpty(),U"Generated partial railways retain valid editable timetables"); }
	});
	runner.add(U"Terrain.RegionalComposition",[](TestContext& context)
	{
		const Array<uint64> seeds{7,42,130,2026,0,1,2,3,4,5,6,8,9,10,11,12,13,14,15,16};
		JSON report; double central=0,periphery=0,roughness=0,coast=0;
		int centralPlains=0,centralMountains=0,centralBays=0,inlandLakes=0;
		for (size_t i=0;i<seeds.size();++i)
		{
			const auto result=surveyRegion(seeds[i]); report[U"seeds"][i]=result;
			context.expect(result[U"upliftedCells"].get<int>()>0 && result[U"erodedCells"].get<int>()>0
				&& result[U"depositedCells"].get<int>()>0,U"Terrain receives uplift, stream erosion and downstream deposition");
			central+=result[U"coreDry"].get<double>(); periphery+=result[U"edgeBarrier"].get<double>();
			roughness+=result[U"plainRelief1024"].get<double>(); coast+=result[U"shoreSinuosity"].get<double>();
			centralPlains+=result[U"coreDry"].get<double>()>.5;
			centralMountains+=result[U"coreMountain"].get<double>()>.08;
			centralBays+=result[U"coreWater"].get<double>()>.08;
			inlandLakes+=result[U"inlandLakeKm2"].get<double>()>1;
		}
		const double count=static_cast<double>(seeds.size());
		report[U"meanCoreDry"]=central/count; report[U"meanEdgeBarrier"]=periphery/count;
		report[U"meanPlainRelief1024"]=roughness/count; report[U"meanShoreSinuosity"]=coast/count;
		report[U"centralPlains"]=centralPlains; report[U"centralMountains"]=centralMountains;
		report[U"centralBays"]=centralBays; report[U"inlandLakes"]=inlandLakes;
		report.save(U"TestResults/terrain_composition.json");
		context.expect(central / count > .35 && centralPlains >= 3,
			U"Interior hills leave developable valleys without requiring one broad central plain");
		context.expect(periphery/count>.55,U"Map edges tend to be mountains or sea without a compulsory border wall");
		context.expect(roughness/count>5 && roughness/count<24,U"Plains contain gentle kilometre-scale undulations while remaining buildable");
		context.expect(coast/count>1.6,U"Ocean shores have real inlets and capes beyond a nearly straight boundary");
		context.expect(centralMountains>0 && centralBays>0 && inlandLakes>0,U"Central mountains, large bays and inland lakes remain possible");
	});
}
