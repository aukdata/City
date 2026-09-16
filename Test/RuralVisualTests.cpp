#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/ui/FrameRateGraph.hpp"
#include "src/ui/WorldMapView.hpp"
#include "src/render/WorldRenderer.hpp"
#include "src/ui/MapTerrainLayer.hpp"
#include "src/render/TreeGeometry.hpp"
#include "src/render/TrainRenderer.hpp"
#include "src/render/VehicleRenderer.hpp"
#include "src/render/VehiclePaint.hpp"
#include "src/render/RoadRenderer.hpp"
#include "src/render/CityLighting.hpp"
#include "src/asset/AssetRegistrar.hpp"
#include "src/railway/TrainConsist.hpp"

void registerRuralVisualTests(TestRunner& runner)
{
	runner.add(U"RuralVisual.FrameRateGraph",[](TestContext& context)
	{
		FrameRateGraph graph;
		for (int i=0;i<600;++i) { graph.sample(1.0/60); }
		context.expectNear(graph.latest(),60,.01,U"FPS uses frame intervals, not CPU rendering duration");
		for (int i=0;i<900;++i) { graph.sample(1.0/30); }
		context.expect(graph.count()==300 && Abs(graph.latest()-30)<.01,U"History rolls over and follows a sustained drop");
		const Size size{800,600}; const RenderTexture target{size};
		const Font font{16}; graph.visible=true;
		{ const ScopedRenderTarget2D scope{target.clear(ColorF{.5})}; graph.draw(font,size); }
		Graphics2D::Flush(); Image pixels; target.readAsImage(pixels);
		int green=0;
		for (const auto color:pixels) { green+=color.g>color.r*1.5 && color.g>color.b*1.2; }
		context.expect(green>100,U"The lower-left FPS graph draws its history");
		context.expect(graph.bounds(size).br().y<size.y && graph.bounds(size).x==16,U"Graph stays inside the lower-left screen");
		pixels.save(U"Screenshot/fps_history.png");
	});
	runner.add(U"RuralVisual.ParcelMask",[](TestContext& context)
	{
		World world; world.reserveChunks(); world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		world.installChunkDirect({32,32},HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,20.0f),20,20});
		LandPatch patch;patch.type=LandPatchType::GardenSoil;
		patch.polygon={{32800,32800},{32920,32800},{32920,32900},{32800,32900}};
		RoadNetwork network;WorldRenderer renderer;
		const MeshData surface=renderer.landPatchSurface(world,network,{32,32},patch);
		const Mesh mesh{surface};const Size size{400,300};const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm,HasDepth::Yes};
		int visible[2]{};
		for(int mode=0;mode<2;++mode)
		{
			{
				const ScopedRenderTarget3D scope{target.clear(ColorF{0,0,0,0})};
				const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite,mode==0 ? RasterizerState::SolidCullBack : RasterizerState::SolidCullNone};
				Graphics3D::SetCameraTransform(BasicCamera3D{size,40_deg,Vec3{32860,160,32710},Vec3{32860,20,32850}});
				mesh.draw(ColorF{1});
			}
			Graphics3D::Flush();Image pixels;target.readAsImage(pixels);
			for(const auto color:pixels) { visible[mode]+=color.a>128; }
		}
		JSON report;report[U"backCullPixels"]=visible[0];report[U"twoSidedPixels"]=visible[1];report.save(U"TestResults/parcel_mask.json");
		context.expect(visible[1]>2000 && visible[0]==visible[1],U"Selected parcel faces upward and draws a complete mask with normal culling");
	});
	runner.add(U"RuralVisual.MapQueuedRebuild",[](TestContext& context)
	{
		const Size size{400,300};const Font font{14};
		WorldMapView map;map.center={32768,32768};map.zoom=128;
		const Texture terrain{Image{32,32,Color{80,130,65}}};
		const RenderTexture first{size},second{size};
		{ const ScopedRenderTarget2D scope{first.clear(Palette::Black)};map.draw(size,terrain,font,{0,0}); }
		map.center.x+=100;
		{ const ScopedRenderTarget2D scope{second.clear(Palette::Black)};map.draw(size,terrain,font,{0,0}); }
		Graphics2D::Flush();Image a,b;first.readAsImage(a);second.readAsImage(b);
		int incorrect=0;
		for(int y=80;y<210;++y) { for(int x=20;x<300;++x) { const auto p=a[y][x];incorrect+=Abs(p.r-80)>2 || Abs(p.g-130)>2 || Abs(p.b-65)>2; } }
		JSON report;report[U"incorrectPixels"]=incorrect;report.save(U"TestResults/map_rebuild.json");
		context.expect(incorrect==0,U"Rebuilding the map must preserve previously queued map draws");
	});
	runner.add(U"RuralVisual.MapTextureLifetime",[](TestContext& context)
	{
		MapTerrainLayer terrain;const Size size{64,64};
		terrain.update({1000,1000,100,100},size,[](Vec2){return 10.0f;});
		const RenderTexture expected{size},queued{size};
		{ const ScopedRenderTarget2D scope{expected.clear(Palette::Black)};terrain.texture.draw(); }
		Graphics2D::Flush();Image reference;expected.readAsImage(reference);
		{ const ScopedRenderTarget2D scope{queued.clear(Palette::Black)};terrain.texture.draw(); }
		terrain.invalidate();terrain.update({1000,1000,100,100},size,[](Vec2){return -70.0f;});
		Graphics2D::Flush();Image actual;queued.readAsImage(actual);
		int changed=0;for(int y=0;y<64;++y) { for(int x=0;x<64;++x) { changed+=reference[y][x]!=actual[y][x]; } }
		JSON report;report[U"unexpectedPixels"]=changed;report.save(U"TestResults/map_texture_lifetime.json");
		context.expect(changed==0,U"A terrain refresh cannot overwrite the map already queued for drawing");
	});
	runner.add(U"RuralVisual.TreeDetailApproach",[](TestContext& context)
	{
		const Vec2 mountain{1400,1450};const Point chunk{32,32};
		context.expect(TreeGeometry::nearTile(1128,chunk,{32768-500,1452,32800},mountain),U"Detailed trees appear 500 m ahead even on mountains above 900 m");
		context.expect(!TreeGeometry::nearTile(1128,chunk,{32768-900,1452,32800},mountain),U"Distant crowns remain cheap outside the detail range");
		context.expect(TreeGeometry::nearChunk(chunk,{32768-900,1452,32800},mountain,TreeGeometry::kPrepareDistance),U"Tree preparation starts before the display boundary");
		context.expect(!TreeGeometry::nearTile(1128,chunk,{32800,2600,32800},mountain),U"High aerial cameras keep the inexpensive distant model");
		context.expect(TreeGeometry::nearChunk(chunk,{32768-1250,1452,32800},mountain,TreeGeometry::kRetainDistance),U"Prepared trees survive small return trips across the preload boundary");
	});

	runner.add(U"RuralVisual.TrainSelection",[](TestContext& context)
	{
		const FilePath directory=FileSystem::CurrentDirectory();
		struct Restore { FilePath value; ~Restore() { FileSystem::ChangeCurrentDirectory(value); } } restore{directory};
		FileSystem::ChangeCurrentDirectory(directory+U"../../App/");
		TrainNetwork network;const int a=network.addStation({100,20,100},U"始点"),b=network.addStation({600,40,100},U"終点");
		const int edge=network.addEdge(a,b,{260,26,100},{430,34,100},60);
		Train train;train.currentEdge=edge;train.routeEdges={edge};train.routeProgress=0;train.forward=true;train.arcPos=250;train.type=TrainType::Local;
		const auto pose=TrainConsist::carPose(train,network,0);
		context.expect(pose.has_value(),U"A sloped train has a selectable car pose");if (!pose) { return; }
		const Vec3 eye=pose->position+Vec3{0,24,-55};
		context.expect(TrainConsist::hitDistance(train,network,Ray{eye,(pose->position+Vec3{0,1.8,0}-eye).normalized()}).has_value(),U"Clicking a sloped car hits its rendered body");
		context.expect(!TrainConsist::hitDistance(train,network,Ray{eye,Vec3{0,1,0}}),U"Clicking sky does not select a train");
		const Size size{400,300};const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm,HasDepth::Yes};TrainRenderer renderer;
		{ const ScopedRenderTarget3D scope{target.clear(ColorF{0,0,0,0})};const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
		Graphics3D::SetCameraTransform(BasicCamera3D{size,40_deg,eye,pose->position});renderer.drawTrainSilhouette(train,network,eye,ColorF{1}); }
		Graphics3D::Flush();Image pixels;target.readAsImage(pixels);int visible=0;for(const auto pixel:pixels) { visible+=pixel.a>128; }
		context.expect(visible>1000,U"The complete train creates the same selection mask used for yellow building outlines");
		pixels.save(directory+U"Screenshot/train_selection.png");
	});

	runner.add(U"RuralVisual.VehiclePaintAcrossLod",[](TestContext& context)
	{
		const FilePath directory=FileSystem::CurrentDirectory();struct Restore { FilePath value;~Restore(){FileSystem::ChangeCurrentDirectory(value);} } restore{directory};
		FileSystem::ChangeCurrentDirectory(directory+U"../../App/");RegisterAssets();
		Array<Vehicle> vehicles;HashSet<uint32> colors;
		for (int id=0;id<100;++id)
		{
			Vehicle car;car.id=id;car.type=VehicleType::PassengerCar;const auto paint=VehiclePaint::color(car);colors.insert(paint.toColor().asUint32());
			if ((paint.r>paint.b*3 || paint.b>paint.r*3) && vehicles.size()<8) { car.position={static_cast<double>(vehicles.size()%4)*7,0,static_cast<double>(vehicles.size()/4)*8};vehicles << car; }
		}
		context.expect(colors.size()>=10,U"Ordinary traffic has a stable ten-color palette");
		const Size size{640,480};const Vec3 eye{14,20,-26};const BasicCamera3D camera{size,40_deg,eye,Vec3{10,0,5}};
		const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};VehicleRenderer renderer;CityLighting lighting;lighting.initialize();
		for (int level=0;level<3;++level)
		{
			{ const ScopedRenderTarget3D scope{target.clear(ColorF{.12})};const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
			Graphics3D::SetCameraTransform(camera);Graphics3D::SetSunDirection(Vec3{1,1,-1}.normalized());Graphics3D::SetSunColor(ColorF{.8});Graphics3D::SetGlobalAmbientColor(ColorF{.4});lighting.bind();
			renderer.render(vehicles,level==0 ? eye : Vec3{0,level==1 ? 180.0 : 900.0,0}); }
			Graphics3D::Flush();Image pixels;target.readAsImage(pixels);int red=0,blue=0;
			for (const auto pixel:pixels) { red+=pixel.r>pixel.b*1.7 && pixel.r>50;blue+=pixel.b>pixel.r*1.7 && pixel.b>50; }
			context.expect(red>80 && blue>80,U"Both red and blue paint remain visible in each actual LOD");pixels.save(directory+U"Screenshot/vehicle_paint_{}.png"_fmt(level));
		}
	});
	runner.add(U"RuralVisual.RoadDetailPreparesAtNearEnd",[](TestContext& context)
	{
		const FilePath directory=FileSystem::CurrentDirectory();struct Restore { FilePath value;~Restore(){FileSystem::ChangeCurrentDirectory(value);} } restore{directory};
		FileSystem::ChangeCurrentDirectory(directory+U"../../App/");RegisterAssets();World world;world.reserveChunks();world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		for (int x=0;x<4;++x) { world.installChunkDirect({x,0},HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,20.0f),20,20}); }
		RoadNetwork network;const Vec3 a{10,20,500},b{3900,20,500};const int first=network.addNode(a),last=network.addNode(b);const auto road=network.addEdge(first,last,a.lerp(b,1.0/3),a.lerp(b,2.0/3),RoadType::LocalRoad,2);context.expect(road.has_value(),U"Road fixture exists");if (!road) { return; }
		auto* edge=network.getEdge(*road);edge->edgeState=EdgeState::Open;for (auto& part:edge->parts) { part.build=BuildState::Built; }
		RoadRenderer renderer;renderer.loadAssets();const Size size{400,300};const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm,HasDepth::Yes};
		for (int approach=0;approach<2;++approach)
		{
			const Vec3 eye{10,100,500-(approach==0 ? 1500.0 : 1000.0)};const BasicCamera3D camera{size,50_deg,eye,Vec3{1000,20,500}};
			{ const ScopedRenderTarget3D scope{target.clear(ColorF{.1})};const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};Graphics3D::SetCameraTransform(camera);renderer.render(network,world,ViewFrustum{camera,8000},eye); }
			Graphics3D::Flush();const auto stats=renderer.cacheBuildStats();
			JSON report;report[U"edges"]=stats.edges;report[U"markings"]=stats.markings;report[U"furniture"]=stats.furniture;report.save(directory+U"TestResults/road_detail_{}.json"_fmt(approach));
			context.expect(approach==0 ? stats.furniture>0 && stats.markings>0 : stats.furniture==0 && stats.edges==0,U"Long roads preload their details before reaching the near-end display range");
		}
	});

}
