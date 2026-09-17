#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/ui/Camera.hpp"
#include "src/render/FrontageGeometry.hpp"
#include "src/ui/WorldMapView.hpp"
#include "src/ui/CollapsibleHudPanel.hpp"
#include "src/ui/LandParcelPanel.hpp"
#include "src/road/RoadMarkingGenerator.hpp"
#include "src/render/WorldRenderer.hpp"
#include "src/render/TreeGeometry.hpp"
#include "src/render/CityLighting.hpp"
#include "src/render/UIRenderer.hpp"
#include "src/asset/AssetRegistrar.hpp"

void registerUrbanUsabilityTests(TestRunner& runner)
{
	runner.add(U"UI.FullScreenMap", [](TestContext& context)
	{
		WorldMapView map; const Size size{1000,600}; map.open({32768,32768});
		const Vec2 cursor{620,360}, anchor=map.toWorld(cursor,size);
		map.zoomAt(cursor,3,size);
		context.expect(map.toWorld(cursor,size).distanceFrom(anchor)<1e-6,U"Zoom stays anchored to the cursor");
		map.pan({80,0},size);
		context.expect(map.toWorld(cursor,size).x>anchor.x,U"Dragging right reveals the west side");
		map.zoom=128;map.center={32768,32768};
		for (int index=-4;index<=4;++index)
		{
			const double x=32768+index*120;
			map.streets << WorldMapView::Stroke{{Vec2{x,32000},Vec2{x,33500}},RectF{x-1,32000,2,1500},index==0 ? 13.0 : 5.0,index==0 ? 1 : 0};
			map.streets << WorldMapView::Stroke{{Vec2{32000,x},Vec2{33500,x}},RectF{32000,x-1,1500,2},6,1};
		}
		map.labels << WorldMapView::Label{{32700,32700},U"中央駅",true};
		map.showContext({560,330},size);
		const Vec2 destination=*map.contextWorld;
		const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm}; const Font font{16};
		{ const ScopedRenderTarget2D render{target}; map.draw(size,Texture{},font,{32768,32768}); }
		Graphics2D::Flush(); Image result;target.readAsImage(result); result.save(U"Screenshot/fullscreen_map.png");
		context.expect(result[Point{8,8}].r>240 && result[Point{10,300}].r>180,U"Map covers the full display and draws an opaque header");
		const auto jump=map.jumpFromMenu(map.menuBounds().center());
		context.expect(jump && jump->distanceFrom(destination)<1e-6 && !map.visible,U"Context menu jumps to the clicked world point and closes the map");
		map.open({0,0});map.close();context.expect(!map.visible && !map.contextWorld,U"Closing clears modal state");
	});
	runner.add(U"UI.CollapsibleHudAndParcelPreview",[](TestContext& context)
	{
		CollapsibleHudPanel left,right;
		right.expanded={410,10,360,360};
		const Font font{20};
		LandPatch patch;patch.type=LandPatchType::PaddyField;patch.polygon={{0,0},{80,0},{80,50},{0,50}};
		const RenderTexture target{Size{800,440},TextureFormat::R8G8B8A8_Unorm};
		for (int state=0;state<2;++state)
		{
			{ const ScopedRenderTarget2D rt{target.clear(ColorF{.24,.32,.2})};
				left.draw(font,U"街の情報");right.draw(font,U"街の動き・地図");
				if (!left.collapsed) { LandParcelPanel::draw(font,patch,{20,56}); }
				if (!right.collapsed) { RectF{558,150,200,200}.draw(ColorF{.25,.4,.3}); }
			}
			Graphics2D::Flush();Image result;target.readAsImage(result);
			result.save(U"Screenshot/hud_parcel_{}.png"_fmt(state));
			const Color background=result[Point{390,100}], body=result[Point{350,200}];
			context.expect(state ? body==background : body.r<background.r,U"Collapsed HUD restores the full game viewport below its header");
			context.expect(left.click(left.toggleBounds().center()),U"Left toggle is clickable");
			context.expect(right.collapsed==static_cast<bool>(state),U"Panels fold independently");
			right.click(right.toggleBounds().center());
		}
		context.expect(!left.click({20,60}),U"Content cannot toggle the header");
	});
	runner.add(U"LandPlot.ConcaveSelection",[](TestContext& context)
	{
		Chunk chunk;
		LandPatch patch;patch.id=17;patch.polygon={{10,10},{110,10},{110,50},{50,50},{50,110},{10,110}};
		for (const auto type : {LandPatchType::FarmField,LandPatchType::PaddyField,LandPatchType::GardenSoil})
		{
			patch.type=type;chunk.landPatches={patch};
			context.expect(LandPlot::find(chunk,{30,90})!=nullptr,U"Farm, paddy and residential plots can be selected");
			context.expect(LandPlot::find(chunk,{90,90})==nullptr,U"An empty concave corner does not select its bounding box");
			context.expectNear(LandPlot::shape(patch).area(),6400,.01,U"Area follows the actual plot boundary");
		}
	});
	runner.add(U"Camera.WalkingHeightAndWasd",[](TestContext& context)
	{
		World world;world.reserveChunks();world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		world.installChunkDirect({0,0},HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,23.0f),23,23});
		for (const float yaw : {0.0f,1.2f,3.1f,4.8f})
		{
			GameCamera captureCamera;captureCamera.setWalkingState({512,23,512},yaw);
			context.expect(captureCamera.mode()==CameraMode::FirstPerson,U"Review captures use the actual walking camera");
			context.expectNear(captureCamera.eyePosition().y,24.5,.0001,U"Capture position uses exactly the normal 1.5 m eye height");
			GameCamera camera;camera.setState({512,23,512},100,yaw,.5f);camera.cycleMode();camera.walk({0,0},0,world);
			context.expectNear(camera.eyePosition().y,24.5,.0001,U"Eye remains exactly 1.5 m above terrain");
			const Vec3 eye=camera.eyePosition(),forward=camera.camera3D().getLookAtVector();
			const auto frame=camera.camera3D();
			camera.walk({-1,0},1,world);const Vec3 left=camera.eyePosition()-eye;
			const auto a=frame.worldToScreenPoint(Float3{eye+forward*10+left});
			context.expect(a.x<Scene::Width()*.5,U"A moves left on screen at every yaw");
			camera.walk({1,0},1,world);camera.walk({0,-1},1,world);
			context.expect((camera.eyePosition()-eye).dot(forward)<-.99,U"S moves backward rather than sideways");
			camera.walk({0,1},1,world);
			context.expect(camera.eyePosition().distanceFrom(eye)<.0001,U"Opposite keys undo the displacement");
		}
	});
	runner.add(U"RoadMarkings.SeamRequiresBothApproaches",[](TestContext& context)
	{
		RoadNetwork network;const int a=network.addNode({-100,0,0}),b=network.addNode({0,0,0}),c=network.addNode({100,0,0});
		const int first=*network.addEdge(a,b,{-66,0,0},{-33,0,0},RoadType::Arterial,2);
		const int second=*network.addEdge(b,c,{33,0,0},{66,0,0},RoadType::Arterial,2);
		for (const int id : {first,second}) { network.getEdge(id)->edgeState=EdgeState::Open; }
		context.expect(!RoadMarkingGenerator::collectNode(network,b,true).isEmpty(),U"Two marked approaches retain their seam");
		RoadMarkingPlacement suppression;suppression.kind=RoadMarkingKind::LaneLine;suppression.scope=RoadMarkingScope::Edge;suppression.edgeId=first;suppression.overrideMode=RoadMarkingOverrideMode::Suppress;
		network.addManualMarking(suppression);
		context.expect(RoadMarkingGenerator::collectNode(network,b,true).isEmpty(),U"An explicitly suppressed approach cannot leave seam-only lines");
		network.clearManualMarkings();
		network.getEdge(first)->roadType=RoadType::LocalRoad;
		context.expect(RoadMarkingGenerator::collectNode(network,b,true).isEmpty(),U"An unmarked approach suppresses the seam");
		network.getEdge(second)->roadType=RoadType::LocalRoad;
		context.expect(RoadMarkingGenerator::collectNode(network,b,true).isEmpty(),U"Two unmarked local roads never acquire isolated white lines");
	});
	runner.add(U"LandPlot.VisibleSurfaceAndRoadCutouts",[](TestContext& context)
	{
		World world;world.reserveChunks();world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		Grid<float> heights(HEIGHT_CELLS+1,HEIGHT_CELLS+1,10.0f);
		for (int row=0;row<=HEIGHT_CELLS;++row) for (int col=0;col<=HEIGHT_CELLS;++col) { heights[{col,row}]=10+col*.04f+row*.02f; }
		world.installChunkDirect({0,0},HeightMapResult{heights,10,14});
		RoadNetwork roads;const int a=roads.addNode({100,10,60}),b=roads.addNode({100,10,160});
		const int edge=*roads.addEdge(a,b,{100,10,93},{100,10,126},RoadType::Arterial,2);roads.getEdge(edge)->edgeState=EdgeState::Open;
		LandPatch patch;patch.type=LandPatchType::PaddyField;patch.polygon={{60,70},{140,70},{140,150},{60,150}};
		WorldRenderer renderer;const MeshData surface=renderer.landPatchSurface(world,roads,{0,0},patch);
		context.expect(!surface.indices.isEmpty(),U"Visible field surface exists");
		context.expect(LandPlot::containsSurface(surface,{75,110}),U"Visible cultivated ground is selectable");
		context.expect(!LandPlot::containsSurface(surface,{100,110}),U"Road cutout cannot steal a road click");
		for (const auto& vertex : surface.vertices)
		{
			context.expectNear(vertex.pos.y,world.sampleHeight(vertex.pos.x,vertex.pos.z)+patch.elevationOffset,.001,U"Selection outline stays on the rendered sloping ground");
		}
	});
	runner.add(U"Landscape.TreeLodAndMaterialReadback",[](TestContext& context)
	{
		CityLighting lighting;context.expect(lighting.initialize(U"../../App/shaders/hlsl/city_forward.hlsl"),U"Farm, paddy and leaf shaders compile");
		if (!lighting.ready()) { return; }
		TextWriter report{U"TestResults/landscape_detail.txt"};
		for (const bool cedar : {false,true})
		{
			const auto tree=TreeGeometry::build(17,cedar);
			const size_t near=tree.wood.indices.size()+tree.leaves.indices.size(),far=tree.distant.indices.size();
			report << U"cedar={} nearTriangles={} farTriangles={} reduction={:.1f}%"_fmt(cedar,near,far,100.0*(1-static_cast<double>(far)/near));
			context.expect(far*10<near,U"Distant trees use fewer than one tenth of the close-range triangles");
			context.expect(!tree.wood.indices.isEmpty(),U"Near trees include trunks and branching wood");
		}
		const int key=TreeGeometry::materialKey(125,{30,30});
		context.expect(TreeGeometry::nearTile(key,{0,0},{30,30,30}),U"Nearby foliage is detailed");
		context.expect(!TreeGeometry::nearChunk({0,0},{1800,30,1800}),U"Fully distant chunks use merged crowns");
		context.expect(!TreeGeometry::nearTile(key,{0,0},{950,30,950}),U"Far trees in the same chunk do not inherit the near tile's detail");
		const Size size{800,440};const Vec3 focus{0,0,0},sun=Vec3{1,2,-1}.normalized();
		const BasicCamera3D camera{size,45_deg,{0,12,-18},focus};
		MeshData surface;
		surface.vertices={Vertex3D{{-8,0,-8},{0,1,0},{0,0}},Vertex3D{{-8,0,8},{0,1,0},{0,16}},Vertex3D{{8,0,-8},{0,1,0},{16,0}},Vertex3D{{8,0,8},{0,1,0},{16,16}}};
		surface.indices={{0,1,2},{2,1,3}};
		const Mesh ground{surface};
		const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};
		lighting.update(camera,focus,sun,1,1,[](Vec3,double){});
		Array<Image> images;
		for (int variant=0;variant<3;++variant)
		{
			{
				const ScopedRenderTarget3D rt{target.clear(ColorF{.35,.43,.49})};
				const ScopedRenderStates3D states{DepthStencilState::DepthTestWrite,RasterizerState::SolidCullNone};
				Graphics3D::SetCameraTransform(camera);Graphics3D::SetSunDirection(sun);Graphics3D::SetSunColor(ColorF{.9});Graphics3D::SetGlobalAmbientColor(ColorF{.32});lighting.bind();
				const ScopedCustomShader3D shader{variant==0 ? lighting.fieldShader() : lighting.paddyShader()};
				ground.draw(ColorF{1});
				if (variant==2)
				{
					const auto tree=TreeGeometry::build(17,false);
					const ScopedCustomShader3D leafShader{lighting.foliageShader()};
					Mesh wood{tree.wood},leaves{tree.leaves};
					const Transformer3D transform{Mat4x4::Scale(8.0)};wood.draw(ColorF{.16,.085,.04});leaves.draw(ColorF{.09,.16,.04});
				}
			}
			Graphics3D::Flush();Image capture;target.readAsImage(capture);capture.save(U"Screenshot/landscape_material_{}.png"_fmt(variant));images << std::move(capture);
		}
		int differences=0,greenSamples=0;
		for (int y=170;y<370;++y) for (int x=200;x<600;++x)
		{
			const Color field=images[0][y][x],paddy=images[1][y][x];
			differences+=Abs(static_cast<int>(field.r)-paddy.r)+Abs(static_cast<int>(field.g)-paddy.g)+Abs(static_cast<int>(field.b)-paddy.b)>12;
			greenSamples+=field.g>field.r+4;
		}
		report << U"fieldPaddyDifferentPixels={} cropPixels={}"_fmt(differences,greenSamples);
		context.expect(differences>10000,U"Water-filled rice paddies and dry cultivated soil are visibly distinct");
		context.expect(greenSamples>1000,U"Cultivated crop rows are visible on the GPU");
	});
	runner.add(U"UI.IntegratedHudLayout",[](TestContext& context)
	{
		RegisterAssets();UIRenderer hud;CityHudStats stats;GameClock clock;Economy economy;
		stats.activeEventSummaries={U"道路工事中",U"春のお祭り",U"通勤時間帯"};stats.notificationSummaries={U"まちの開発が進んでいます",U"農地をクリックして選択"};
		hud.updateLayout(stats);const auto map=hud.minimapBounds();
		context.expect(map.has_value() && map->y>=150,U"Map is below all event and notice rows");
		const RenderTexture target{Scene::Size(),TextureFormat::R8G8B8A8_Unorm};
		{ const ScopedRenderTarget2D rt{target.clear(ColorF{.24,.32,.2})};hud.render(clock,20,U"",economy,stats);if(map) map->draw(ColorF{.25,.4,.3}); }
		Graphics2D::Flush();Image capture;target.readAsImage(capture);capture.save(U"Screenshot/hud_integrated.png");
		context.expect(capture.size()==Scene::Size(),U"Actual production HUD renders in the Test application");
	});

	runner.add(U"StreetDetail.FacadeAttachments",[](TestContext& context)
	{
		for (const bool commercial : {false,true})
		{
			for (const uint32 seed : {0u,1u,17u})
			{
				const auto parts=FrontageGeometry::build(8,-3,commercial,seed);
				int signs=0;
				for (const auto& part : parts)
				{
					signs+=part.material==134;
					for (const auto& vertex : part.mesh.vertices)
					{
						context.expect(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y) && std::isfinite(vertex.pos.z),U"Frontage vertices are finite");
						context.expect(vertex.pos.y>=-.001 && vertex.pos.y<3,U"Entry height material={} y={} (expected 0..3 m)"_fmt(part.material,vertex.pos.y));
						context.expect(vertex.pos.z>=-3.76 && vertex.pos.z<=-2.89 && Abs(vertex.pos.x)<4,U"Details stay within the facade's 75 cm entrance/service strip");
					}
					for (const auto triangle : part.mesh.indices)
					{
						context.expect(triangle.i0<part.mesh.vertices.size() && triangle.i1<part.mesh.vertices.size() && triangle.i2<part.mesh.vertices.size(),U"Batched details use valid triangle indices");
					}
				}
				context.expectEqual(signs,commercial ? 1 : 0,U"Only commercial entrances get a shop fascia");
			}
		}
		const auto metadata=TOMLReader{U"../../App/assets/buildings/residential/residential_001.toml"};
		context.expectNear(metadata[U"front_wall_z_m"].get<double>(),-2.80,.02,U"A projecting roof does not move the entry away from the ground-floor wall");
	});
	runner.add(U"StreetDetail.MaterialGpuReadback",[](TestContext& context)
	{
		CityLighting lighting;
		context.expect(lighting.initialize(U"../../App/shaders/hlsl/city_forward.hlsl"),U"All production city shaders including building glass compile");
		const PixelShader asphalt=HLSL{U"../../App/shaders/hlsl/city_forward.hlsl",U"Asphalt_PS"};
		const PixelShader pavement=HLSL{U"../../App/shaders/hlsl/city_forward.hlsl",U"Pavement_PS"};
		context.expect(static_cast<bool>(asphalt) && static_cast<bool>(pavement),U"Asphalt and pavement shaders compile on Direct3D");
		if (!lighting.ready() || !asphalt || !pavement) { return; }
		const Size size{640,400};
		const BasicCamera3D camera{size,50_deg,{0,5,-7},{0,0,0}};
		const Vec3 sun=Vec3{1,2,-1}.normalized();
		MeshData surface;
		surface.vertices={Vertex3D{{-6,0,-6},{0,1,0},{0,0}},Vertex3D{{-6,0,6},{0,1,0},{0,12}},Vertex3D{{6,0,-6},{0,1,0},{12,0}},Vertex3D{{6,0,6},{0,1,0},{12,12}}};
		surface.indices={{0,1,2},{2,1,3}};
		const Mesh ground{surface};
		const Texture normal{U"../../App/assets/third_party/polyhaven/asphalt_floor/asphalt_floor_nor_gl_1k.jpg",TextureDesc::Mipped};
		const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};
		lighting.update(camera,{0,0,0},sun,1,1,[](Vec3,double){});
		Array<Image> captures;
		for (int variant=0;variant<3;++variant)
		{
			{
				const ScopedRenderTarget3D rt{target.clear(ColorF{.15,.20,.27})};
				const ScopedRenderStates3D states{DepthStencilState::DepthTestWrite,RasterizerState::SolidCullBack};
				Graphics3D::SetCameraTransform(camera);Graphics3D::SetSunDirection(sun);Graphics3D::SetSunColor(ColorF{.9});Graphics3D::SetGlobalAmbientColor(ColorF{.32});
				lighting.bind();Graphics3D::SetPSTexture(4,normal);
				const ScopedCustomShader3D shader{variant==0 ? lighting.shader() : variant==1 ? pavement : asphalt};
				ground.draw(ColorF{.36,.31,.26});
			}
			Graphics3D::Flush();Image capture;target.readAsImage(capture);capture.save(U"Screenshot/street_material_{}.png"_fmt(variant));captures << std::move(capture);
		}
		int joints=0,coolAsphalt=0;
		for (int y=160;y<350;++y) for (int x=100;x<540;++x)
		{
			joints+=static_cast<int>(captures[0][y][x].r)-captures[1][y][x].r>15;
			coolAsphalt+=(static_cast<int>(captures[0][y][x].r)-captures[0][y][x].b)>(static_cast<int>(captures[2][y][x].r)-captures[2][y][x].b)+10;
		}
		context.expect(joints>1000,U"Pavement has visible joints rather than an untextured strip");
		context.expect(coolAsphalt>1000,U"Asphalt loses its previous brown cast");
	});

}
