#include <cstring>
#include <sstream>
#include "TestRunner.hpp"
#include "src/sim/SourceWater.hpp"
#include "src/world/World.hpp"
#include "src/gen/LeveeRoad.hpp"
#include "src/render/SourceWaterGeometry.hpp"
#include "src/ui/MapTerrainLayer.hpp"
#include "src/render/ShaderAsset.hpp"

/// @brief Sink-only sea, exact conservation, snapshot recovery and source-only game generation.
void registerSourceWaterTests(TestRunner& runner)
{
	runner.add(U"SourceWater.SignedDryBanks",[](TestContext& context)
	{
		SourceWater water{512,256,[](double,double) { return 0.0; },[](double,double) { return false; },{},128};
		water.configureChannels({{64,128,448,128,10,10,40,40,2,-2,64}});
		context.expect(water.sample(256,128,0).depth<0,U"A dry configured river cannot acquire visible water from bank smoothing");
		water.advance(6000);
		const auto center=water.sample(256,128,0),bank=water.sample(256,164,0),high=water.sample(256,152,12);
		context.expect(center.depth>0,U"The supplied river still has visible water");
		context.expectNear(bank.head,center.head,1e-12,U"The dry-bank support retains the same continuous hydraulic plane");
		context.expect(bank.depth<=-8,U"A low depression outside the actual river width cannot become water");
		context.expectNear(high.depth,high.head-12,1e-12,U"High dry banks retain true signed terrain depth instead of a 5 cm shelf");
		context.expectNear(water.inflow(),0,0,U"Smoothing cannot introduce sea backflow");
	});
	runner.add(U"SourceWater.HigherDischargeFillsFaster",[](TestContext& context)
	{
		const auto height=[](double,double) { return 0.0; };const auto marine=[](double,double) { return false; };
		const std::vector<SourceWater::Channel> channels={
			{0,50,100,50,10,9,20,20,2,2,4},{50,0,100,50,10,9,20,20,2,2,6},{100,50,200,50,9,8,30,30,2,-2,0}};
		auto stronger=channels;for (auto& c:stronger) { c.inlet*=4; }
		SourceWater normal{512,128,height,marine,{},128},fast{512,128,height,marine,{},128};
		normal.configureChannels(channels);fast.configureChannels(stronger);
		for (std::size_t i=0;i<channels.size();++i)
		{
			context.expectNear(fast.channelResidence(i)*4,normal.channelResidence(i),1e-12,U"Higher source discharge shortens residence without increasing design depth");
		}
		for(int step=0;step<200;++step) { normal.advance(24);fast.advance(6); }
		for (std::size_t i=0;i<channels.size();++i)
		{
			context.expectNear(fast.channelDepth(i),normal.channelDepth(i),1e-10,U"The same river stage arrives in one quarter of the physical time");
			context.expectNear(fast.channelFlow(i),normal.channelFlow(i)*4,1e-9,U"Water actually flows faster instead of only changing an animation clock");
		}
		context.expectNear(fast.ownedVolume(),normal.ownedVolume(),1e-8,U"Both river states hold the same real source-supplied volume");
		context.expectNear(fast.injected(),normal.injected(),1e-8,U"Equal source volume is supplied in a shorter time");
		context.expectNear(fast.balanceError(),0,1e-8,U"Stronger flow still conserves water through confluence and sea drainage");
		context.expectNear(fast.inflow(),0,0,U"Faster filling does not use water from the sea");
	});
	runner.add(U"SourceWater.RiverConfluenceAndDrying",[](TestContext& context)
	{
		const auto height=[](double,double) { return 0.0; };const auto marine=[](double,double) { return false; };
		const std::vector<SourceWater::Channel> channels={
			{0,50,100,50,10,9,20,20,2,2,4},
			{50,0,100,50,10,9,20,20,2,2,6},
			{100,50,200,50,9,8,30,30,2,-2,0}};
		SourceWater water{512,128,height,marine,{},128};water.configureChannels(channels);
		context.expectNear(water.ownedVolume(),0,0,U"Every riverbed starts completely dry");
		context.expectNear(water.channelFlow(2),0,0,U"A painted river cannot flow before source water arrives");
		water.advance(6);context.expect(water.channelDepth(0)>water.channelDepth(2),U"Water begins at the source and fills the downstream reach later");
		context.expectNear(water.injected(),60,1e-10,U"Only the two point sources supply water");
		for (int i=0;i<2000;++i) { water.advance(6); }
		const double initial=water.ownedVolume();
		context.expectNear(water.channelFlow(2),10,.001,U"Continued point supply eventually joins both tributaries and reaches the sea");
		context.expectNear(water.inflow(),0,0,U"Sea boundary cannot send water upstream");
		context.expectNear(water.balanceError(),0,1e-8,U"All water is from sources and equals storage plus sea drain");
		water.setSupplyScale(0);for (int i=0;i<2000;++i) { water.advance(6); }
		context.expect(water.ownedVolume()<initial*.002 && water.channelFlow(2)<.02,U"Switching sources off drains the real stored river water");
		context.expectNear(water.balanceError(),0,1e-7,U"Source-off recession still conserves water");
		std::ostringstream snapshot{std::ios::binary};context.expect(water.write(snapshot),U"Routed river snapshot is written");
		SourceWater restored{512,128,height,marine,{},128};restored.configureChannels(channels);
		std::istringstream input{snapshot.str(),std::ios::binary};context.expect(restored.read(input),U"Routed river snapshot restores after validation");
		std::istringstream exact{snapshot.str(),std::ios::binary};context.expect(restored.read(exact,false),U"Restored stage, discharge and ledger are exact");
		water.advance(60);restored.advance(60);context.expectNear(water.ownedVolume(),restored.ownedVolume(),0,U"Restored river continues identically");
		const auto before=restored.ownedVolume();std::istringstream corrupt{snapshot.str().substr(0,snapshot.str().size()-2),std::ios::binary};
		context.expect(!restored.read(corrupt) && restored.ownedVolume()==before,U"Incomplete river state cannot mutate a live world");
	});
	runner.add(U"SourceWater.SettledStepsMatchFullSteps",[](TestContext& context)
	{
		const auto height=[](double,double) { return 0.0; };const auto marine=[](double,double) { return false; };
		const std::vector<SourceWater::Channel> channels={
			{0,50,100,50,10,9,20,20,2,2,4},
			{50,0,100,50,10,9,20,20,2,2,6},
			{100,50,200,50,9,8,30,30,2,-2,0}};
		SourceWater fast{512,128,height,marine,{},128},full{512,128,height,marine,{},128};
		fast.configureChannels(channels);full.configureChannels(channels);
		const auto snapshot=[](const SourceWater& water) { std::ostringstream out{std::ios::binary};water.write(out);return out.str(); };
		bool settled=false;
		for (int step=0;step<6000;++step)
		{
			fast.advance(6);settled|=fast.channelsSettled();
			// Restoring a snapshot clears the settled state, so this copy always runs the full routing.
			std::istringstream input{snapshot(full),std::ios::binary};full.read(input);full.advance(6);
		}
		context.expect(settled,U"Steady routed flow reaches a bit-identical fixed point");
		context.expect(snapshot(fast)==snapshot(full),U"Skipping settled steps leaves volumes, flows and ledgers bit-identical");
		fast.setSupplyScale(.5);full.setSupplyScale(.5);
		for (int step=0;step<50;++step) { fast.advance(6);std::istringstream input{snapshot(full),std::ios::binary};full.read(input);full.advance(6); }
		context.expect(snapshot(fast)==snapshot(full),U"A supply change resumes full routing");
	});
	runner.add(U"SourceWater.ClosedRiverAndCycleValidation",[](TestContext& context)
	{
		SourceWater water{512,128,[](double,double) { return 0.0; },[](double,double) { return false; },{},128};
		bool rejected=false;try { water.configureChannels({{0,0,100,0,10,9,20,20,2,0,4}}); }
		catch (const std::invalid_argument&) { rejected=true; }
		context.expect(rejected && water.channels().empty(),U"A cyclic channel graph is rejected before mutation");
		water.configureChannels({{0,0,100,0,10,9,20,20,2,-1,4}});const double initial=water.ownedVolume();
		water.advance(1000);context.expectNear(water.ownedVolume(),initial+4000,1e-8,U"An inland dead end retains water rather than silently removing it");
		context.expectNear(water.outflow(),0,0,U"Only an actual sea outlet is a drain");
	});
	runner.add(U"SourceWater.DrainWithoutBackflow",[](TestContext& context)
	{
		// A dry depression is lower than the sea but must never receive prescribed ocean water.
		SourceWater water{512,128,[](double x,double) { return x<384 ? -4.0 : -10.0; },
			[](double x,double) { return x>=384; },{},128};
		water.advance(600);
		context.expectNear(water.ownedVolume(),0,0,U"Dry below-sea land cannot be flooded by the drain");
		context.expectNear(water.inflow(),0,0,U"Ocean exchange is strictly one-way");
		context.expectNear(water.sample(448,64,-10).head,0,0,U"Sea level remains exactly zero");
		context.expect(!water.sample(64,64,-4).ocean,U"An isolated low depression is not classified as sea");
	});
	runner.add(U"SourceWater.InletAndSeaLedger",[](TestContext& context)
	{
		SourceWater water{1024,384,[](double x,double) { return (896-x)*0.003-0.2; },
			[](double x,double) { return x>=896; },{{64,192,64}},128};
		water.advance(18000);
		context.expectNear(water.grid().injected,64*18000,1e-6,U"Only point sources inject the prescribed rate");
		context.expect(water.outflow()>1000,U"Water reaches the sea drain along unchanged downhill terrain");
		context.expect(water.ownedVolume()>0,U"Land retains finite flowing water");
		context.expectNear(water.inflow(),0,0,U"Sea never emits water back onto land");
		context.expectNear(water.balanceError(),0,0.00001,U"Source input equals stored water plus ocean loss");
		for (int z=0;z<3;++z)
		{
			const auto i=water.grid().index(7,z);
			context.expectNear(water.grid().bed[i]+water.grid().depth[i],0,1e-14,U"Each ocean cell keeps its fixed head after substeps");
		}
		context.expect(water.hasLandWater(0,0,896,384),U"Water remains owned by the whole map independently of its view");
	});
	runner.add(U"SourceWater.SaveRestoreAndTerrainEdit",[](TestContext& context)
	{
		const auto height=[](double x,double) { return (384-x)*0.01; };
		const auto marine=[](double x,double) { return x>=384; };
		SourceWater water{512,128,height,marine,{{64,64,64}},128};
		water.advance(6000);const double volume=water.ownedVolume();
		water.setBed(1,0,8);
		context.expectNear(water.ownedVolume(),volume,1e-9,U"Raising terrain keeps owned water instead of silently deleting it");
		std::ostringstream saved{std::ios::binary};context.expect(water.write(saved),U"Snapshot writes the exact binary water state");
		SourceWater restored{512,128,height,marine,{{64,64,64}},128};
		std::istringstream input{saved.str(),std::ios::binary};context.expect(restored.read(input),U"Valid snapshot restores");
		context.expect(restored.grid().bed==water.grid().bed && restored.grid().depth==water.grid().depth,U"Every bed and depth is restored bit-for-bit");
		context.expectNear(restored.outflow(),water.outflow(),0,U"Historical drain loss survives loading");
		water.advance(60);restored.advance(60);
		context.expect(restored.grid().depth==water.grid().depth,U"Resumed evolution matches uninterrupted evolution");
		const auto before=restored.grid().depth;
		std::istringstream corrupt{saved.str().substr(0,40),std::ios::binary};
		context.expect(!restored.read(corrupt) && restored.grid().depth==before,U"A truncated save is rejected without changing live state");
		std::string invalid=saved.str();invalid.push_back('x');std::istringstream trailing{invalid,std::ios::binary};
		context.expect(!restored.read(trailing),U"Unexpected snapshot bytes are rejected");
	});
	runner.add(U"SourceWater.GameSourceOnlyAndPause",[](TestContext& context)
	{
		World world;world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);

		world.generateSourceWater();
		const auto* water=world.sourceWater();
		context.expect(water && water->sourceCount()>0 && water->oceanCells()>0,U"The actual seeded game map generates sources and ocean drains");
		context.expect(!world.rivers().reaches.isEmpty() && water->channels().size()==world.rivers().reaches.size(),U"River morphology and conservative routing share the same connected reaches");
		const auto& first=world.rivers().reaches.front();
		context.expect(world.computeHeight(static_cast<float>(first.start.x),static_cast<float>(first.start.z))<first.start.y,U"The actual terrain contains a riverbed below the water");
		context.expectNear(water->ownedVolume(),0,0,U"The actual game starts with no inland water");
		const auto midpoint=first.start.lerp(first.end,.5);
		context.expectNear(world.waterSurfaceHeight(midpoint.x,midpoint.z),0,0,U"Planning a bridge does not create water in a dry riverbed");
		context.expect(world.waterPlanningHeight(midpoint.x,midpoint.z)>world.waterBedHeight(midpoint.x,midpoint.z),U"Infrastructure reserves clearance for the future source-fed river");
		const auto firstFlow=first.end.xz()-first.start.xz();const auto crossing=Vec2{firstFlow.y,-firstFlow.x}.normalized()*30;
		context.expect(!LeveeRoad::dryConnection(world,midpoint-Vec3{crossing.x,0,crossing.y},midpoint+Vec3{crossing.x,0,crossing.y}),U"A levee access road cannot cross a currently dry future riverbed");
		world.advanceSourceWater(0);context.expectNear(water->elapsed(),0,0,U"Pause does not advance water");
		world.advanceSourceWater(0.1);context.expectNear(water->elapsed(),6,0,U"Water follows game time in physical seconds");
		context.expectNear(water->injected(),static_cast<double>(water->sourceCount())*GenerationSettings::get().rivers_sourceRate*6,1e-7,U"Every generated inlet injects its configured volume");
		world.advanceSourceWater(10);context.expectNear(water->elapsed(),12,0,U"A slow frame performs at most one bounded water step");
		context.expectNear(water->inflow(),0,0,U"The real game ocean is a drain only");
		JSON report;report[U"sources"]=water->sourceCount();report[U"oceanCells"]=water->oceanCells();
		report[U"gridBytes"]=water->grid().allocatedBytes();report[U"balanceError"]=water->balanceError();
		report[U"spacing"]=water->grid().cellSize;report[U"reaches"]=water->channels().size();
		double rate=0;int outlets=0,closed=0;for (const auto& c:water->channels()) { rate+=c.inlet;outlets+=c.downstream==-2;closed+=c.downstream==-1; }
		context.expectNear(rate,water->sourceCount()*GenerationSettings::get().rivers_sourceRate,1e-8,U"Every point source is mapped onto a real river reach");
		int banks=0;for (const auto& reach:world.rivers().reaches)
		{
			const Vec2 flow{reach.end.x-reach.start.x,reach.end.z-reach.start.z};
			if (Min(reach.halfWidth,reach.endHalfWidth)<42 || Max(reach.start.y,reach.end.y)>=180 || Abs(reach.end.y-reach.start.y)/flow.length()>=.025) { continue; }
			const auto middle=reach.start.lerp(reach.end,.5);const auto normal=Vec2{flow.y,-flow.x}.normalized();
			const auto p=middle.xz()+normal*((reach.halfWidth+reach.endHalfWidth)*.5+30);
			const auto nearest=world.rivers().nearest(p);
			if (nearest.distance-nearest.halfWidth<27 || nearest.distance-nearest.halfWidth>33) { continue; }
			context.expect(world.computeHeight(static_cast<float>(p.x),static_cast<float>(p.y))>=nearest.surface+1.6,U"Levee crests are solid walkable terrain above the river");
			++banks;if (banks>=20) { break; }
		}
		context.expect(banks>0,U"The real map contains lowland levees");report[U"verifiedLevees"]=banks;
		context.expect(closed==0,U"Every game river follows a spillway to the sea instead of stopping at a land boundary");
		report[U"mappedSupply"]=rate;report[U"seaOutlets"]=outlets;report[U"closedEnds"]=closed;report.save(U"TestResults/source_water_game.json");
	});
	runner.add(U"SourceWater.DryGameEventuallyReachesSea",[](TestContext& context)
	{
		World world;world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);world.generateSourceWater();
		const auto* water=world.sourceWater();const auto& channels=water->channels();
		context.expectNear(water->ownedVolume(),0,0,U"No river or lake water exists before its source supplies it");
		double supply=0;for (const auto& c:channels) { supply+=c.inlet; }
		const Stopwatch timer{StartImmediately::Yes};JSON report;report[U"initialStored"]=water->ownedVolume();
		report[U"samples"]=Array<JSON>{};
		for (int step=1;step<=14400;++step)
		{
			world.advanceSourceWater(.1);
			if (step==1 || step==100 || step==600 || step==3600 || step==14400)
			{
				JSON sample;int wet=0;double seaFlow=0;
				for (std::size_t i=0;i<channels.size();++i) { wet+=water->channelDepth(i)>.003;if (channels[i].downstream==-2) { seaFlow+=water->channelFlow(i); } }
				sample[U"seconds"]=water->elapsed();sample[U"wetReaches"]=wet;sample[U"stored"]=water->ownedVolume();
				sample[U"injected"]=water->injected();sample[U"seaOut"]=water->outflow();sample[U"seaFlow"]=seaFlow;
				report[U"samples"].push_back(sample);
			}
		}
		context.expect(water->outflow()>0,U"Continued source supply actually reaches and drains into the fixed sea");
		context.expectNear(water->ownedVolume()+water->outflow(),water->injected(),.01,U"Every inland litre came from a source");
		context.expectNear(water->inflow(),0,0,U"The sea never helps fill the dry river");
		double seaFlow=0;for (std::size_t i=0;i<channels.size();++i) { if (channels[i].downstream==-2) { seaFlow+=water->channelFlow(i); } }
		context.expect(seaFlow>supply*.8,U"Most source supply reaches the sea within one simulated day");
		report[U"totalSupply"]=supply;report[U"meanStepMs"]=timer.msF()/14400;report[U"balanceError"]=water->balanceError();
		report.save(U"TestResults/source_river_progression.json");
	});
	runner.add(U"SourceWater.NewGameStartsWithEstablishedFlow",[](TestContext& context)
	{
		World world;world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);world.generateSourceWater();
		const auto* water=world.sourceWater();
		context.expectNear(water->ownedVolume(),0,0,U"The generator remains source-only and initially dry");
		world.prepareSourceWaterForNewGame();
		double supply=0;size_t wet=0;
		for (std::size_t i=0;i<water->channels().size();++i) { supply+=water->channels()[i].inlet;wet+=water->channelDepth(i)>.003; }
		context.expect(wet==water->channels().size(),U"Every real river reach already carries source water before new-game play begins");
		context.expectNear(water->injected(),supply*water->elapsed(),1e-5,U"Loading preparation contains only integrated point-source supply");
		context.expectNear(water->ownedVolume()+water->outflow(),water->injected(),.01,U"Established initial flow still conserves source water");
		context.expectNear(water->inflow(),0,0,U"Startup flow never draws water from the sea");
		context.expectNear(world.sourceWaterBacklog(),0,0,U"Loading preparation does not create a calendar catch-up backlog");
		const double seconds=water->elapsed(),stored=water->ownedVolume();
		world.prepareSourceWaterForNewGame();world.advanceSourceWater(0);
		context.expectNear(water->elapsed(),seconds,0,U"Preparation cannot rerun on a live or restored river, and pause still stops time");
		context.expectNear(water->ownedVolume(),stored,0,U"Repeated preparation cannot add hidden water");
	});
	runner.add(U"SourceWater.MapShowsElevatedWater",[](TestContext& context)
	{
		MapTerrainLayer layer;const Size size{64,64};
		layer.update({1000,1000,100,100},size,[](Vec2) { return 120.0f; },[](Vec2 p,float) { return p.x<1050 ? 2.0 : -.05; });
		const RenderTexture target{size};
		{ const ScopedRenderTarget2D scope{target.clear(Palette::Black)};layer.texture.draw(); }
		Graphics2D::Flush();Image pixels;target.readAsImage(pixels);
		const auto wet=pixels[32][8],dry=pixels[32][56];
		context.expect(wet.b>wet.r && wet.b>wet.g,U"Water above sea level is blue on the map");
		context.expect(dry.g>dry.b,U"Dry banks retain their terrain colour");
		pixels.save(U"Screenshot/source_water_map.png");
	});
	runner.add(U"SourceWater.CachedLatticeMatchesDirect",[](TestContext& context)
	{
		World world;world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);world.generateSourceWater();
		context.expect(!world.rivers().waterSources.isEmpty(),U"Real game sources are available");
		if (world.rivers().waterSources.isEmpty()) { return; }
		const auto source=world.rivers().waterSources.front().position;
		const double ox=Floor((source.x-512)/16.0)*16.0, oz=Floor((source.y-512)/16.0)*16.0;
		const auto same=[](const MeshData& a,const MeshData& b)
		{
			return a.vertices.size()==b.vertices.size() && a.indices.size()==b.indices.size()
				&& std::memcmp(a.vertices.data(),b.vertices.data(),a.vertices.size_bytes())==0
				&& std::memcmp(a.indices.data(),b.indices.data(),a.indices.size_bytes())==0;
		};
		SourceWaterGeometry::LatticeCache cache;
		for(int step=0;step<300;++step) { world.advanceSourceWater(0.1); }
		const auto first=SourceWaterGeometry::build(world,ox,oz,64,16,2);
		context.expect(!first.indices.isEmpty(),U"The sampled chunk holds river water");
		context.expect(same(first,SourceWaterGeometry::build(world,ox,oz,64,16,2,&cache)),U"A cold lattice cache reproduces the direct mesh bit for bit");
		for(int step=0;step<300;++step) { world.advanceSourceWater(0.1); }
		context.expect(same(SourceWaterGeometry::build(world,ox,oz,64,16,2),SourceWaterGeometry::build(world,ox,oz,64,16,2,&cache)),
			U"A warm cache reused after the water level changes still matches the direct mesh");
	});
	runner.add(U"SourceWater.NativeGameSurface",[](TestContext& context)
	{
		World world;world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);world.generateSourceWater();
		context.expect(!world.rivers().waterSources.isEmpty(),U"Real game sources are available for native rendering");
		if (world.rivers().waterSources.isEmpty()) { return; }
		const auto source=world.rivers().waterSources.front().position;
		const double x=source.x, z=source.y;
		const double ox=Floor((x-512)/16.0)*16.0, oz=Floor((z-512)/16.0)*16.0;
		MeshData terrain;
		for(int row=0;row<=64;++row)
		{
			for(int col=0;col<=64;++col)
			{
				const double wx=ox+col*16,wz=oz+row*16;
				Vertex3D vertex;vertex.pos={static_cast<float>(wx),world.computeHeight(static_cast<float>(wx),static_cast<float>(wz)),static_cast<float>(wz)};
				vertex.normal={0,1,0};vertex.tex={0,0};terrain.vertices << vertex;
			}
		}
		for(uint32 row=0;row<64;++row) { for(uint32 col=0;col<64;++col) { const uint32 a=row*65+col;terrain.indices << TriangleIndex32{a,a+65,a+1} << TriangleIndex32{a+1,a+65,a+66}; } }
		const Mesh ground{terrain};
		#if SIV3D_PLATFORM(WINDOWS)
		const auto terrainShader=PixelShader::HLSL(U"../../Test/shaders/source_water_terrain.hlsl",U"TerrainPS");
		const auto bankShader=PixelShader::HLSL(U"../../App/shaders/hlsl/city_forward.hlsl",U"RiverBank_PS");
		context.expect(!!bankShader,U"The production terrain-aligned gravel-bed shader compiles in the native GPU process");
#else
		const PixelShader terrainShader=GLSL{U"../../Test/shaders/source_water_terrain.frag", {}};
#endif
		const auto waterShader=ShaderAsset::pixel(U"../../App/shaders/hlsl/source_water.hlsl",U"WaterPS");
		context.expect(!!terrainShader && !!waterShader,U"The production HLSL water shader compiles in the native Test process");
		if (!terrainShader || !waterShader) { return; }
		const Stopwatch simulation{StartImmediately::Yes};
		for(int step=0;step<600;++step) { world.advanceSourceWater(0.1); }
		const double simulationMs=simulation.msF();
		const Stopwatch geometry{StartImmediately::Yes};
		const double storedBeforeMesh=world.sourceWater()->ownedVolume();
		const auto data=SourceWaterGeometry::build(world,ox,oz,64,16,2);
		context.expect(data.vertices.size()>65*65 && data.vertices.size()<65*65*2,U"Only shoreline cells receive half-grid samples, within a bounded vertex budget");
		context.expect(data.indices.size()<64*64*2,U"Fully dry cells are omitted instead of refining the whole map");
		context.expectNear(world.sourceWater()->ownedVolume(),storedBeforeMesh,0,U"Smoothing the visible bank does not alter simulated water");
		const double meshMs=geometry.msF();
		context.expect(!data.indices.isEmpty(),U"The production mesh contains owned land triangles");
		const Mesh water{data};
		const Size size{800,600};Window::Resize(size);Scene::Resize(size);
		const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm,HasDepth::Yes};
		const double bed=world.waterBedHeight(x,z);
		const BasicCamera3D camera{size,40_deg,{x+480,bed+460,z-480},{x,bed,z}};
		struct Visual { Float4 parameters; };ConstantBuffer<Visual> visual;visual->parameters={10,0,0,0};
		Image bare,wet,moving;
		for(int frame=0;frame<5 && System::Update();++frame)
		{
			{
				const ScopedRenderTarget3D renderTarget{target.clear(ColorF{0.60,0.72,0.78})};
				const ScopedRenderStates3D state{BlendState::Opaque,RasterizerState::SolidCullNone,DepthStencilState::DepthTestWrite};
				Graphics3D::SetCameraTransform(camera);Graphics3D::SetGlobalAmbientColor(ColorF{0.45});
				Graphics3D::SetSunColor(ColorF{0.85});Graphics3D::SetSunDirection(Vec3{-.4,.82,-.35}.normalized());
				{ const ScopedCustomShader3D shader{terrainShader};ground.draw(); }
				if (frame>0)
				{
					if (frame==4) { visual->parameters.x=10.02f; }
					Graphics3D::SetPSConstantBuffer(5,visual);
					const ScopedRenderStates3D stateWater{BlendState::NonPremultiplied,RasterizerState::SolidCullNone,DepthStencilState::DepthTest};
					const ScopedCustomShader3D shader{waterShader};water.draw();
				}
			}
			Graphics3D::Flush();target.draw();
			if(frame==0) { target.readAsImage(bare); }
			if(frame==3) { target.readAsImage(wet);wet.save(U"Screenshot/source_water_native.png"); }
			if(frame==4) { target.readAsImage(moving); }
		}
		size_t changed=0;
		if (bare.size()==wet.size() && !wet.isEmpty())
		{
			for(int row=0;row<wet.height();++row) { for(int col=0;col<wet.width();++col) { const auto a=bare[row][col],b=wet[row][col];changed+=Abs(static_cast<int>(a.r)-b.r)+Abs(static_cast<int>(a.g)-b.g)+Abs(static_cast<int>(a.b)-b.b)>8 ? 1 : 0; } }
		}
		context.expect(changed>50,U"GPU readback confirms visible source water inside the carved game riverbed");
		size_t motion=0;
		if (wet.size()==moving.size() && !moving.isEmpty())
		{
			for(int row=0;row<wet.height();++row) { for(int col=0;col<wet.width();++col)
			{
				const auto a=wet[row][col],b=moving[row][col];
				motion+=Abs(static_cast<int>(a.r)-b.r)+Abs(static_cast<int>(a.g)-b.g)+Abs(static_cast<int>(a.b)-b.b)>2 ? 1 : 0;
			} }
		}
		context.expect(motion>20,U"The supplied downstream velocity animates the actual river shader without changing water storage");
		context.expectNear(world.sourceWater()->ownedVolume(),storedBeforeMesh,0,U"Visual current animation remains independent of the volume ledger");
		const auto sea=SourceWaterGeometry::build(world,31232,34304,16,128,1);
		for(const auto& vertex:sea.vertices) { context.expectNear(vertex.pos.y,0.01,1e-8,U"Every sea render vertex shares the same fixed level plus only the depth bias"); }
		JSON report;report[U"sourceX"]=x;report[U"sourceZ"]=z;report[U"changedPixels"]=changed;
		report[U"meanStepMs"]=simulationMs/600;report[U"meshMs"]=meshMs;report[U"seaTriangles"]=sea.indices.size();
		report[U"vertices"]=data.vertices.size();report[U"triangles"]=data.indices.size();report[U"motionPixels"]=motion;
		report[U"balanceError"]=world.sourceWater()->balanceError();report[U"seaIn"]=world.sourceWater()->inflow();
		report.save(U"TestResults/source_water_native.json");
	});

}
