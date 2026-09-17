#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/zone/ZoneManager.hpp"
#include "src/road/RoadPlanDraft.hpp"
#include "src/ui/ZonePalette.hpp"

namespace
{
	void flatWorld(World& world, float height=10)
	{
		world.reserveChunks();
		for (int x=0;x<2;++x) { world.installChunkDirect({x,0},HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,height),height,height}); }
	}
	int road(RoadNetwork& network, double z=512)
	{
		const int a=network.addNode({400,10,z}),b=network.addNode({1120,10,z});
		const int id=*network.addEdge(a,b,{640,10,z},{880,10,z});
		network.applyEdgeTemplate(id,RoadPlanDraft::makeRoadTemplate(0)); network.getEdge(id)->edgeState=EdgeState::Open; return id;
	}
	ZoneDevelopmentResult advance(ZoneManager& manager, World& world, RoadNetwork& network, int seconds)
	{
		ZoneDevelopmentResult total;
		for (int i=0;i<seconds;++i)
		{
			const auto step=manager.updateDevelopment(world,network,{.1,.1,.1},1,i+1);
			total.buildings+=step.buildings; total.housing+=step.housing;
		}
		return total;
	}
}
void registerZoneDevelopmentTests(TestRunner& runner)
{
	runner.add(U"Zoning.RoadsideGrowthAndRepeatedBrush",[](TestContext& context)
	{
		World world;flatWorld(world);RoadNetwork network;const int id=road(network);ZoneManager manager;
		for (int second=0;second<40;++second)
		{
			manager.paintZone(world,{504,10,520},ZoneType::LowResidential,0);
			manager.updateDevelopment(world,network,{.1,.1,.1},1,second+1);
		}
		const auto* chunk=world.getChunk({0,0});const Building& home=chunk->buildingGrid[{31,32}];
		context.expect(home.type==BuildingType::Detached,U"A painted plot develops even at the minimum real city demand");
		context.expectEqual(home.edgeId,id,U"The completed building uses its actual frontage road");
		context.expectNear(home.angle,0,.02,U"The entrance faces the road from its north side");
		context.expect(home.offsetZ>0 && home.offsetZ<=5,U"A modest setback keeps the frontage clear");
		context.expect(!chunk->landPatches.isEmpty() && chunk->landPatches.front().polygon.size()>=3,U"A garden connects the new home to its road");
		context.expectEqual(manager.developmentSummary().completed,1,U"Holding the brush neither resets progress nor duplicates buildings");
		context.expect(chunk->meshDirty,U"The visible city is refreshed after construction");
	});
	runner.add(U"Zoning.PauseCancelAndPreserveBuildings",[](TestContext& context)
	{
		World world;flatWorld(world);RoadNetwork network;road(network);ZoneManager manager;
		manager.paintZone(world,{504,10,520},ZoneType::Residential,0);
		for (int i=0;i<60;++i) { manager.updateDevelopment(world,network,{1,1,1},0,1000); }
		context.expectEqual(manager.developmentSummary().completed,0,U"Calendar time and pause cannot advance construction");
		manager.paintZone(world,{504,10,520},ZoneType::Unzoned,0);
		context.expectEqual(advance(manager,world,network,60).buildings,0,U"Removing the zone cancels its pending development");
		manager.paintZone(world,{504,10,520},ZoneType::Residential,0);
		context.expectEqual(advance(manager,world,network,40).buildings,1,U"Repainting starts a fresh project");
		const BuildingType built=world.getChunk({0,0})->buildingGrid[{31,32}].type;
		manager.paintZone(world,{504,10,520},ZoneType::Industrial,0);
		advance(manager,world,network,60);
		context.expect(world.getChunk({0,0})->buildingGrid[{31,32}].type==built,U"Rezoning never silently demolishes an occupied site");
	});
	runner.add(U"Zoning.RoadOpeningAndNoElevatedAccess",[](TestContext& context)
	{
		World world;flatWorld(world);RoadNetwork network;const int id=road(network);ZoneManager manager;
		manager.paintZone(world,{504,10,520},ZoneType::LowResidential,0);
		for (int state=0;state<4;++state)
		{
			auto* edge=network.getEdge(id); edge->edgeState=state==0 ? EdgeState::UnderConstruction : EdgeState::Open;
			edge->useElevation=state==1; edge->tunnel=state==2;
			edge->roadType=state==3 ? RoadType::Expressway : RoadType::LocalRoad;
			context.expectEqual(advance(manager,world,network,40).buildings,0,U"Unfinished roads, bridges, tunnels and expressways do not provide plot access");
		}
		auto* edge=network.getEdge(id); edge->useElevation=false; edge->tunnel=false; edge->roadType=RoadType::LocalRoad;
		context.expectEqual(manager.developmentSummary().needsRoad,1,U"The player is told that an accessible road is missing");
		context.expectEqual(advance(manager,world,network,40).buildings,1,U"Opening the street starts development without repainting");
	});
	runner.add(U"Zoning.WaterSlopeRoadAndNeighbourClearance",[](TestContext& context)
	{
		for (int scenario=0;scenario<4;++scenario)
		{
			World world;flatWorld(world,scenario==0 ? 0.0f : 10.0f);RoadNetwork network;road(network,scenario==2 ? 520 : 512);ZoneManager manager;
			if (scenario==1)
			{
				auto* chunk=world.getChunk({0,0});
				for (int z=0;z<=HEIGHT_CELLS;++z) for (int x=0;x<=HEIGHT_CELLS;++x) { chunk->heightMap[{x,z}]=10.0f+(z-32)*4.0f; }
			}
			const Vec3 plot=scenario==3 ? Vec3{1016,10,520} : Vec3{504,10,520};
			if (scenario==3)
			{
				auto& neighbour=world.getChunk({1,0})->buildingGrid[{0,32}];
				neighbour.type=BuildingType::Detached; neighbour.offsetX=-10;
			}
			manager.paintZone(world,plot,ZoneType::LowResidential,0);
			context.expectEqual(advance(manager,world,network,50).buildings,0,U"Water, steep slopes, pavement and an overlapping neighbour remain free of new buildings");
			const auto status=manager.developmentSummary();
			context.expect(scenario<2 ? status.unsuitableTerrain==1 : status.needsSpace==1,U"A blocked plot reports the measured terrain or space reason");
		}
	});
	runner.add(U"Zoning.SaveResumeAndFieldAccess",[](TestContext& context)
	{
		World world;flatWorld(world);RoadNetwork network;road(network);ZoneManager manager;
		manager.paintZone(world,{504,10,520},ZoneType::LowResidential,0);advance(manager,world,network,17);
		const JSON snapshot=manager.developmentSnapshot();
		context.expect(snapshot.save(U"TestResults/zoning_progress.json"),U"A developing plot can be saved");
		ZoneManager loaded;loaded.restoreDevelopment(JSON::Load(U"TestResults/zoning_progress.json"),world);
		context.expectEqual(advance(loaded,world,network,19).buildings,1,U"Reload resumes the saved progress instead of restarting or abandoning it");
		loaded.paintZone(world,{600,10,520},ZoneType::Agriculture,0);
		const auto field=advance(loaded,world,network,40);
		context.expectEqual(field.buildings,1,U"A flat road-accessible plot becomes farmland");
		context.expectEqual(field.housing,0,U"Fields cannot create residents");
		loaded.paintZone(world,{696,10,600},ZoneType::UrbanControl,0);
		context.expectEqual(advance(loaded,world,network,40).buildings,0,U"Urban control preserves land rather than spawning arbitrary buildings");
	});
	runner.add(U"Zoning.EditedCitySurvivesRegeneration",[](TestContext& context)
	{
		World world;flatWorld(world);RoadNetwork network;road(network);ZoneManager manager;
		manager.paintZone(world,{504,10,520},ZoneType::LowResidential,0);advance(manager,world,network,40);
		manager.paintZone(world,{600,10,520},ZoneType::Commercial,0);advance(manager,world,network,17);
		manager.paintZone(world,{696,10,520},ZoneType::Unzoned,0);
		const JSON snapshot=manager.saveState(world);
		World regenerated;flatWorld(regenerated);
		regenerated.getChunk({0,0})->zoneMap[{43,32}]=ZoneType::Residential;
		regenerated.getChunk({0,0})->buildingGrid[{43,32}].type=BuildingType::Detached;
		ZoneManager loaded;loaded.restoreState(snapshot,regenerated);
		context.expect(regenerated.getChunk({0,0})->buildingGrid[{31,32}].type==BuildingType::Detached,U"A newly built home survives procedural world reconstruction");
		context.expect(regenerated.getChunk({0,0})->buildingGrid[{43,32}].type==BuildingType::None && regenerated.getChunk({0,0})->zoneMap[{43,32}]==ZoneType::Unzoned,U"An erased zone stays empty on reload");
		context.expectEqual(advance(loaded,regenerated,network,19).buildings,1,U"A developing shop resumes after the edited zoning has been restored");
		ZoneManager secondLoad;secondLoad.restoreState(loaded.saveState(regenerated),world);
		context.expect(world.getChunk({0,0})->buildingGrid[{37,32}].type!=BuildingType::None,U"Subsequent saves retain both generations of player development");
	});
	runner.add(U"Zoning.RailwayCorridor",[](TestContext& context)
	{
		World world;flatWorld(world);RoadNetwork network;road(network);TrainNetwork railway;ZoneManager manager;
		manager.paintZone(world,{504,10,520},ZoneType::LowResidential,0);
		manager.updateDevelopment(world,network,{.1,.1,.1},1,1,&railway);
		const int a=railway.addNode({400,10,524}),b=railway.addNode({700,10,524});
		railway.addEdge(a,b,{500,10,524},{600,10,524});
		for (int i=0;i<40;++i) { manager.updateDevelopment(world,network,{.1,.1,.1},1,i+2,&railway); }
		context.expectEqual(manager.developmentSummary().completed,0,U"Adding a railway stops construction inside its corridor before completion");
		context.expectEqual(manager.developmentSummary().needsSpace,1,U"Railway clearance uses the same reserved space as the initial city");
	});
	runner.add(U"Zoning.NewHomePartitionsOldGarden",[](TestContext& context)
	{
		World world;flatWorld(world);RoadNetwork network;road(network);ZoneManager manager;
		auto* chunk=world.getChunk({0,0});chunk->buildingGrid[{33,32}].type=BuildingType::Detached;
		LandPatch garden;garden.sourceParcelKey=(32<<8)|33;garden.type=LandPatchType::GardenSoil;
		garden.polygon={{496,516},{550,516},{550,540},{496,540}};chunk->landPatches << garden;
		manager.paintZone(world,{504,10,520},ZoneType::LowResidential,0);
		context.expectEqual(advance(manager,world,network,40).buildings,1,U"An unused frontage can grow beside an existing home");
		const auto& oldGarden=chunk->landPatches.front().polygon;
		context.expect(oldGarden.size()>=3,U"The neighbour keeps a valid garden");
		const Vec2 oldCenter{536,520},newCenter{504,520+chunk->buildingGrid[{31,32}].offsetZ};
		bool separate=true;
		for (const Vec2 point : oldGarden) { separate=separate && point.distanceFromSq(oldCenter)<point.distanceFromSq(newCenter); }
		context.expect(separate,U"Old garden and fences are trimmed away from the new plot instead of passing through its house");
	});
	runner.add(U"Zoning.PaletteVisualReview",[](TestContext& context)
	{
		const Font font{FontMethod::MSDF,14},bold{FontMethod::MSDF,14,Typeface::Bold};
		const int width=ZonePalette::kWidth;
		RenderTexture target{Size{width,ZonePalette::kHeight}};
		for (int selection=0;selection<7;++selection)
		{
			ZonePalette::State state;state.zone=static_cast<ZoneType>(selection);state.brushRadius=selection%3;
			state.demand={.1,.55,1};state.development={1,12,8,3,1,5};state.paused=selection==0;
			{
				const ScopedRenderTarget2D scope{target.clear(ColorF{.055,.075,.09})};
				ZonePalette::draw(font,bold,width,state);
			}
			Graphics2D::Flush();Image image;target.readAsImage(image);
			context.expect(image.save(U"Screenshot/zone_palette_{}.png"_fmt(selection)),U"Render each palette state for local review before integration");
			context.expect(font(ZonePalette::description(state.zone)).region().w<=width-16,U"Zone explanations fit without clipping");
			int visible=0;
			for (int y=0;y<image.height();++y) for (int x=0;x<image.width();++x) { const auto pixel=image[y][x]; if (pixel.r>100 || pixel.g>100 || pixel.b>100) { ++visible; } }
			context.expect(visible>6000,U"GPU readback contains the actual controls and legible contrasting text");
		}
		for (int i=0;i<7;++i)
		{
			const auto rect=ZonePalette::zoneButton(i,width);
			context.expect(rect.x>=0 && rect.br().x<=width && rect.y>=0 && rect.br().y<=172,U"Every zone has an in-bounds hit target");
			for (int j=i+1;j<7;++j) { context.expect(!rect.intersects(ZonePalette::zoneButton(j,width)),U"Adjacent zone targets never overlap"); }
		}
		context.expect(bold(U"一時停止中 — 再開すると開発が進みます").region().w<=width-16,U"Pause guidance fits the panel");
	});
}
