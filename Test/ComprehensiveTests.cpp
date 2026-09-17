#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/road/RoadPlanConstruction.hpp"
#include "src/gen/AgriculturalLayout.hpp"
#include "src/ui/Camera.hpp"
#include "src/gen/RoadTerrainFit.hpp"
#include "src/gen/RoadDesignLimits.hpp"
#include "src/gen/RoadConstructionCost.hpp"
#include "src/road/RoadConstructionStart.hpp"
#include "src/ui/WorldMapView.hpp"
#include "src/ui/MapTerrainLayer.hpp"
#include "src/ui/TownBillboards.hpp"
#include "src/ui/CommandPalette.hpp"
#include "src/ui/LocationTooltip.hpp"
#include "src/debug/CommandExecution.hpp"
#include "src/gen/SettlementNames.hpp"
#include "src/gen/SettlementPlacement.hpp"
#include "src/ui/LocalMapView.hpp"
#include "src/render/RoadRenderer.hpp"
#include "src/render/VegetationProfile.hpp"
#include "src/render/CityLighting.hpp"
#include "src/render/RegionalTerrain.hpp"
#include "src/render/WorldRenderer.hpp"
#include "src/render/MinimapRenderer.hpp"
#include "src/asset/AssetRegistrar.hpp"

namespace
{
	void flatWorld(World& world,float height=30)
	{
		world.reserveChunks();world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		for (int z=31;z<=33;++z) { for (int x=31;x<=33;++x) { world.installChunkDirect({x,z},HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,height),height,height});world.getChunk({x,z})->zoneMap.fill(ZoneType::Agriculture); } }
	}
}

void registerComprehensiveTests(TestRunner& runner)
{
	runner.add(U"Comprehensive.RepeatedRoadGeneration",[](TestContext& context)
	{
		World world;flatWorld(world);RoadNetwork network;RoadPlanDraft draft;const auto road=RoadPlanDraft::makeRoadTemplate(0);int attempts=0,failed=0;JSON report;report[U"failures"]=Array<JSON>{};
		for (int round=0;round<3;++round) { for (const double length:{12.0,45.0,85.0,180.0,350.0}) { for (const double angle:{0.0,.43,.785,1.22})
		{
			draft.clear();const Vec3 start{33000,30,33000},end=start+Vec3{Cos(angle)*length,0,Sin(angle)*length};draft.place(start);draft.place(end);++attempts;
			if (!draft.generate(world,road)) { ++failed;JSON row;row[U"round"]=round;row[U"length"]=length;row[U"angle"]=angle;report[U"failures"].push_back(row); }
			if (round==0 && angle==0 && length==350 && draft.valid()) { const auto result=RoadPlanConstruction::commit(network,world,draft,road,{},100,0);context.expect(std::holds_alternative<RoadPlanConstruction::Receipt>(result),U"First road can be committed before generating further candidates"); }
		} } }
		report[U"attempts"]=attempts;report[U"failed"]=failed;report.save(U"TestResults/repeated_road_generation.json");context.expectEqual(failed,0,U"Every repeated request across headings and lengths finds the obvious flat route");
	});
	runner.add(U"Comprehensive.GeneratedFarmRoadsOpen",[](TestContext& context)
	{
		World world;flatWorld(world);RoadNetwork roads;const Vec3 a{31900,30,33000},b{34200,30,33000};const int id=*roads.addEdge(roads.addNode(a),roads.addNode(b),a.lerp(b,1.0/3),a.lerp(b,2.0/3));roads.getEdge(id)->edgeState=EdgeState::Existing;
		const auto stats=AgriculturalLayout::prepare(world,roads,42);int planned=0,closed=0;
		for (const auto& edge:roads.edges()) { if (edge.id>=0 && edge.farmAccess) { planned+=edge.edgeState==EdgeState::Planned;closed+=!edge.isRoadbedBuilt() || (edge.edgeState!=EdgeState::Existing && edge.edgeState!=EdgeState::Open); } }
		JSON report;report[U"roads"]=stats.tracks;report[U"planned"]=planned;report[U"closed"]=closed;report.save(U"TestResults/farm_initial_status.json");context.expect(stats.tracks>10 && planned==0 && closed==0,U"Every initially generated farm road is built and open");
	});
	runner.add(U"Comprehensive.OverviewTerrainHeight",[](TestContext& context)
	{
		Array<double> clearance;
		for (const float height:{30.0f,1400.0f}) { World world;flatWorld(world,height);GameCamera camera;camera.setState({33000,0,33000},600,0,.7f);camera.setBlockInput(true);camera.setKeyboardBlocked(true);camera.update(0,world);clearance << camera.eyePosition().y-height; }
		JSON report;report[U"clearance"]=clearance;report.save(U"TestResults/overview_height.json");context.expectNear(clearance[0],clearance[1],.01,U"The overview preserves its ground clearance on high terrain");context.expect(clearance[1]>300,U"High mountains do not push the overview down to a 3 m ground clamp");
	});
	runner.add(U"Comprehensive.AfterConstructionTerrain",[](TestContext& context)
	{
		World world;flatWorld(world);RoadNetwork roads;const auto road=RoadPlanDraft::makeRoadTemplate(0);
		for (int z=31;z<=33;++z) { for (int x=31;x<=33;++x)
		{
			auto* chunk=world.getChunk({x,z});
			for (int row=0;row<=HEIGHT_CELLS;++row) { for (int col=0;col<=HEIGHT_CELLS;++col)
			{
				const double px=x*CHUNK_SIZE+col*(static_cast<double>(CHUNK_SIZE)/HEIGHT_CELLS),pz=z*CHUNK_SIZE+row*(static_cast<double>(CHUNK_SIZE)/HEIGHT_CELLS);
				chunk->heightMap[row][col]=static_cast<float>(30+(px-33000)*.02+3*Sin(pz/220));
			} }
		}
		}
		int attempts=0,failed=0;JSON report;report[U"failures"]=Array<JSON>{};
		for (int round=0;round<3;++round)
		{
			for (const double length:{45.0,130.0,350.0,700.0}) { for (const double angle:{0.0,.43,.785,1.22,1.8,2.5})
			{
				RoadPlanDraft draft;Vec3 a{33000,0,33000},b=a+Vec3{Cos(angle)*length,0,Sin(angle)*length};
				a.y=world.sampleHeight(static_cast<float>(a.x),static_cast<float>(a.z));b.y=world.sampleHeight(static_cast<float>(b.x),static_cast<float>(b.z));
				draft.place(a);draft.place(b);++attempts;
				if (!draft.generate(world,road)) { ++failed;JSON row;row[U"round"]=round;row[U"length"]=length;row[U"angle"]=angle;report[U"failures"].push_back(row); }
				if (round==0 && length==700 && angle==0 && draft.valid())
				{
					const auto result=RoadPlanConstruction::commit(roads,world,draft,road,{},100,0);
					if (const auto* receipt=std::get_if<RoadPlanConstruction::Receipt>(&result)) { RoadTerrainFit::apply(roads,world,HashSet<int>{receipt->edgeIds.begin(),receipt->edgeIds.end()}); }
					else { context.expect(false,U"The first construction must complete before further route generation"); }
				}
			} }
		}
		report[U"attempts"]=attempts;report[U"failed"]=failed;report.save(U"TestResults/after_construction_routes.json");
		context.expectEqual(failed,0,U"Obvious gentle hillside routes remain available after road earthworks");
	});
	runner.add(U"Comprehensive.OverviewZoom",[](TestContext& context)
	{
		GameCamera batched,individual;batched.zoom(5);for(int i=0;i<5;++i) { individual.zoom(1); }
		context.expectNear(batched.distance(),individual.distance(),.001,U"Wheel event batching does not change zoom distance");
		batched.zoom(-5);context.expectNear(batched.distance(),600,.001,U"Opposite notch counts exactly undo zoom");
		batched.zoom(200);context.expectNear(batched.distance(),60000,.01,U"Overview zooms out to 60 km");
	});
	runner.add(U"Comprehensive.SandboxConstruction",[](TestContext& context)
	{
		World world;flatWorld(world);RoadNetwork roads;RoadPlanDraft draft;const auto road=RoadPlanDraft::makeRoadTemplate(0);draft.place({33000,30,33000});draft.place({33300,30,33000});context.expect(draft.generate(world,road),U"Construction fixture generated");
		const auto normal=RoadPlanConstruction::commit(roads,world,draft,road,{},0,0);context.expect(std::holds_alternative<RoadPlanConstruction::Error>(normal),U"Economy mode rejects an unaffordable new road");
		RoadPlanConstruction::Request request;request.sandbox=true;
		const auto sandbox=RoadPlanConstruction::commit(roads,world,draft,road,request,0,0);context.expect(std::holds_alternative<RoadPlanConstruction::Receipt>(sandbox),U"Sandbox builds a new road with zero funds");
		RoadNetwork planned;const int a=planned.addNode({33000,30,33000}),b=planned.addNode({33300,30,33000});const int id=*planned.addEdge(a,b,{33100,30,33000},{33200,30,33000});
		context.expect(!RoadConstructionStart::start(planned,{id},0,0),U"Economy mode checks existing planned road funds");
		context.expect(RoadConstructionStart::start(planned,{id},0,0,true).has_value(),U"Sandbox starts existing planned roads with zero funds");
	});
	runner.add(U"Comprehensive.ShortViaductPiers",[](TestContext& context)
	{
		World world;flatWorld(world);RoadNetwork roads;
		for (const double length:{12.0,24.0,45.0,60.0,120.0})
		{
			const Vec3 a{33000,50,33000},b=a+Vec3{length,0,0};const int id=*roads.addEdge(roads.addNode(a),roads.addNode(b),a.lerp(b,1.0/3),a.lerp(b,2.0/3));roads.getEdge(id)->useElevation=true;
			roads.generatePiersForEdge(id,world);int piers=0;for(const auto& object:roads.objects()) { piers+=object.id>=0 && object.parentEdgeId==id && object.type==RoadObjectType::Pier; }
			context.expect(piers>0,U"Even a {} m viaduct segment has a support"_fmt(length));
		}
	});
	runner.add(U"Comprehensive.ExpandedMapLocations",[](TestContext& context)
	{
		const Size size{800,600};const Font font{14};MapTerrainLayer terrain;WorldMapView map;const RenderTexture target{size};int failed=0,frames=0;JSON observations;observations[U"frames"]=Array<JSON>{};
		for (const double zoom:{1.0,4.0,32.0,128.0,512.0}) { for (const Vec2 center:{Vec2{1024,1024},Vec2{31000.125,27000.375},Vec2{32768,32768},Vec2{64000,64000}})
		{
			map.center=center;map.zoom=zoom;
			const Vec2 a=map.toWorld(map.body(size).tl(),size),b=map.toWorld(map.body(size).br(),size);
			const RectF visible{Vec2{Min(a.x,b.x),Min(a.y,b.y)},Vec2{Abs(a.x-b.x),Abs(a.y-b.y)}};
			if (terrain.update(visible,{512,384},[](Vec2){return 100.0f;})) { map.invalidateCartography(); }
			{ const ScopedRenderTarget2D scope{target.clear(Palette::Black)};map.draw(size,terrain.texture,font,{-100000,-100000},terrain.bounds); }
			Graphics2D::Flush();Image pixels;target.readAsImage(pixels);int wrong=0;
			for (int y=90;y<500;++y) { for (int x=40;x<680;++x)
			{
				const Vec2 world=map.toWorld({x+.5,y+.5},size);if (world.x<32 || world.y<32 || world.x>65504 || world.y>65504) { continue; }
				const auto color=pixels[y][x];wrong+=color.g<90 || color.g>210 || color.a<250;
			} }
			failed+=wrong>0;++frames;JSON observation;observation[U"zoom"]=zoom;observation[U"center"]=Array<double>{center.x,center.y};observation[U"wrong"]=wrong;observations[U"frames"].push_back(observation);
		}
		}
		observations[U"count"]=frames;observations[U"failed"]=failed;observations.save(U"TestResults/expanded_map_locations.json");context.expectEqual(failed,0,U"Every magnification/location keeps terrain opaque and correctly colored");
	});

	runner.add(U"Comprehensive.MapTunnelAndReading",[](TestContext& context)
	{
		const Font font{14};const Size size{800,600};WorldMapView map;map.center={32768,32768};map.zoom=128;
		map.labels << WorldMapView::Label{map.center,U"山里村",false,1,U"Yamazato-mura"};
		const Texture terrain{Image{32,32,Color{100,140,100}}};const RenderTexture target{size};Array<int> white;
		for (bool tunnel:{false,true})
		{
			map.streets={WorldMapView::Stroke{{{30000,32868},{35000,32868}},{30000,32860,5000,16},6,0,tunnel}};map.invalidateCartography();
			{const ScopedRenderTarget2D scope{target.clear(Palette::Black)};map.draw(size,terrain,font,{-100000,-100000});}Graphics2D::Flush();Image pixels;target.readAsImage(pixels);
			const int row=static_cast<int>(map.toScreen({32768,32868},size).y);int count=0;
			for(int x=50;x<680;++x) { count+=pixels[row][x].r>230 && pixels[row][x].g>230; }white << count;pixels.save(U"Screenshot/map_tunnel_{}.png"_fmt(tunnel));
		}
		JSON strokeReport;strokeReport[U"whitePixels"]=white;strokeReport.save(U"TestResults/tunnel_strokes.json");
		context.expect(white[0]>600 && white[1]>200 && white[1]<white[0]*.8,U"Tunnel routes have visible repeated gaps across the map");
		GameCamera camera;camera.setState({32768,30,32768},1000,0,.8f);
		const auto labels=TownBillboards::layout({{{32768,50,32768},U"山里村",1,U"Yamazato-mura"}},camera,size,font);
		context.expect(labels.size()==1 && labels.front().bounds.h>=45,U"Overview labels reserve a separate romanized line");
		{const ScopedRenderTarget2D scope{target.clear(ColorF{.1})};TownBillboards::draw(labels,font);}Graphics2D::Flush();Image pixels;target.readAsImage(pixels);int reading=0;
		if (!labels.isEmpty()) { const auto bounds=labels.front().bounds;for(int y=static_cast<int>(bounds.y+27);y<bounds.bottomY();++y) { for(int x=static_cast<int>(bounds.x);x<bounds.rightX();++x) { reading+=pixels[y][x].r>120; } } }
		context.expect(reading>30,U"Romanized place names appear in the lower line on the GPU");
	});
	runner.add(U"Comprehensive.BlueGuideApproach",[](TestContext& context)
	{
		const FilePath directory=FileSystem::CurrentDirectory();struct Restore { FilePath value;~Restore(){FileSystem::ChangeCurrentDirectory(value);} } restore{directory};FileSystem::ChangeCurrentDirectory(directory+U"../../App/");RegisterAssets();
		World world;flatWorld(world);RoadNetwork roads;const Vec3 a{33000,30,33000},b{33000,30,33400};const int id=*roads.addEdge(roads.addNode(a),roads.addNode(b),a.lerp(b,1.0/3),a.lerp(b,2.0/3));auto* edge=roads.getEdge(id);edge->edgeState=EdgeState::Existing;for(auto& part:edge->parts) { part.build=BuildState::Built; }
		GuideSignPlacement sign;sign.parentEdgeId=id;sign.nodeEndId=edge->nodeA;sign.arcOffset=200;sign.lateralOffset=-5;sign.widthOverride=4;sign.heightOverride=2;SignElement text;text.kind=SignElementKind::Text;text.text=U"山里";text.posX=.5f;text.posY=.5f;sign.elements << text;roads.addGuideSign(sign);
		RoadRenderer renderer;renderer.loadAssets();renderer.setCacheBuildBudget(1000);renderer.prepareGuideSignTextures(roads);Graphics2D::Flush();const Size size{400,300};const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};Array<int> visible;
		for(int side:{-1,1})
		{
			const Vec3 focus{33000,33.3,33200},eye{33000,33.3,33200+side*15.0};const BasicCamera3D camera{size,60_deg,eye,focus};
			{const ScopedRenderTarget3D scope{target.clear(ColorF{.1})};const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite,RasterizerState::SolidCullBack};Graphics3D::SetCameraTransform(camera);Graphics3D::SetGlobalAmbientColor(ColorF{1});Graphics3D::SetSunColor(ColorF{0});renderer.render(roads,world,ViewFrustum{camera,1000},eye);}Graphics3D::Flush();Image pixels;target.readAsImage(pixels);int blue=0;for(const auto pixel:pixels) { blue+=pixel.b>pixel.r+40 && pixel.b>pixel.g+20; }visible << blue;
		}
		Graphics3D::SetSunColor(ColorF{1});Graphics3D::SetGlobalAmbientColor(ColorF{.5});JSON report;report[U"bluePixels"]=visible;report.save(directory+U"TestResults/guide_approach.json");context.expect(visible[0]>100 && visible[1]==0,U"Blue guide boards face vehicles departing from their reference end and show metal backs to opposite traffic");
	});

	runner.add(U"Comprehensive.CommandParserAndPalette",[](TestContext& context)
	{
		for (const String line:{U"/time set 13 46",U"/day set 1 4 2",U"/road status set 1446 planned",U"/camera goto 33000 32000",U"/camera zoom 60000",U"/speed set 4",U"/fps on"})
		{ context.expect(GameCommands::parse(line).command.has_value(),U"Valid command parses: "+line); }
		for (const String line:{U"/time set 24 00",U"/day set 1 2 31",U"/road status set -1 open",U"/money set nan",U"/speed set 3",U"/camera goto inf 0",U"/time set 1 2 extra"})
		{ context.expect(!GameCommands::parse(line).command,U"Invalid input is rejected without effects: "+line); }
		const auto suggestions=GameCommands::suggest(U"/road status set 1446 p");
		context.expect(suggestions.size()==1 && suggestions.front().completion==U"/road status set 1446 planned",U"Completion preserves the typed road ID");
		CommandPalette palette;palette.open();context.expect(GameInput::keyboardBlocked(),U"Opening the palette owns keyboard input immediately");palette.input=U"/road status set 1446 p";palette.report(U"道路の状態を変更しました",false);
		const Size size{800,600};const Font font{16};const RenderTexture target{size};
		{const ScopedRenderTarget2D scope{target.clear(ColorF{.3})};palette.draw(size,font);}Graphics2D::Flush();Image pixels;target.readAsImage(pixels);pixels.save(U"Screenshot/command_palette.png");int glyphs=0,stray=0;
		for(int y=0;y<size.y;++y) { for(int x=0;x<size.x;++x) { if(pixels[y][x].r>160) { if(palette.bounds(size).contains(Vec2{x,y})) { ++glyphs; } else { ++stray; } } } }
		context.expect(glyphs>150 && stray==0,U"Command text, suggestions and result stay inside the bottom-left palette");palette.close();GameInput::textOwnedFrame=false;
	});

	runner.add(U"Comprehensive.CommandExecution",[](TestContext& context)
	{
		World world;flatWorld(world);RoadNetwork roads;GameClock clock;GameCamera camera;double funds=0;bool fps=false;
		const auto run=[&](StringView line)
		{
			const auto parsed=GameCommands::parse(line);if(!parsed.command) { return CommandExecution::Result{false,parsed.message,{},false}; }
			return CommandExecution::execute(*parsed.command,{clock,roads,world,camera,funds,fps});
		};
		context.expect(run(U"/time set 13 46").success && clock.timeString().ends_with(U"13:46"),U"Time command sets hour and minute");
		context.expect(run(U"/day set 1 4 2").success && clock.day==2 && clock.month==4 && clock.timeString().ends_with(U"13:46"),U"Date command preserves time of day");
		context.expect(run(U"/day set 1 1 1").success && clock.day==1 && clock.month==1,U"Calendar dates before the initial April date work");
		context.expect(run(U"/time set 0 0").success && clock.hour==0,U"Midnight is representable");
		const int a=roads.addNode({33000,30,33000}),b=roads.addNode({33300,30,33000});const int id=*roads.addEdge(a,b,{33100,30,33000},{33200,30,33000});
		for (const String state:{U"open",U"closed",U"planned",U"construction"})
		{
			const auto result=run(U"/road status set {} {}"_fmt(id,state));context.expect(result.success && result.changedEdges==Array<int>{id},U"Road status produces the targeted renderer/network invalidation");
			const auto* edge=roads.getEdge(id);context.expect(edge->isRoadbedBuilt()==(state==U"open" || state==U"closed"),U"Physical road parts match the requested road status");
		}
		context.expect(!run(U"/road status set 999999 open").success,U"Unknown road IDs do not mutate the network");
		context.expect(run(U"/money set 25.5").success && funds==25.5,U"Money command updates funds");context.expect(run(U"/fps on").success && fps,U"FPS command updates the actual flag");
		camera.setDrivingState({33000,30,33000},0,0);run(U"/camera goto 33200 33400");context.expect(camera.mode()==CameraMode::Overview && camera.focusPoint().x==33200,U"Teleport exits the camera's driving state");
	});
	runner.add(U"Comprehensive.SettlementNames",[](TestContext& context)
	{
		MapGenerator::Settlement town;town.name=U"山里";town.reading=U"yamazato";
		town.kind = MapGenerator::SettlementKind::RegionalCity;
		context.expect(SettlementNames::name(town) == U"山里市" && SettlementNames::reading(town) == U"yamazato City",
			U"City labels have city suffixes and romanization");
		town.kind=MapGenerator::SettlementKind::LocalTown;context.expect(SettlementNames::name(town)==U"山里町",U"Market towns use machi");
		town.kind = MapGenerator::SettlementKind::RuralSettlement;
		context.expect(SettlementNames::name(town) == U"山里村", U"Villages use mura");
		town.name = U"松村";
		town.reading = U"matsumura";
		context.expect(SettlementNames::name(town) == U"松村" && SettlementNames::reading(town) == U"matsumura Vill.",
			U"Generated roots already ending in mura do not duplicate the suffix");
	});

	runner.add(U"Comprehensive.VegetationTransitions",[](TestContext& context)
	{
		const auto low=VegetationProfile::at(300),middle=VegetationProfile::at(1300),high=VegetationProfile::at(1800),snow=VegetationProfile::at(2300);
		context.expect(low.trees==1 && middle.trees>0 && middle.trees<1 && high.trees==0,U"Forest becomes gradually sparse toward the tree line");
		context.expect(high.alpine>0 && snow.alpine==0 && snow.snow==1,U"Alpine shrubs yield to full snow cover");
		const auto habitat=[](Vec2 point){return point.x>=0;};
		const VegetationProfile::BoundaryField left{{-1024,0,1024,1024},habitat},right{{0,0,1024,1024},habitat};
		context.expect(right.density({25,300})<.1 && right.density({200,300})>.2 && right.density({200,300})<.7 && right.density({500,300})>.98,U"Forest density fades over approximately 500 metres");
		for(int y=0;y<=1024;y+=64) { context.expectNear(left.density({0,y}),right.density({0,y}),1e-6,U"The boundary field is continuous across terrain chunks"); }
	});
	runner.add(U"Comprehensive.AltitudeTerrainPixels",[](TestContext& context)
	{
		CityLighting lighting;context.expect(lighting.initialize(U"../../App/shaders/hlsl/city_forward.hlsl"),U"Altitude terrain shader compiles");
		const Size size{240,180};const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};const Texture grass{Image{8,8,Color{90,125,70}}};Array<double> brightness;JSON report;
		for(const double height:{300.0,1500.0,1850.0,2300.0})
		{
			const Vec3 center{33000,height,33000};const BasicCamera3D camera{size,60_deg,center+Vec3{0,30,-20},center};
			{const ScopedRenderTarget3D scope{target.clear(Palette::Black)};const ScopedRenderStates3D states{DepthStencilState::DepthTestWrite,RasterizerState::SolidCullNone};const ScopedCustomShader3D shader{lighting.terrainShader()};Graphics3D::SetCameraTransform(camera);Graphics3D::SetGlobalAmbientColor(ColorF{1});Graphics3D::SetSunColor(ColorF{0});lighting.bind();Box{center,120,1,120}.draw(grass);}
			Graphics3D::Flush();Image pixels;target.readAsImage(pixels);double sum=0;for(int y=60;y<120;++y) { for(int x=80;x<160;++x) { const auto c=pixels[y][x];sum+=(c.r+c.g+c.b)/3.0; } }brightness << sum/4800;pixels.save(U"Screenshot/terrain_altitude_{}.png"_fmt(static_cast<int>(height)));
		}
		Graphics3D::SetSunColor(ColorF{1});Graphics3D::SetGlobalAmbientColor(ColorF{.5});report[U"brightness"]=brightness;report.save(U"TestResults/altitude_terrain.json");
		context.expect(brightness.back()>brightness.front()+70 && brightness.back()>200,U"High altitude snow is visibly distinct from lowland ground on the GPU");
	});

	runner.add(U"Comprehensive.OverviewLocationTooltip",[](TestContext& context)
	{
		const Font font{FontMethod::MSDF,32};const Texture shield{U"../../App/assets/signs/guide/national_route.png"};
		const LocationTooltip::Content content{U"山里村",U"Yamazato-mura",U"木曽街道",19};
		for(const Size size:{Size{800,600},Size{1280,800},Size{320,480}})
		{
			const RenderTexture target{size};
			for(const double bottomInset:{12.0,192.0})
			{
				const RectF box=LocationTooltip::bounds(size,content,bottomInset);
				context.expect(box.x==12 && box.h<=48 && box.bottomY()==size.y-bottomInset && box.rightX()<=size.x-12,U"Compact address stays at bottom left and above an open FPS graph");
				{const ScopedRenderTarget2D scope{target.clear(Color{51,51,51})};LocationTooltip::draw(size,content,font,shield,bottomInset);}Graphics2D::Flush();Image pixels;target.readAsImage(pixels);int blue=0,text=0,untouched=0,outside=0;
				for(int y=0;y<size.y;++y) { for(int x=0;x<size.x;++x)
				{
					const auto c=pixels[y][x];const bool changed=c.r!=51 || c.g!=51 || c.b!=51;
					if(!box.stretched(2).contains(Vec2{x,y})) { outside+=changed;continue; }
					blue+=c.b>c.r+50 && c.b>c.g+25;text+=c.r>160;untouched+=!changed;
				} }
				context.expect(blue>40 && text>70 && untouched>box.area()*.55 && outside==0,U"Route shield and text render without a panel or stray pixels");
				pixels.save(U"Screenshot/compact_location_{}_{}.png"_fmt(size.x,static_cast<int>(bottomInset)));
			}
		}
		RoadNetwork roads;const int id=*roads.addEdge(roads.addNode({33000,50,33000}),roads.addNode({33500,50,33000}),{33166,50,33000},{33333,50,33000});RoadPlanSnapIndex index;index.rebuild(roads);
		const auto hover=index.find(roads,{33200,30,33003},12,Math::Inf,false);context.expect(hover.edgeId==id,U"Overview lookup finds the road above the pointed terrain");
		context.expect(!index.find(roads,{33200,30,33003}).connected,U"Construction snapping still rejects a different road height");
	});

	runner.add(U"Comprehensive.NextRoadAllowedGrade",[](TestContext& context)
	{
		World world;flatWorld(world);RoadNetwork roads;const auto road=RoadPlanDraft::makeRoadTemplate(0);RoadPlanDraft first;
		first.place({32900,30,33000});first.place({33000,30,33000});context.expect(first.generate(world,road),U"First road is available");
		const auto built=first.apply(roads,world,road);context.expect(!built.isEmpty(),U"First road has been placed before the next draft");
		RoadPlanDraft next;const double grade=RoadDesignLimits::forType(road.roadType).maximumGrade*.999;
		next.place({33000,30,33000});next.place({33100,30+100*grade,33000});
		context.expect(next.generate(world,road),U"A second road with an allowed grade must survive preview generation");
		if(next.valid()) { context.expect(next.propose(roads,world,road).has_value(),U"Confirmed geometry matches the generated preview"); }
	});

	runner.add(U"Comprehensive.FarmRoadConnections",[](TestContext& context)
	{
		World world;flatWorld(world);RoadNetwork roads;
		for(double z:{33000.0,33160.0})
		{
			const Vec3 a{31900,30,z},b{34200,30,z};const int id=*roads.addEdge(roads.addNode(a),roads.addNode(b),a.lerp(b,1.0/3),a.lerp(b,2.0/3));auto* edge=roads.getEdge(id);edge->edgeState=EdgeState::Existing;for(auto& part:edge->parts) { part.build=BuildState::Built; }
		}
		const auto stats=AgriculturalLayout::prepare(world,roads,42);context.expect(stats.connections>0,U"Farm tracks join a nearby parallel public road instead of stopping short");
		JSON report;report[U"tracks"]=stats.tracks;report[U"connections"]=stats.connections;report.save(U"TestResults/farm_connections.json");
		for(const auto& node:roads.nodes()) { if(node.id<0 || node.attachments.size()<3) { continue; }bool farm=false;for(int id:node.edgeIds()) { farm|=roads.getEdge(id)->farmAccess; }if(farm) { context.expect(!node.laneConnections.isEmpty(),U"Joined farm junctions include vehicle connections"); } }
	});

	runner.add(U"Comprehensive.PlainMapLabels",[](TestContext& context)
	{
		const Font font{FontMethod::MSDF,16};const Size size{800,600};const Texture terrain{Image{16,16,Color{85,120,95}}};const RenderTexture target{size};
		for(bool local:{false,true})
		{
			WorldMapView map;map.visible=true;map.center={32768,32768};map.zoom=16;LocalMapView minimap;minimap.center=map.center;
			const RectF rect{0,0,800,600};const Vec2 place{32768,32600};Array<Image> frames;
			for(bool labels:{false,true})
			{
				if(labels) { map.labels << WorldMapView::Label{place,U"山里村",false,1,U"Yamazato-mura"}; }
				{const ScopedRenderTarget2D scope{target.clear(Palette::Black)};if(local) { minimap.draw(rect,terrain,font,map); } else { map.draw(size,terrain,font,map.center); }}
				Graphics2D::Flush();Image image;target.readAsImage(image);frames << image;
			}
			const Vec2 anchor=local ? minimap.toScreen(place,rect) : map.toScreen(place,size);
			const RectF box{anchor+Vec2{6,local ? -8 : -10},Max(font(U"山里村").region().w,font(U"Yamazato-mura").region(local ? 10 : 11).w)+(local ? 6 : 12),local ? 35 : 40};
			int changed=0,emptyCorners=0,reading=0;
			for(int y=static_cast<int>(box.y);y<box.bottomY();++y) { for(int x=static_cast<int>(box.x);x<box.rightX();++x)
			{
				const bool different=frames[0][y][x]!=frames[1][y][x];changed+=different;
				if(x<box.x+2 || x>box.rightX()-2) { emptyCorners+=different; }
				if(y>box.y+24) { reading+=different; }
			} }
			context.expect(changed>70 && changed<box.area()*.55 && emptyCorners==0 && reading>20,U"Map labels retain Japanese and romanized text with transparent surroundings");
			frames[1].save(U"Screenshot/plain_{}_labels.png"_fmt(local ? U"local" : U"full"));
		}
	});
	runner.add(U"Comprehensive.MinorRoadSignals",[](TestContext& context)
	{
		for(const int scenario:{0,1,2,3})
		{
			RoadNetwork roads;const Vec3 center{500,30,500};const int node=roads.addNode(center);
			for(int arm=0;arm<4;++arm)
			{
				const double angle=arm*Math::HalfPi;const Vec3 end=center+Vec3{Cos(angle)*100,0,Sin(angle)*100};
				const RoadType type=scenario==2 && arm%2==0 ? RoadType::Arterial : RoadType::LocalRoad;
				const int lanes=scenario==3 && arm%2==0 ? 4 : 2;
				const int edge=*roads.addEdge(node,roads.addNode(end),center.lerp(end,1.0/3),center.lerp(end,2.0/3),type,lanes);
				if(scenario==1 && arm%2==0) { roads.getEdge(edge)->farmAccess=true; }
			}
			roads.rebuildNodeConnectivity(node,node);const auto* crossing=roads.getNode(node);
			context.expect(crossing->laneConnections.size()>8,U"Signal policy is exercised on a connected four-way junction");
			context.expect(crossing->signalPlacement.has_value()==(scenario>=2),U"Only major or wide approaches trigger automatic signals");
			if(scenario<2) { for(const auto& attachment:crossing->attachments) { context.expect(attachment.control!=TrafficControl::Signal,U"Unsignalized small streets also retain unsignalized traffic controls"); } }
			if(scenario==0)
			{
				roads.getNode(node)->signalPlacement=SignalPlacement{U"signal_3lamp"};roads.rebuildNodeConnectivity(node,node);
				context.expect(roads.getNode(node)->signalPlacement.has_value(),U"An explicitly placed player signal is preserved");
			}
		}
	});

	runner.add(U"Comprehensive.ZoomTerrainShading",[](TestContext& context)
	{
		const FilePath directory=FileSystem::CurrentDirectory();
		struct Restore {FilePath path;~Restore(){FileSystem::ChangeCurrentDirectory(path);}} restore{directory};
		FileSystem::ChangeCurrentDirectory(directory+U"../../App/");RegisterAssets();
		CityLighting lighting;context.expect(lighting.initialize(),U"Production terrain shader loads");
		const Size size{320,240};const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};
		JSON report;report[U"cases"]=Array<JSON>{};
		for(const float height:{10.0f,60.0f,1500.0f,2300.0f,500.0f})
		{
			World world;flatWorld(world,height);RoadNetwork roads;WorldRenderer renderer;renderer.setAsyncTerrain(false);renderer.setTerrainShader(lighting.terrainShader());
			const Vec3 focus{33280,height,33280};
			if(height==500)
			{
				for(int z=31;z<=33;++z) { for(int x=31;x<=33;++x)
				{
					auto* chunk=world.getChunk({x,z});
					for(int row=0;row<=HEIGHT_CELLS;++row) { for(int col=0;col<=HEIGHT_CELLS;++col)
					{
						const double px=x*CHUNK_SIZE+col*(static_cast<double>(CHUNK_SIZE)/HEIGHT_CELLS),pz=z*CHUNK_SIZE+row*(static_cast<double>(CHUNK_SIZE)/HEIGHT_CELLS);
						chunk->heightMap[row][col]=static_cast<float>(height+(px-focus.x)*.25+(pz-focus.z)*.10);
					} }
					chunk->heightMin=chunk->heightMap[0][0];chunk->heightMax=chunk->heightMap[HEIGHT_CELLS][HEIGHT_CELLS];
				} }
			}
			world.update(focus);Array<double> means;Array<Image> frames;
			for(const double clearance:{4499.0,4501.0})
			{
				const BasicCamera3D camera{size,8_deg,focus+Vec3{0,clearance,-1},focus};
				lighting.update(camera,focus,Vec3{-.4,1,-.3}.normalized(),1,1,[](Vec3,double){});
				{const ScopedRenderTarget3D scope{target.clear(Palette::Black)};const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};Graphics3D::SetCameraTransform(camera);Graphics3D::SetGlobalAmbientColor(ColorF{.5});Graphics3D::SetSunColor(ColorF{.8});Graphics3D::SetSunDirection(Vec3{-.4,1,-.3}.normalized());lighting.bind();renderer.render(world,roads,camera);}
				Graphics3D::Flush();Image pixels;target.readAsImage(pixels);double sum=0;
				for(int y=60;y<180;++y) { for(int x=80;x<240;++x) {const auto c=pixels[y][x];sum+=(c.r+c.g+c.b)/3.0;} }
				means << sum/19200;frames << pixels;
				pixels.save(directory+U"Screenshot/zoom_terrain_{}_{}.png"_fmt(static_cast<int>(height),static_cast<int>(clearance)));
			}
			double difference=0;for(int y=60;y<180;++y) { for(int x=80;x<240;++x) {const auto a=frames[0][y][x],b=frames[1][y][x];difference+=(Abs(int(a.r)-int(b.r))+Abs(int(a.g)-int(b.g))+Abs(int(a.b)-int(b.b)))/3.0;} }
			difference/=19200;JSON row;row[U"height"]=height;row[U"meanBrightness"]=means;row[U"meanChannelDifference"]=difference;report[U"cases"].push_back(row);
			context.expect(difference<3,U"Zooming past the regional terrain threshold preserves ground shading at altitude {} (difference {:.2f})"_fmt(height,difference));
		}
		report.save(directory+U"TestResults/zoom_terrain_shading.json");Graphics3D::SetSunColor(ColorF{1});
	});

	runner.add(U"Comprehensive.RegionalTerrain",[](TestContext& context)
	{
		World world;flatWorld(world);const auto data=RegionalTerrain::build(world);
		context.expect(data.vertices.size()==513*513 && data.indices.size()==512*512*2,U"Regional terrain covers all 64 km at bounded complexity");
		for(const auto& vertex:data.vertices) { if(InRange(vertex.pos.x,32000.0f,33700.0f) && InRange(vertex.pos.z,32000.0f,33700.0f)) { context.expectNear(vertex.pos.y,30,.01,U"Regional vertices retain the actual terrain height"); } }
		GameCamera camera;camera.setState({32768,30,32768},60000,0,1.4f);const auto labels=TownBillboards::layout({{{32768,55,32768},U"山里市",2,U"Yamazato-shi"}},camera,{1280,800},Font{18});context.expect(!labels.isEmpty(),U"City labels remain visible at the maximum overview distance");
	});

	runner.add(U"Comprehensive.SettlementQuarterAndMargin",[](TestContext& context)
	{
		int retained=0;
		for(int index=0;index<10000;++index) { retained+=SettlementPlacement::retainRuralSite(42,{(index%100)*480.0,(index/100)*480.0}); }
		context.expect(InRange(retained,2300,2700),U"Stable location sampling retains one quarter of town and village candidates");
		Array<SettlementPlacement::Candidate> candidates;
		for(int z=0;z<=12000;z+=480) { for(int x=0;x<=12000;x+=480) { candidates << SettlementPlacement::Candidate{{x,z},.7f}; } }
		int count=0;
		for(uint64 seed:{7,42,130,2026})
		{
			for(const auto& site:SettlementPlacement::generate(seed,candidates,{0,0,12000,12000},[](Vec2){return 20.0;}))
			{
				++count;context.expect(InRange(site.center.x,1000.0,11000.0) && InRange(site.center.y,1000.0,11000.0),U"All city, town and village centers avoid the outer kilometre");
			}
		}
		context.expect(count>5,U"Margin validation examines actual generated settlements");
		JSON report;report[U"retainedOf10000"]=retained;report[U"settlements"]=count;report.save(U"TestResults/quarter_settlements.json");
	});
	runner.add(U"Comprehensive.LocalMapTunnel",[](TestContext& context)
	{
		LocalMapView local;local.center={32768,32768};local.zoomIndex=0;
		const RectF rect{0,0,400,428};const Font font{14};const Texture terrain{Image{16,16,Color{100,140,100}}};
		const RenderTexture target{Size{400,428}};Array<int> white;
		for(bool tunnel:{false,true})
		{
			WorldMapView data;data.streets={WorldMapView::Stroke{{{32268,32868},{33268,32868}},{32268,32860,1000,16},6,0,tunnel}};
			{const ScopedRenderTarget2D scope{target.clear(Palette::Black)};local.draw(rect,terrain,font,data);}
			Graphics2D::Flush();Image pixels;target.readAsImage(pixels);int count=0;
			const int row=static_cast<int>(local.toScreen({32768,32868},rect).y);
			for(int x=40;x<360;++x) { count+=pixels[row][x].r>230 && pixels[row][x].g>230; }
			white << count;
		}
		context.expect(white[0]>300 && white[1]>100 && white[1]<white[0]*.8,U"The local map shares the tunnel dash pattern with the expanded map");
	});
	runner.add(U"Comprehensive.PaletteOwnsBufferedInput",[](TestContext& context)
	{
		CommandPalette palette;palette.open();(void)palette.update();
		GameInput::buffer=KeyboardActionBuffer{};GameInput::buffer.update({{0,0,KeyW.code(),true,false}},true);
		context.expect(GameInput::keyboardBlocked() && !GameInput::down(KeyW),U"Palette input cannot also move the camera");
		GameInput::buffer=KeyboardActionBuffer{};GameInput::buffer.update({{0,0,KeyTab.code(),true,false}},true);
		palette.input=U"/road status set 1446 p";(void)palette.update();
		context.expect(palette.input==U"/road status set 1446 planned",U"A short Tab press completes arguments without discarding the road ID");
		GameInput::buffer=KeyboardActionBuffer{};GameInput::buffer.update({{0,0,KeyEnter.code(),true,false}},true);
		context.expect(palette.update()==Optional<String>{palette.input},U"A buffered Enter executes the visible command once");
		GameInput::buffer=KeyboardActionBuffer{};GameInput::buffer.update({{0,0,KeyEscape.code(),true,false}},true);(void)palette.update();
		context.expect(!palette.visible,U"A buffered Escape closes the palette");
		GameInput::buffer=KeyboardActionBuffer{};GameInput::textOwnedFrame=false;
	});
	runner.add(U"Comprehensive.PartialTunnelCartography",[](TestContext& context)
	{
		RegisterAssets();World world;flatWorld(world);RoadNetwork roads;TrainNetwork trains;GameCamera camera;
		const Vec3 a{33000,30,33000},b{33700,30,33000};const int edge=*roads.addEdge(roads.addNode(a),roads.addNode(b),a.lerp(b,1.0/3),a.lerp(b,2.0/3));
		roads.getEdge(edge)->edgeState=EdgeState::Existing;roads.getEdge(edge)->tunnel=true;
		for(int z=31;z<=33;++z) { for(int x=31;x<=33;++x)
		{
			auto* chunk=world.getChunk({x,z});
			for(int row=0;row<=HEIGHT_CELLS;++row) { for(int col=0;col<=HEIGHT_CELLS;++col)
			{
				const double px=x*CHUNK_SIZE+col*(static_cast<double>(CHUNK_SIZE)/HEIGHT_CELLS);
				chunk->heightMap[row][col]=InRange(px,33200.0,33500.0) ? 70.0f : 30.0f;
			} }
		} }
		MinimapRenderer map;map.buildTerrainTexture(world);map.openFullScreen(camera,roads,trains,{});
		double solid=0,dashed=0;
		for(const auto& stroke:map.mapView().streets)
		{
			for(size_t i=1;i<stroke.points.size();++i) { (stroke.tunnel ? dashed : solid)+=stroke.points[i].distanceFrom(stroke.points[i-1]); }
		}
		context.expect(solid>300 && dashed>250 && dashed<350,U"Only the covered middle of a partly tunneled road is dashed");
		context.expectNear(solid+dashed,700,.1,U"Portal splitting neither drops nor duplicates road lengths");
	});
	runner.add(U"Comprehensive.ViaductPiersRendered",[](TestContext& context)
	{
		const FilePath directory=FileSystem::CurrentDirectory();struct Restore {FilePath path;~Restore(){FileSystem::ChangeCurrentDirectory(path);}} restore{directory};FileSystem::ChangeCurrentDirectory(directory+U"../../App/");
		RegisterAssets();World world;flatWorld(world);RoadNetwork roads;
		const Vec3 a{33000,50,33000},b{33045,50,33000};const int edge=*roads.addEdge(roads.addNode(a),roads.addNode(b),a.lerp(b,1.0/3),a.lerp(b,2.0/3));
		roads.getEdge(edge)->edgeState=EdgeState::Existing;roads.getEdge(edge)->useElevation=true;roads.generatePiersForEdge(edge,world);
		const Size size{640,480};const BasicCamera3D camera{size,45_deg,Vec3{33022.5,40,32940},Vec3{33022.5,40,33000}};
		const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};RoadRenderer renderer;
		context.expect(renderer.loadAssets(),U"Production pier and pavement assets load");
		{const ScopedRenderTarget3D scope{target.clear(ColorF{.01})};const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};Graphics3D::SetCameraTransform(camera);Graphics3D::SetGlobalAmbientColor(ColorF{.7});renderer.render(roads,world,ViewFrustum{camera,1000},camera.getEyePosition());}
		Graphics3D::Flush();Image image;target.readAsImage(image);int supports=0;
		for(int y=180;y<330;++y) { for(int x=60;x<580;++x) { supports+=image[y][x].r>40; } }
		context.expect(supports>300,U"Actual road renderer shows supports between the ground and deck");
		image.save(directory+U"Screenshot/comprehensive_piers.png");
	});
	runner.add(U"Comprehensive.PreloadedRoadShadows",[](TestContext& context)
	{
		const FilePath directory=FileSystem::CurrentDirectory();struct Restore {FilePath path;~Restore(){FileSystem::ChangeCurrentDirectory(path);}} restore{directory};FileSystem::ChangeCurrentDirectory(directory+U"../../App/");
		RegisterAssets();World world;flatWorld(world);RoadNetwork roads;
		for(double x:{33000.0,33300.0})
		{
			const Vec3 a{x,30,33000},b{x+45,30,33000};const int id=*roads.addEdge(roads.addNode(a),roads.addNode(b),a.lerp(b,1.0/3),a.lerp(b,2.0/3));roads.getEdge(id)->edgeState=EdgeState::Existing;
		}
		RoadRenderer renderer;context.expect(renderer.loadAssets(),U"Preload comparison uses real road assets");const Size size{640,480};const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};
		const BasicCamera3D first{size,45_deg,Vec3{33022,50,32960},Vec3{33022,30,33000}};
		{const ScopedRenderTarget3D scope{target.clear(ColorF{.01})};const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};Graphics3D::SetCameraTransform(first);renderer.render(roads,world,ViewFrustum{first,1000},first.getEyePosition());}Graphics3D::Flush();
		const BasicCamera3D light{size,60_deg,Vec3{33150,400,32999},Vec3{33150,30,33000}};Array<Image> images;
		for(bool preload:{false,true})
		{
			if(preload) { renderer.preloadFallbacks(roads,world); }
			{const ScopedRenderTarget3D scope{target.clear(ColorF{.01})};const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};Graphics3D::SetCameraTransform(light);renderer.renderShadowCasters({33150,30,33000},1000);}Graphics3D::Flush();Image image;target.readAsImage(image);images << std::move(image);
		}
		int different=0,visible=0;
		for(int y=0;y<size.y;++y) { for(int x=0;x<size.x;++x) { different+=images[0][y][x]!=images[1][y][x];visible+=images[0][y][x].r>40; } }
		context.expect(visible>100 && different==0,U"Preparing unseen roads adds no shadow draw geometry and preserves the rendered image");
	});
}
