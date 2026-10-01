#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "../src/ui/WorldMapView.hpp"
#include "../src/ui/LocalMapView.hpp"
#include "../src/ui/NavigationHeader.hpp"
#include "../src/ui/TownBillboards.hpp"
#include "../src/ui/MapTerrainLayer.hpp"
#include "../src/render/RoadRouteSignRenderer.hpp"
#include "../src/asset/AssetRegistrar.hpp"
#include "../src/road/RoadSign.hpp"
#include "../src/road/GuideSign.hpp"
#include "../src/railway/RailDepotBuilder.hpp"
#include "../src/gen/RailwayAlignment.hpp"
#include "../src/render/TreeGeometry.hpp"
#include "../src/render/TunnelGeometry.hpp"
#include "../src/render/TransportLandscape.hpp"
#include "../src/road/RoadPlanDraft.hpp"
#include "../src/road/RoadGeometry.hpp"

namespace
{
	bool hitsMesh(const MeshData& mesh,Vec3 from,Vec3 direction,double maximumDistance=4)
	{
		for (const auto face : mesh.indices)
		{
			const Vec3 a{mesh.vertices[face.i0].pos},b{mesh.vertices[face.i1].pos},c{mesh.vertices[face.i2].pos};
			const Vec3 first=b-a,second=c-a,cross=direction.cross(second);
			const double determinant=first.dot(cross);
			if (Abs(determinant)<1e-9) { continue; }
			const Vec3 delta=from-a;
			const double u=delta.dot(cross)/determinant,v=direction.dot(delta.cross(first))/determinant;
			const double distance=second.dot(delta.cross(first))/determinant;
			if (u>=0 && v>=0 && u+v<=1 && distance>=0 && distance<=maximumDistance) { return true; }
		}
		return false;
	}
}

void registerMapTransportTests(TestRunner& runner)
{
	runner.add(U"MapSigns.TownBillboardLayout", [](TestContext& context)
	{
		RegisterAssets(); const Font font = FontAsset(Asset::CJK24);
		GameCamera camera; camera.setState({30000, 20, 30000}, 1800, 0, .8f);
		Array<TownBillboards::Place> places{{{30000, 50, 30000}, U"城下町", 2}, {{30000, 50, 30000}, U"重なる村", 0}, {{30600, 50, 30000}, U"山里", 0}};
		const auto labels = TownBillboards::layout(places, camera, Scene::Size(), font);
		context.expect(!labels.isEmpty() && labels.front().name == U"城下町", U"Town label follows its world anchor and takes priority over overlapping village");
		for (size_t i = 0; i < labels.size(); ++i)
			for (size_t j = i + 1; j < labels.size(); ++j) { context.expect(!labels[i].bounds.intersects(labels[j].bounds), U"Name billboards do not overlap"); }
		if (!labels.isEmpty())
		{
			const auto hidden = TownBillboards::layout(places, camera, Scene::Size(), font, {labels.front().bounds});
			context.expect(hidden.isEmpty() || hidden.front().name != U"城下町", U"Actual HUD rectangles exclude anchored labels");
		}
		const RenderTexture target{Scene::Size()};
		{ const ScopedRenderTarget2D scope{target.clear(ColorF{.1})}; TownBillboards::draw(labels, font); } Graphics2D::Flush();
		Image image; target.readAsImage(image); image.save(U"Screenshot/town_billboards.png");
		int glyphPixels = 0, strayPixels = 0;
		for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x)
		{
			if (image[y][x].r < 160) { continue; }
			bool inside = false; for (const auto& label : labels) { inside |= label.bounds.stretched(4, 15).contains(Vec2{x, y}); }
			if (inside) { ++glyphPixels; } else { ++strayPixels; }
		}
		context.expect(glyphPixels > 40 && strayPixels == 0, U"GPU readback has readable glyphs only inside label bounds, with no atlas spill");
		camera.setWalkingState({30000, 20, 30000}, 0);
		context.expect(TownBillboards::layout(places, camera, Scene::Size(), font).isEmpty(), U"Walking keeps the forward view free of town billboards");
		camera.setDrivingState({30000, 20, 30000}, 0, 0);
		context.expect(TownBillboards::layout(places, camera, Scene::Size(), font).isEmpty(), U"Driving uses the fixed header instead of world billboards");
	});

	runner.add(U"MapSigns.RouteHeaderSelection",[](TestContext& context)
	{
		RegisterAssets();RoadNetwork network;const int a=network.addNode({100,20,100}),b=network.addNode({100,20,300});
		const int edge=*network.addEdge(a,b,{100,20,167},{100,20,233});network.getEdge(edge)->edgeState=EdgeState::Existing;
		const int route=network.addRoute(RoadRouteKind::NationalRoute,U"国道178号",{edge},178);
		GameCamera camera;camera.setWalkingState({100,20,200},0);RoadRouteSignRenderer renderer;
		const RenderTexture target{Scene::Size()};
		{const ScopedRenderTarget2D scope{target.clear(ColorF{.1})};renderer.render(network,camera);}Graphics2D::Flush();
		Image image;target.readAsImage(image);image.save(U"Screenshot/national_route_header.png");
		context.expect(renderer.hitTest(NavigationHeader::routeBounds(Scene::Size()).center())==route,U"The actual national-road header opens the road beneath the camera");
		context.expect(!renderer.hitTest(Scene::CenterF()),U"No invisible route hit area remains in the forward view");
		camera.setOverviewState({100,20,200},800,0,.8f);renderer.render(network,camera);
		context.expect(!renderer.hitTest(NavigationHeader::routeBounds(Scene::Size()).center()),U"Overview has no fixed route header hit area");
		camera.setDrivingState({100,20,200},0,0);renderer.render(network,camera);
		context.expect(renderer.hitTest(NavigationHeader::routeBounds(Scene::Size()).center())==route,U"Driving retains the top route header");
		camera.setWalkingState({10000,20,10000},0);renderer.render(network,camera);
		context.expect(!renderer.hitTest(NavigationHeader::routeBounds(Scene::Size()).center()),U"Leaving the road removes its stale clickable label");
	});

	runner.add(U"MapSigns.VisibleTerrainDetail",[](TestContext& context)
	{
		MapTerrainLayer tile;int samples=0;
		const auto height=[&](Vec2 p){++samples;return static_cast<float>(90+60*Math::Sin(p.x/18)*Math::Cos(p.y/28));};
		context.expect(tile.update({30000,30000,500,500},{512,512},height),U"First local tile samples actual terrain");
		context.expect(tile.metersPerPixel<1.5,U"A 500 m map has metre-scale terrain sampling instead of 128 m texels");
		const int before=samples;
		context.expect(!tile.update({30001,30001,500,500},{512,512},height) && samples==before,U"Small camera steps reuse the padded terrain tile");
		const RenderTexture target{512,512};{const ScopedRenderTarget2D scope{target};tile.texture.draw();}Graphics2D::Flush();
		Image image;target.readAsImage(image);image.save(U"Screenshot/map_terrain_detail.png");
		int changes=0;for (int y=10;y<500;y+=10) for (int x=10;x<500;++x) {changes+=Abs(int(image[y][x].g)-int(image[y][x-1].g))>2;}
		context.expect(changes>3000,U"Narrow valleys and slopes remain visible at neighbourhood scale");
		context.expect(tile.update({30125,30125,250,250},{512,512},height) && tile.metersPerPixel<.75,U"Zooming in resamples terrain");
	});
	runner.add(U"MapSigns.TerrainTextureRegistration",[](TestContext& context)
	{
		Image source{64,64,Palette::Red};for (int y=0;y<64;++y) for (int x=32;x<64;++x) {source[y][x]=Palette::Blue;}
		const Texture terrain{source}; const RectF region{29000,29000,2000,2000};const RectF rect{0,0,300,300};
		const Font font{14};WorldMapView data;LocalMapView map;map.follow({30000,30000},{0,-1});map.zoomIndex=2;
		for (const Vec2 direction : {Vec2{0,-1},Vec2{1,0},Vec2{0,1},Vec2{-1,0}})
		{
			map.headingUp=true;map.follow(map.center,direction);const RenderTexture target{300,300};
			{const ScopedRenderTarget2D scope{target.clear(Palette::Black)};map.draw(rect,terrain,font,data,region);}Graphics2D::Flush();
			Image image;target.readAsImage(image);
			const Point red=map.toScreen({29600,30000},rect).asPoint(),blue=map.toScreen({30400,30000},rect).asPoint();
			context.expect(image[red].r>200 && image[red].b<30 && image[blue].b>200 && image[blue].r<30,U"Terrain and vector coordinates remain aligned for all four camera headings");
		}
		data.open(map.center);data.zoom=32;const Size size{960,640};const RenderTexture target{size};
		{const ScopedRenderTarget2D scope{target};data.draw(size,terrain,font,map.center,region);}Graphics2D::Flush();
		Image image;target.readAsImage(image);const Point red=data.toScreen({29600,30000},size).asPoint(),blue=data.toScreen({30400,30000},size).asPoint();
		context.expect(image[red].r>200 && image[blue].b>200 && red.x>blue.x,U"Full and local maps share terrain orientation");
	});
	runner.add(U"MapSigns.GpuFrontAndBack",[](TestContext& context)
	{
		const auto directory=FileSystem::CurrentDirectory();FileSystem::ChangeCurrentDirectory(directory+U"../../App/");
		Image artwork{128,128,Palette::White};for (int y=0;y<128;++y) for (int x=0;x<128;++x)
		{artwork[y][x]=(x<64 && y<64) ? Palette::Red : Palette::Blue;}
		const Texture texture{artwork};const Size size{320,320};
		for (int kind=0;kind<5;++kind)
		{
			const auto type=std::array<RoadSignType,4>{RoadSignType::Stop,RoadSignType::SpeedLimit,RoadSignType::NationalRoute,RoadSignType::CurveWarning}[Min(kind,3)];
			const auto frontData=kind==4 ? GuideSign::CreateBoardMesh(.9,.6) : RoadSign::CreateBoardMesh(type);
			const Mesh front{frontData},back{RoadSign::CreateBoardBackingMesh(frontData)};
			for (int facing=0;facing<2;++facing) for (int approach=0;approach<2;++approach)
			{
				const Mat4x4 transform=Mat4x4::RotateY(facing==0 ? 0.0f : static_cast<float>(Math::Pi));
				const double side=(facing==0 ? -1.0 : 1.0)*(approach==0 ? 1 : -1);
				const BasicCamera3D camera{size,35_deg,Vec3{0,0,side*2},Vec3{0,0,0}};
				const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};
				{
					const ScopedRenderTarget3D scope{target.clear(ColorF{.12})};
					const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite,RasterizerState::SolidCullBack};
					Graphics3D::SetCameraTransform(camera);Graphics3D::SetGlobalAmbientColor(ColorF{1});Graphics3D::SetSunColor(ColorF{0});
					back.draw(transform,ColorF{.55});front.draw(transform,texture);
				}Graphics3D::Flush();Image image;target.readAsImage(image);
				int saturated=0,red=0;double redX=0,redY=0;
				for (int y=0;y<320;++y) for (int x=0;x<320;++x)
				{
					const auto pixel=image[y][x];saturated+=Max(pixel.r,Max(pixel.g,pixel.b))-Min(pixel.r,Min(pixel.g,pixel.b))>70;
					if (pixel.r>200 && pixel.b<70) {++red;redX+=x;redY+=y;}
				}
				context.expect(approach==0 ? saturated>5000 : saturated==0,U"Symbols appear only from the approaching driver's side, never through the metal back");
				if (approach==0) {context.expect(red>500 && redX/red<160 && redY/red<160,U"The top-left texture corner stays top-left, without inverted text or turn arrows");}
				if (facing==0) {image.save(directory+U"Screenshot/sign_face_{}_{}.png"_fmt(kind,approach));}
			}
		}
		Graphics3D::SetSunColor(ColorF{1});Graphics3D::SetGlobalAmbientColor(ColorF{.5});FileSystem::ChangeCurrentDirectory(directory);
	});

	runner.add(U"MapSigns.CameraHandedness",[](TestContext& context)
	{
		const Size size{800,600}; const RectF rect{0,0,240,240};
		for (const Vec3 direction : {Vec3{0,0,1},Vec3{1,0,0},Vec3{0,0,-1},Vec3{-1,0,0}})
		{
			const BasicCamera3D camera{size,60_deg,Vec3{0,2,0},direction*100+Vec3{0,2,0}};
			const Vec3 right=tangentToRight(direction);
			const Float3 projected=camera.worldToScreenPoint(Float3{direction*100+right*10+Vec3{0,2,0}});
			LocalMapView map; map.headingUp=true; map.follow({0,0},{direction.x,direction.z});
			const Vec2 onMap=map.toScreen({right.x*100,right.z*100},rect);
			context.expect(projected.x>400 && onMap.x>map.body(rect).center().x,U"A landmark on the driver's right stays on the right of the heading-up map");
		}
	});
	runner.add(U"MapSigns.BoardFrontFaces",[](TestContext& context)
	{
		const auto directory=FileSystem::CurrentDirectory(); FileSystem::ChangeCurrentDirectory(directory+U"../../App/");
		for (auto type : {RoadSignType::Stop,RoadSignType::SpeedLimit,RoadSignType::NationalRoute,RoadSignType::CurveWarning})
		{
			const auto mesh=RoadSign::CreateBoardMesh(type);int wrong=0;
			for (const auto triangle : mesh.indices)
			{
				const Vec3 a{mesh.vertices[triangle.i0].pos},b{mesh.vertices[triangle.i1].pos},c{mesh.vertices[triangle.i2].pos};
				wrong+=(b-a).cross(c-a).z>=0;
			}
			context.expectEqual(wrong,0,U"Only local -Z front faces may carry a road sign symbol");
		}
		const auto guide=GuideSign::CreateBoardMesh(3,2);
		context.expect(guide.vertices.front().normal.z<0,U"Guide boards use the same front as warning and regulatory signs");
		FileSystem::ChangeCurrentDirectory(directory);
	});
	runner.add(U"MapSigns.CurveTravelDirection",[](TestContext& context)
	{
		RoadNetwork network; const int a=network.addNode({0,10,0}),b=network.addNode({200,10,200});
		const auto id=network.addEdge(a,b,{0,10,110},{90,10,200}); context.expect(id.has_value(),U"Build a right-hand bend");
		if (!id) { return; }
		const auto signs=RoadSign::InferAutoForEdge(*network.getEdge(*id),network); int curves=0;
		for (const auto& sign : signs) if (sign.type==RoadSignType::CurveWarning)
		{
			++curves;context.expectEqual(sign.auxValue,sign.nodeEndId==b ? 2 : 1,U"The same curve is right going A to B and left going B to A");
		}
		context.expectEqual(curves,2,U"Both approaches receive a curve warning");
	});
	runner.add(U"MapSigns.NavigationHeaderLayout",[](TestContext& context)
	{
		const Font font{FontMethod::MSDF,24,Typeface::CJK_Regular_JP};
		for (const Size size : {Size{960,640},Size{1280,720},Size{1920,1080}})
		{
			const RenderTexture target{size};
			{ const ScopedRenderTarget2D scope{target.clear(Color{35,55,65})};
				NavigationHeader::drawPlace(size,font,U"山あいの長い名前の中央地区",U"Yamaai Chuo");
				NavigationHeader::drawRoute(size,font,178,U"山あい街道"); }
			Graphics2D::Flush();Image image;target.readAsImage(image);image.save(U"Screenshot/navigation_header_{}.png"_fmt(size.x));
			int lower=0,painted=0;for (int y=0;y<size.y;++y) for (int x=0;x<size.x;++x)
			{ if (image[y][x]!=image[0][0]) { ++painted;lower+=y>=90; } }
			context.expect(painted>2000 && lower==0,U"Location and route are readable at the top and never cover the road ahead");
			const auto bounds=NavigationHeader::placeBounds(size);
			context.expect(bounds.x>=370 && bounds.rightX()<=size.x-370,U"The header fits between the city information panels");
		}
	});

	runner.add(U"MapTransport.CartographySurvivesFontAtlasUpdates",[](TestContext& context)
	{
		const Size size=Scene::Size();
		const Font font{FontMethod::MSDF,14,Typeface::CJK_Regular_JP};
		const Texture terrain{Image{16,16,Color{70,100,140}}};
		WorldMapView map; map.open({32768,32768}); map.zoom=16;
		map.labels<<WorldMapView::Label{{31000,31000},U"山あい中央駅",true};
		const String characters=U"川淵栄西中田梅城下草常願磯浜椿御根北杉林菅峰山松木福岬奥島春寺王市地本東谷大水江秋角宮泉池土芦石長坪街路図運転速度鉄道車庫路線駅乗客";
		// Force multiple 2D vertex/index-buffer batches, as the city map does.
		for (int line=0;line<1200;++line)
		{
			WorldMapView::Stroke street; street.width=3;
			street.bounds={30000,30000,5000,5000};
			for (int point=0;point<300;++point) { street.points << Vec2{30000+point*16.0,30500+line*.05}; }
			map.streets<<std::move(street);
		}
		int worstWhite=0;
		for (size_t frame=0;frame<18;++frame)
		{
			context.expect(System::Update(),U"The real frame boundary remains available during map replay");
			{
				font(characters.substr(0,Min(characters.size(),(frame+1)*4))).draw(10,10);
				map.pan({5,2},size); map.draw(size,terrain,font,{0,0});
			}
			ScreenCapture::RequestCurrentFrame();
			context.expect(System::Update(),U"Present the regenerated map without an explicit flush");
			Image image;context.expect(ScreenCapture::GetFrame(image),U"Read the actual presented frame");
			int white=0;
			for (int y=200;y<450;++y) for (int x=150;x<850;++x)
			{
				const auto pixel=image[y][x];white+=pixel.r>230 && pixel.g>230 && pixel.b>230;
			}
			worstWhite=Max(worstWhite,white);
			if (frame==17) { image.save(U"Screenshot/map_font_atlas.png"); }
		}
		context.expectEqual(worstWhite,0,U"Panning with newly cached MSDF glyphs must not place the font atlas over blank terrain");
	});
	runner.add(U"MapTransport.LocalMapFollowAndControls",[](TestContext& context)
	{
		LocalMapView local; const RectF rect{20,20,240,240}; local.follow({30000,30000},{1,0});
		context.expectNear(local.toScreen(local.center,rect).distanceFrom(local.body(rect).center()),0,1e-8,U"The camera stays at the middle of the local map");
		const Vec2 sample=local.center+Vec2{100,130};
		for (int orientation=0;orientation<2;++orientation)
		{
			local.headingUp=orientation==1;
			context.expectNear(local.toWorld(local.toScreen(sample,rect),rect).distanceFrom(sample),0,1e-7,U"Rotated map coordinates round-trip");
		}
		const Vec2 ahead=local.toScreen(local.center+Vec2{100,0},rect);
		context.expect(ahead.y<local.body(rect).center().y && Abs(ahead.x-local.body(rect).center().x)<1e-8,U"Heading east puts east at the top in heading-up mode");
		const double before=local.span();
		context.expect(local.interact(rect,local.zoomButton(rect,true).center(),true,0)==LocalMapView::Action::None && local.span()<before,U"Zoom controls do not accidentally open the full map");
		local.interact(rect,local.orientationButton(rect).center(),true,0);
		context.expect(!local.headingUp,U"Orientation toggles back to north-up");
		context.expect(local.interact(rect,local.body(rect).center(),true,0)==LocalMapView::Action::OpenFullScreen,U"Clicking map content opens the full map");
		WorldMapView data; data.streets<<WorldMapView::Stroke{{{29500,30000},{30500,30000}},RectF{29500,29999,1000,2},10,1};
		const Font font{FontMethod::MSDF,14}; const Texture terrain{Image{4,4,Color{130,165,110}}};
		for (int orientation=0;orientation<2;++orientation)
		{
			local.headingUp=orientation==1; const RenderTexture target{Size{280,280}};
			const Rect scissor{3,4,260,260};Graphics2D::SetScissorRect(scissor);
			{ const ScopedRenderTarget2D scope{target.clear(Palette::Black)};local.draw(rect,terrain,font,data); }
			context.expect(Graphics2D::GetScissorRect()==scissor,U"The map restores its caller's clipping rectangle");
			Graphics2D::Flush();Image image;target.readAsImage(image);image.save(U"Screenshot/local_map_{}.png"_fmt(orientation));
			const Point center=local.body(rect).center().asPoint();
			context.expect(image[center].r>image[center].g && image[0][0]==Palette::Black,U"The viewpoint marker is visible and drawing stays inside the map bounds");
		}
	});

	runner.add(U"MapTransport.StationAndDepotCurveLimits",[](TestContext& context)
	{
		TrainNetwork tight; const int a=tight.addStation({100,20,200},U"急曲線"),b=tight.addStation({300,20,400},U"終点");
		tight.addEdge(a,b,{250,20,200},{150,20,400});
		context.expect(RailwaySite::stationMinimumRadius(tight,a)<RailwaySite::kStationMinimumRadius,U"A sharp bend cannot qualify as a station or depot entrance");
		World world;world.reserveChunks();world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		world.installChunkDirect({0,0},HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,10),10,10});
		RoadNetwork roads;String error;const auto before=tight.saveState().formatMinimum();
		context.expect(!RailDepotBuilder::add(tight,world,roads,a,error) && tight.saveState().formatMinimum()==before,U"Invalid station curvature rejects a depot before changing any track");
		TrainNetwork straight;const int c=straight.addStation({100,20,200},U"直線"),d=straight.addStation({800,20,200},U"終点");
		straight.addEdge(c,d,{330,20,200},{560,20,200});
		context.expect(RailDepotBuilder::add(straight,world,roads,c,error),U"A longer lead is searched until the depot approach radius is feasible");
		for (const auto& edge : straight.edges())
		{
			if (!edge.depotTrack) { continue; }
			const bool siding=straight.getNode(edge.nodeB)->type==TrackNodeType::Buffer;
			context.expect(straight.getBezier(edge.id)->minimumHorizontalRadius()>=(siding ? RailwaySite::kSidingMinimumRadius : RailwaySite::kDepotApproachMinimumRadius),U"Every accepted lead and siding satisfies its own radius limit");
		}
	});
	runner.add(U"MapTransport.GeneratedStationApproaches",[](TestContext& context)
	{
		World world;world.reserveChunks();world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		for (int z=0;z<7;++z) for (int x=0;x<7;++x) { world.installChunkDirect({x,z},HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,20),20,20}); }
		MapGenerator::Settlement a,b;a.center={1000,1000};b.center={4100,3700};a.name=U"西町";b.name=U"東町";
		a.plan.station=b.plan.station=Vec2{0,0};b.gridAxisX={0,1};b.gridAxisZ={-1,0};
		TrainNetwork network;RailwayAlignment::Audit audit; RailwayAlignment::generate(network,world,{a,b},nullptr,&audit);
		TextWriter diagnostic{U"TestResults/rail_generation_audit.txt"};
		for (const auto& item : audit.candidates) { diagnostic << U"variant={} profile={} radius={} grade={}"_fmt(item.variant,item.profile,item.radius,item.grade); }
		context.expect(!network.schedules().isEmpty(),U"Reserving straight station space still allows an actual terrain-generated route");
		for (const auto& node : network.nodes())
		{
			if (node.type!=TrackNodeType::Station) { continue; }
			context.expect(RailwaySite::stationMinimumRadius(network,node.id)>=RailwaySite::kStationMinimumRadius,U"The full generated platform length meets the station curvature limit");
		}
	});
	runner.add(U"MapTransport.VegetationClearance",[](TestContext& context)
	{
		const Polygon road{Array<Vec2>{{-50,-4},{50,-4},{50,4},{-50,4}}};
		context.expect(!TreeGeometry::clearOfCorridor({0,6.1},15,road),U"A tree previously accepted by the 2m trunk rule is rejected when its crown overlaps the road");
		for (const bool cedar : {false,true}) for (uint32 seed=0;seed<12;++seed)
		{
			const auto tree=TreeGeometry::build(seed,cedar,true);const double width=cedar ? 11.4 : 17.1;
			for (const auto* part : {&tree.wood,&tree.leaves,&tree.distant}) for (const auto& vertex : part->vertices)
			{
				context.expect(Vec2{vertex.pos.x,vertex.pos.z}.length()*width+.8<TreeGeometry::horizontalClearance(width),U"All tree LOD vertices and sway fit within the enforced transport clearance");
			}
		}
		World world;world.reserveChunks();world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		world.installChunkDirect({0,0},HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,30),30,30});
		TrainNetwork rail;const int a=rail.addStation({100,31,300},U"西"),b=rail.addStation({900,31,300},U"東");
		rail.addEdge(a,b,{366,31,300},{633,31,300});
		const auto sites=RailwaySite::landscapeFootprints(rail,world);bool blocked=false;
		for (const auto& site : sites)
		{
			const Polygon corridor{Array<Vec2>{site.begin(),site.end()}};
			blocked|=!TreeGeometry::clearOfCorridor({500,304},15,corridor);
		}
		context.expect(blocked,U"The middle of a railway is excluded from woodland, far beyond its station footprints");
	});
	runner.add(U"MapTransport.TunnelJunctionPassage",[](TestContext& context)
	{
		World world;world.reserveChunks();world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		world.installChunkDirect({0,0},HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,70),70,70});
		RoadNetwork roads;
		const auto add=[&](Vec3 a,Vec3 b)
		{
			const int start=roads.addNode(a),end=roads.addNode(b);
			const int id=*roads.addEdge(start,end,a.lerp(b,1.0/3),a.lerp(b,2.0/3),RoadType::LocalRoad,2);
			auto* edge=roads.getEdge(id);edge->useElevation=true;edge->tunnel=true;edge->edgeState=EdgeState::Open;return id;
		};
		const int horizontal=add({100,40,300},{500,40,300});add({300,40,100},{300,40,500});
		const auto before=TunnelGeometry::build(*roads.getBezier(horizontal),world,roads.getEdge(horizontal)->totalWidth(),false);
		context.expect(hitsMesh(before.lining,{300,41.3,300},{0,0,1},50),U"The old independent lining reproduces a wall across a crossing tunnel");
		context.expect(roads.resolveIntersections(),U"Crossing road tunnels at the same elevation gain a shared graph node");
		int junction=-1;
		for (const auto& node : roads.nodes()) { if (node.attachments.size()==4) { junction=node.id; } }
		context.expect(junction>=0,U"The underground crossing is a four-way intersection");
		if (junction<0) { return; }
		roads.rebuildLaneConnections(junction);
		context.expect(roads.getNode(junction)->laneConnections.size()>=8,U"The junction has drivable turning connections");
		const auto sections=TunnelGeometry::buildRoadNetwork(world,roads);int blocked=0;bool roof=false;
		for (const auto& section : sections)
		{
			for (const Vec3 direction : {Vec3{1,0,0},Vec3{-1,0,0},Vec3{0,0,1},Vec3{0,0,-1}})
			{
				blocked+=hitsMesh(section.geometry.lining,{300,41.3,300},direction,70);
			}
			roof|=hitsMesh(section.geometry.lining,{300,41.3,300},{0,1,0},9);
		}
		TextWriter{U"TestResults/tunnel_junction.txt"}.write(U"blocked={} roof={} sections={}"_fmt(blocked,roof,sections.size()));
		context.expect(blocked==0 && roof,U"All four branches remain open at driver height while the chamber retains a ceiling");
		const Size size{960,600};const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm,HasDepth::Yes};
		{
			const ScopedRenderTarget3D scope{target.clear(ColorF{.03})};const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite,RasterizerState::SolidCullNone};
			Graphics3D::SetCameraTransform(BasicCamera3D{size,70_deg,{300,41.3,278},{305,42,312}});Graphics3D::SetGlobalAmbientColor(ColorF{.8});
			for (const auto& section : sections) { Mesh{section.geometry.lining}.draw(ColorF{.56,.57,.56});if (!section.geometry.lights.indices.isEmpty()) { Mesh{section.geometry.lights}.draw(ColorF{1,.92,.7}); } }
		}
		Graphics3D::Flush();Image image;target.readAsImage(image);image.save(U"Screenshot/tunnel_junction.png");
	});
	runner.add(U"MapTransport.SeparatedTunnelsRemainSeparate",[](TestContext& context)
	{
		RoadNetwork roads;
		for (int line=0;line<2;++line)
		{
			const Vec3 a=line==0 ? Vec3{100,20,300} : Vec3{300,32,100},b=line==0 ? Vec3{500,20,300} : Vec3{300,32,500};
			const int start=roads.addNode(a),end=roads.addNode(b),id=*roads.addEdge(start,end,a.lerp(b,1.0/3),a.lerp(b,2.0/3),RoadType::LocalRoad,2);
			roads.getEdge(id)->tunnel=true;roads.getEdge(id)->useElevation=true;
		}
		context.expect(!roads.resolveIntersections(),U"Tunnels that cross at different elevations do not become a vertical intersection");
	});

	runner.add(U"MapTransport.InheritedShaderIsolation",[](TestContext& context)
	{
#if SIV3D_PLATFORM(WINDOWS)
		const FilePath path=U"TestResults/map_texture_probe.hlsl";
		TextWriter{path}.write(U"Texture2D source : register(t0); SamplerState sampler0 : register(s0); struct Input { float4 position:SV_POSITION; float4 color:COLOR0; float2 uv:TEXCOORD0; }; float4 PS(Input input):SV_TARGET { return source.Sample(sampler0,input.uv)*input.color; }");
		const PixelShader probe=PixelShader::HLSL(path,U"PS");
#else
		const FilePath path=U"TestResults/map_texture_probe.frag";
		TextWriter{path}.write(U"#version 410\nuniform sampler2D Texture0; layout(location=0) in vec4 Color; layout(location=1) in vec2 UV; layout(location=0) out vec4 FragColor; void main(){ FragColor=texture(Texture0,UV)*Color; }");
		const PixelShader probe=GLSL{path, {}};
#endif
		context.expect(static_cast<bool>(probe),U"The texture-state probe compiles");
		const Size size{1000,600};const Font font{FontMethod::MSDF,14,Typeface::CJK_Regular_JP};
		const Texture terrain{Image{8,8,Color{80,120,90}}};const RenderTexture target{size};Array<Image> frames;
		for (int inherited=0;inherited<2;++inherited)
		{
			WorldMapView map;map.open({32768,32768});
			const Rect clip{2,3,99,88};Graphics2D::SetScissorRect(clip);
			{
				const ScopedRenderTarget2D destination{target.clear(Palette::Black)};
				const ScopedCustomShader2D effect{inherited ? probe : PixelShader{}};
				font(U"川淵栄西中田梅城下草常願磯浜椿御根北杉林菅峰山松木福岬奥島春寺王市地本東谷大水江秋角宮泉池土芦石長坪運転アクセル速度").draw(0,0);
				map.draw(size,terrain,font,{0,0});
				context.expect(Graphics2D::GetCustomPixelShader().has_value()==(inherited!=0),U"The map restores the caller's shader");
			}
			context.expect(Graphics2D::GetScissorRect()==clip,U"Full-screen rendering restores its caller's clip rectangle");
			Graphics2D::Flush();Image frame;target.readAsImage(frame);frames<<frame;frame.save(U"Screenshot/map_inherited_{}.png"_fmt(inherited));
		}
		int different=0;
		for (int y=90;y<450;++y) for (int x=20;x<900;++x) { different+=frames[0][y][x]!=frames[1][y][x]; }
		TextWriter{U"TestResults/map_shader_probe.txt"}.write(U"differentPixels={}"_fmt(different));
		context.expectEqual(different,0,U"A caller's texture shader cannot paint a font atlas across the map");
	});
	runner.add(U"MapTransport.PlannedTunnelCrossings",[](TestContext& context)
	{
		World world;world.reserveChunks();world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		world.installChunkDirect({0,0},HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,70),70,70});
		RoadNetwork roads;
		for (const double x : {300.0,600.0})
		{
			const Vec3 a{x,40,100},b{x,40,800};const int start=roads.addNode(a),end=roads.addNode(b);
			const int id=*roads.addEdge(start,end,a.lerp(b,1.0/3),a.lerp(b,2.0/3),RoadType::LocalRoad,2);
			roads.getEdge(id)->useElevation=true;roads.getEdge(id)->tunnel=true;roads.getEdge(id)->edgeState=EdgeState::Open;
		}
		RoadPlanDraft draft;draft.place({100,40,450});draft.place({900,40,450});
		const auto roadTemplate=RoadPlanDraft::makeRoadTemplate(0);context.expect(draft.rebuild(world,roadTemplate),U"A tunnel draft is feasible");
		const auto proposal=draft.propose(roads,world,roadTemplate);context.expect(proposal.has_value(),U"The tunnel can be proposed");if (!proposal) { return; }
		context.expectEqual(proposal->edgeIds.size(),size_t{3},U"Both crossings split only the planned corridor into three ordered parts");
		int junctions=0,existing=0;
		for (const auto& node : proposal->network.nodes()) { junctions+=node.attachments.size()==4; }
		for (const auto& edge : proposal->network.edges())
		{
			if (edge.id<0) { continue; }
			if (proposal->edgeIds.contains(edge.id)) { context.expect(edge.edgeState==EdgeState::Planned,U"The plan retains construction ownership after repeated splits"); }
			else { existing+=edge.edgeState==EdgeState::Open; }
		}
		context.expect(junctions==2 && existing==4,U"Both existing tunnels stay open and join the new branch");
		context.expect(roads.edges().size()==2,U"Proposing a branch does not mutate the live city");
	});
	runner.add(U"MapTransport.BridgeTreeClearance",[](TestContext& context)
	{
		World world;world.reserveChunks();world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		world.installChunkDirect({0,0},HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,30),30,30});
		RoadNetwork roads;const Vec3 a{100,40,300},b{900,40,300};const int start=roads.addNode(a),end=roads.addNode(b);
		const int id=*roads.addEdge(start,end,a.lerp(b,1.0/3),a.lerp(b,2.0/3),RoadType::LocalRoad,2);
		roads.getEdge(id)->useElevation=true;roads.getEdge(id)->edgeState=EdgeState::Open;
		const auto elevated=TransportLandscape::footprints(TrainNetwork{},roads,world);bool overlaps=false;
		for (const auto& site : elevated) { overlaps|=!TreeGeometry::clearOfCorridor({500,307},15,Polygon{Array<Vec2>{site.begin(),site.end()}}); }
		context.expect(overlaps,U"A bridge excludes tree crowns even though its roadbed does not cut the terrain");
		roads.getNode(start)->position.y=roads.getNode(end)->position.y=10;roads.getEdge(id)->ctrlA.y=roads.getEdge(id)->ctrlB.y=10;
		context.expect(TransportLandscape::footprints(TrainNetwork{},roads,world).isEmpty(),U"Forest above a deep tunnel remains present");
	});

}
