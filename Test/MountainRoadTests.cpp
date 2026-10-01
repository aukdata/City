#include "TestCases.hpp"
#include "src/render/ShaderAsset.hpp"
#include "TestRunner.hpp"
#include "../src/render/TunnelRenderer.hpp"
#include "../src/render/WorldRenderer.hpp"
#include "../src/road/RoadGeometry.hpp"
#include "../src/render/RoadRenderer.hpp"
#include "../src/render/MountainRoadGeometry.hpp"
#include "../src/render/TreeGeometry.hpp"
#include "../src/render/CityLighting.hpp"
#include "../src/gen/StreetProfile.hpp"
#include "../src/ui/Camera.hpp"

namespace
{
	void installMountain(World& world, float height)
	{
		world.reserveChunks(); world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		for (int z=31;z<=33;++z) for (int x=31;x<=33;++x)
		{
			world.installChunkDirect({x,z},HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,height),height,height});
		}
	}
	bool hitsPortal(const MeshData& mesh,Vec3 from,Vec3 direction,double maximumDistance=4)
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

void registerMountainRoadTests(TestRunner& runner)
{
	runner.add(U"MountainRoad.CablePassingCameraGpu",[](TestContext& context)
	{
		const FilePath directory=FileSystem::CurrentDirectory();
		const FilePath shaderPath=directory+U"../../App/shaders/hlsl/city_cable.hlsl";
		const VertexShader vertexShader=ShaderAsset::vertex(shaderPath,U"Cable_VS");
		const PixelShader pixelShader=ShaderAsset::pixel(shaderPath,U"Cable_PS");
		context.expect(vertexShader && pixelShader,U"Production cable shaders load");
		const Size size{800,600};
		struct CableView { Float4 viewport; };
		ConstantBuffer<CableView> view;
		view->viewport=Float4{800,600,1.0f/800,1.0f/600};
		const Vec3 origin{33280,70,33280};
		const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm,HasDepth::Yes};
		TextWriter log{directory+U"TestResults/cable_passing_camera.csv"};
		log.writeln(U"frame,lateral,height,depth,reversed,visiblePixels,impossiblePixels");
		int invalidFrames=0,maximumInvalid=0,visibleFrames=0;
		// Both endpoint orders, slopes and offsets across the eye plane; every
		// physical segment lies strictly above and left of the camera.
		for (int frame=0;frame<480;++frame)
		{
			const Vec3 eye=origin+Vec3{0,1.2,0};
			const BasicCamera3D camera{size,80_deg,eye,eye+Vec3{0,0,10}};
			MeshData data;
			const double lateral=-.5-(frame%4)*2.0;
			const double height=2.0+((frame/4)%4)*2.0;
			const double depth=-2.0+((frame/16)%15)*.2;
			Vec3 from=origin+Vec3{lateral,height,depth};
			Vec3 to=origin+Vec3{-2,5,9};
			if (frame>=240) { std::swap(from,to); }
			const Float3 direction{(to-from).normalized()};
			for (const Vec3 endpoint : {from,to})
			{
				for (const float side : {-1.0f,1.0f})
				{
					data.vertices << Vertex3D{Float3{endpoint},direction,Float2{side,.014f}};
				}
			}
			data.indices << TriangleIndex32{0,1,2} << TriangleIndex32{2,1,3};
			const Mesh cable{data};
			{
				const ScopedRenderTarget3D rt{target.clear(Palette::White)};
				const ScopedRenderStates3D state{BlendState::Default2D,RasterizerState::SolidCullNone,DepthStencilState::DepthTest};
				Graphics3D::SetCameraTransform(camera);Graphics3D::SetVSConstantBuffer(4,view);
				const ScopedCustomShader3D shader{vertexShader,pixelShader};
				cable.draw(ColorF{1});
			}
			Graphics3D::Flush();Image capture;target.readAsImage(capture);
			int visible=0,invalid=0;
			for (int y=0;y<size.y;++y) for (int x=0;x<size.x;++x)
			{
				if (capture[{x,y}].r<250)
				{
					++visible;
					if (x>=size.x/2+4 || y>=size.y/2+4) { ++invalid; }
				}
			}
			visibleFrames+=visible>0;invalidFrames+=invalid>0;
			if (invalid>maximumInvalid || frame==0)
			{
				maximumInvalid=Max(maximumInvalid,invalid);
				capture.save(directory+U"Screenshot/cable_passing_camera_worst.png");
			}
			log.writeln(U"{},{},{},{},{},{},{}"_fmt(frame,lateral,height,depth,frame>=240,visible,invalid));
		}
		context.expect(visibleFrames>30,U"Forward cables remain visible during the drive");
		context.expectEqual(invalidFrames,0,U"Overhead left-side cables cannot produce foreground triangles below or right of the driver");
	});
	runner.add(U"MountainRoad.DesignGradePavementVisibility",[](TestContext& context)
	{
		World world;installMountain(world,72);
		RoadNetwork roads;
		const Vec3 start{33180,70,33280},end{33380,70,33280};
		const int a=roads.addNode(start),b=roads.addNode(end);
		const int id=*roads.addEdge(a,b,start.lerp(end,1.0/3),start.lerp(end,2.0/3),RoadType::Arterial,2);
		auto& edge=*roads.getEdge(id);edge.edgeState=EdgeState::Open;edge.designGrade=true;edge.useElevation=false;edge.edgeState=EdgeState::Open;
		const Vec3 focus=(start+end)*.5;
		world.update(focus);
		WorldRenderer terrain;terrain.setAsyncTerrain(false);
		const Mesh pavement{RoadGeometry::roadbedSurface(edge,*roads.getBezier(id),world)};
		const Size size{800,300};const BasicCamera3D camera{size,40_deg,focus+Vec3{0,190,-.01},focus};
		const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};
		Array<Image> captures;
		for (int pass=0;pass<2;++pass)
		{
			{
				const ScopedRenderTarget3D rt{target.clear(ColorF{.02,.08,.2})};
				const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
				Graphics3D::SetCameraTransform(camera);Graphics3D::SetGlobalAmbientColor(ColorF{.7});
				if (pass) { terrain.render(world,roads,camera); }
				pavement.draw(ColorF{.9,.1,.6});
			}
			Graphics3D::Flush();Image image;target.readAsImage(image);
			image.save(U"Screenshot/mountain_road_ground_{}.png"_fmt(pass));captures<<std::move(image);
		}
		int visible=0,checked=0;
		for (int x=-80;x<=80;x+=2) for (const double z : {-2.0,0.0,2.0})
		{
			const auto screen=camera.worldToScreenPoint(Float3{focus+Vec3{static_cast<double>(x),kRoadSurfaceLift,z}});
			const Point pixel{static_cast<int>(Round(screen.x)),static_cast<int>(Round(screen.y))};
			const Color first=captures[0][pixel],second=captures[1][pixel];
			++checked;visible+=Abs(static_cast<int>(first.r)-second.r)+Abs(static_cast<int>(first.g)-second.g)+Abs(static_cast<int>(first.b)-second.b)<8;
		}
		TextWriter{U"TestResults/mountain_ground.txt"}.write(U"visible={}/{}"_fmt(visible,checked));
		context.expect(visible>=checked*.99,U"Terrain must not obscure a ground road using its designed vertical alignment");
	});
	runner.add(U"MountainRoad.CurvedExcavationContinuity",[](TestContext& context)
	{
		World world;installMountain(world,74);
		RoadNetwork roads;TrainNetwork railway;
		const Vec3 a{33180,70,33280},b{33280,72,33380};
		const int start=roads.addNode(a),end=roads.addNode(b);
		const int id=*roads.addEdge(start,end,a+Vec3{55,0,0},b-Vec3{0,0,55},RoadType::Arterial,2);
		roads.getEdge(id)->useElevation=true;roads.getEdge(id)->tunnel=true;
		TunnelRenderer tunnel;tunnel.build(world,roads,railway);
		const auto curve=roads.getBezier(id);int gaps=0,checked=0;
		for (float arc=5;arc<curve->totalLength-5;arc+=.05f)
		{
			for (const double offset : {-3.0,0.0,3.0})
			{
				const Vec3 point=curve->positionAt(arc)+tangentToRight(curve->tangentAt(arc))*offset+Vec3{0,.1,0};
				bool inside=false;
				for (const auto& opening : tunnel.openings)
				{
					if (!opening.bounds.contains(Vec2{point.x,point.z})) { continue; }
					bool contained=true;
					for (const auto& plane : opening.planes) { contained&=plane.normal.dot(point)-plane.offset<.00001; }
					inside|=contained;
				}
				++checked;gaps+=!inside;
			}
		}
		TextWriter{U"TestResults/mountain_excavation.txt"}.write(U"uncutSamples={}/{} openings={}"_fmt(gaps,checked,tunnel.openings.size()));
		context.expectEqual(gaps,0,U"The complete curved carriageway must lie inside continuous excavation volumes");
	});
	runner.add(U"MountainRoad.TranslatedPortalClearance",[](TestContext& context)
	{
		int blocked=0,checked=0;
		for (const double coordinate : {0.0,33280.0,64000.0})
		{
			const Vec3 center{coordinate,70,coordinate},direction=Vec3{.6,.04,.8}.normalized(),right=tangentToRight(direction);
			const Vec3 along=Vec3{direction.x,0,direction.z}.normalized();
			const auto mesh=TunnelGeometry::portal(center,direction,4.2,5.8);
			for (double y=.15;y<5.6;y+=.2) for (double x=-4;x<=4;x+=.2)
			{
				if (y>2.2 && Square(x/4.2)+Square((y-2.2)/3.6)>.93) { continue; }
				++checked;blocked+=hitsPortal(mesh,center+right*x+Vec3{0,y,0}-along*2,along);
			}
		}
		TextWriter{U"TestResults/mountain_portal.txt"}.write(U"blockedPassageRays={}/{}"_fmt(blocked,checked));
		context.expectEqual(blocked,0,U"No residual portal faces may span the passage, including at large world coordinates");
	});

	runner.add(U"MountainRoad.CurvedSurfaceGpu",[](TestContext& context)
	{
		const FilePath directory = FileSystem::CurrentDirectory();
		struct Restore { FilePath path; ~Restore() { FileSystem::ChangeCurrentDirectory(path); } } restore{directory};
		// 単独実行でも共有樹木シェーダーと地形材質を本体のアセットから解決する。
		FileSystem::ChangeCurrentDirectory(directory + U"../../App/"); RegisterAssets();
		for (const bool tunnel : {false,true})
		{
			World world;installMountain(world,74);RoadNetwork roads;TrainNetwork railway;
			const Vec3 a{33180,70,33280},b{33280,72,33380};
			const int start=roads.addNode(a),end=roads.addNode(b);
			const int id=*roads.addEdge(start,end,a+Vec3{55,0,0},b-Vec3{0,0,55},RoadType::Arterial,2);
			auto& edge=*roads.getEdge(id);edge.edgeState=EdgeState::Open;edge.designGrade=true;edge.useElevation=tunnel;edge.tunnel=tunnel;
			const auto curve=roads.getBezier(id);const Vec3 focus=curve->positionAt(curve->totalLength*.5f);
			world.update(focus);WorldRenderer terrain;terrain.setAsyncTerrain(false);
			if (tunnel) { terrain.setTunnelOpenings(TunnelGeometry::build(*curve,world,edge.totalWidth(),false).openings); }
			const Mesh pavement{RoadGeometry::roadbedSurface(edge,*curve,world)};
			const Size size{1000,800};const BasicCamera3D camera{size,50_deg,focus+Vec3{0,170,-.01},focus};
			const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};Array<Image> images;
			for (int pass=0;pass<2;++pass)
			{
				{
					const ScopedRenderTarget3D rt{target.clear(ColorF{.02,.08,.2})};
					const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
					Graphics3D::SetCameraTransform(camera);Graphics3D::SetGlobalAmbientColor(ColorF{.7});
					if (pass) { terrain.render(world,roads,camera); }
					pavement.draw(ColorF{.9,.1,.6});
				}
				Graphics3D::Flush();Image capture;target.readAsImage(capture);
				capture.save(directory + U"Screenshot/mountain_curve_{}_{}.png"_fmt(tunnel,pass));images<<std::move(capture);
			}
			int matched=0,checked=0;
			for (float arc=6;arc<curve->totalLength-6;arc+=.5f) for (const double offset : {-2.9,0.0,2.9})
			{
				const Vec3 point=curve->positionAt(arc)+tangentToRight(curve->tangentAt(arc))*offset+Vec3{0,kRoadSurfaceLift,0};
				const auto screen=camera.worldToScreenPoint(Float3{point});
				const Point pixel{static_cast<int>(Round(screen.x)),static_cast<int>(Round(screen.y))};
				const auto first=images[0][pixel],second=images[1][pixel];
				++checked;matched+=Abs(static_cast<int>(first.r)-second.r)+Abs(static_cast<int>(first.g)-second.g)+Abs(static_cast<int>(first.b)-second.b)<8;
			}
			TextWriter{directory + U"TestResults/mountain_curve_{}.txt"_fmt(tunnel)}.write(U"visible={}/{}"_fmt(matched,checked));
			context.expect(matched>=checked*.99,U"Curved graded pavement remains visible, including both lane edges and the excavated approach");
		}
	});
	runner.add(U"MountainRoad.ContinuousLiningAndVerge",[](TestContext& context)
	{
		World world;installMountain(world,84);
		CubicBezier curve{{33180,70,33280},{33235,70,33280},{33280,72,33325},{33280,72,33380}};
		const auto geometry=TunnelGeometry::build(curve,world,7,false);
		int holes=0,checked=0;
		for (float arc=1;arc<curve.totalLength-1;arc+=.37f)
		{
			const Vec3 center=curve.positionAt(arc),right=tangentToRight(curve.tangentAt(arc));
			for (const int side : {-1,1})
			{
				for (const double y : {-.2,1.8,4.5})
				{
					++checked;holes+=!hitsPortal(geometry.lining,center+Vec3{0,y,0},right*side,6);
				}
				++checked;holes+=!hitsPortal(geometry.lining,center+right*(side*3.8)+Vec3{0,.3,0},{0,-1,0},.4);
			}
		}
		TextWriter{U"TestResults/mountain_lining.txt"}.write(U"holes={}/{}"_fmt(holes,checked));
		context.expectEqual(holes,0,U"Walls, curved vault joints, invert skirts and paved side verges form continuous surfaces");
	});
	runner.add(U"MountainRoad.IndoorFurnitureGpu",[](TestContext& context)
	{
		const FilePath directory=FileSystem::CurrentDirectory();
		struct Restore { FilePath path;~Restore(){FileSystem::ChangeCurrentDirectory(path);} } restore{directory};
		FileSystem::ChangeCurrentDirectory(directory+U"../../App/");
		for (const bool underground : {false,true})
		{
			World world;installMountain(world,underground ? 84.0f : 70.0f);Array<Image> images;
			for (const bool furnished : {false,true})
			{
				RoadNetwork roads;const Vec3 a{33280,70,33180},b{33280,70,33380};
				const int start=roads.addNode(a),end=roads.addNode(b);
				const int id=*roads.addEdge(start,end,a.lerp(b,1.0/3),a.lerp(b,2.0/3),RoadType::Arterial,2);
				auto& edge=*roads.getEdge(id);edge.edgeState=EdgeState::Open;GeneratedStreet::apply(edge,GeneratedStreet::describe(GeneratedStreet::Role::Mountain));
				edge.useElevation=underground;edge.tunnel=underground;edge.designGrade=true;
				if (!furnished) { edge.parts.remove_if([](const RoadPart& part){return part.placement==RoadPartPlacement::RepeatAlongEdge;}); }
				else
				{
					RoadSignPlacement sign;sign.type=RoadSignType::SpeedLimit;sign.nodeEndId=start;sign.arcOffset=24;sign.lateralOffset=-3.8f;sign.auxValue=40;edge.signs<<sign;
					roads.addRoute(RoadRouteKind::NationalRoute,U"山間道路",{id},123);
					GuideSignPlacement guide;guide.parentEdgeId=id;guide.nodeEndId=start;guide.arcOffset=38;guide.lateralOffset=4.2f;roads.addGuideSign(guide);
				}
				RoadRenderer renderer;context.expect(renderer.loadAssets(),U"Street furniture assets load");
				if (furnished) { renderer.setMunicipalityLookup([](Vec2 point){return point.y<33235 ? U"山里村" : U"杉の町";}); }
				const Size size{800,500};const BasicCamera3D camera{size,60_deg,a+Vec3{.5,2.2,-3},a+Vec3{0,2.2,60}};
				const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};
				{
					const ScopedRenderTarget3D rt{target.clear(ColorF{.12,.22,.31})};
					const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
					Graphics3D::SetCameraTransform(camera);Graphics3D::SetGlobalAmbientColor(ColorF{.7});
					renderer.render(roads,world,ViewFrustum{camera,24000},camera.getEyePosition());
				}
				Graphics3D::Flush();Image capture;target.readAsImage(capture);
				capture.save(directory+U"Screenshot/mountain_furniture_{}_{}.png"_fmt(underground,furnished));images<<std::move(capture);
			}
			int changed=0;
			for (size_t i=0;i<images[0].num_pixels();++i) { changed+=images[0].data()[i]!=images[1].data()[i]; }
			TextWriter{directory+U"TestResults/mountain_furniture_{}.txt"_fmt(underground)}.write(U"furniturePixels={}"_fmt(changed));
			context.expect(underground ? changed==0 : changed>100,U"Outdoor signs, guide signs, route/country signs, poles and wires disappear only inside the tunnel");
		}
	});
	runner.add(U"MountainRoad.ForestAndHillside",[](TestContext& context)
	{
		World world;world.reserveChunks();world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		Grid<float> heights(HEIGHT_CELLS+1,HEIGHT_CELLS+1);
		for (int z=0;z<=HEIGHT_CELLS;++z) for (int x=0;x<=HEIGHT_CELLS;++x) { heights[{x,z}]=static_cast<float>(70+Clamp((x*16-512)*.6,-35.0,70.0)); }
		world.installChunkDirect({32,32},HeightMapResult{heights,35,140});
		RoadNetwork roads;const Vec3 a{33280,70,33180},b{33280,70,33580};
		const int start=roads.addNode(a),end=roads.addNode(b);
		const int id=*roads.addEdge(start,end,a.lerp(b,1.0/3),a.lerp(b,2.0/3),RoadType::Arterial,2);
		auto& edge=*roads.getEdge(id);edge.edgeState=EdgeState::Open;GeneratedStreet::apply(edge,GeneratedStreet::describe(GeneratedStreet::Role::Mountain));edge.designGrade=true;
		const auto geometry=MountainRoadGeometry::build(edge,*roads.getBezier(id),world,4,396);
		context.expect(!geometry.wall.indices.isEmpty() && !geometry.rail.indices.isEmpty() && !geometry.moss.indices.isEmpty() && !geometry.reflectors.indices.isEmpty(),U"Mountain cross section includes a vegetated retaining wall and a valley barrier with reflectors");
		for (const auto& vertex : geometry.wall.vertices) { context.expect(vertex.pos.x>33283,U"Retaining wall stays on the uphill side outside the carriageway"); }
		for (const auto& vertex : geometry.rail.vertices) { context.expect(vertex.pos.x<33277,U"Guardrail follows the valley side, leaving both lanes clear"); }
		context.expect(edge.lanes[0].lineRight==LineType::SolidYellow && edge.lanes[0].lineLeft==LineType::SolidWhite,U"Mountain road uses a solid yellow centre and white edge lines");
		for (const bool cedar : {false,true})
		{
			const auto tree=TreeGeometry::build(17,cedar,true);const double width=cedar ? 10 : 15,height=cedar ? 25 : 19;
			double lowest=1e9,trunkRadius=0;
			for (const auto& vertex : tree.leaves.vertices) { lowest=Min(lowest,vertex.pos.y*height); }
			for (const auto& vertex : tree.wood.vertices) { if (vertex.pos.y<.1) { trunkRadius=Max(trunkRadius,Vec2{vertex.pos.x,vertex.pos.z}.length()*width); } }
			context.expect(lowest>5.5 && trunkRadius<.40,U"Forest trees have tall clear trunks and lifted crowns");
		}
		const FilePath directory=FileSystem::CurrentDirectory();
		struct Restore { FilePath path;~Restore(){FileSystem::ChangeCurrentDirectory(path);} } restore{directory};
		FileSystem::ChangeCurrentDirectory(directory+U"../../App/");RegisterAssets();
		WorldRenderer terrain;terrain.setAsyncTerrain(false);RoadRenderer renderer;context.expect(renderer.loadAssets(),U"Mountain scene assets load");
		CityLighting lighting;context.expect(lighting.initialize(),U"Landscape lighting shader loads");
		terrain.setBuildingShader(lighting.buildingShader());terrain.setTerrainShader(lighting.terrainShader());
		terrain.setLandscapeShaders(lighting.fieldShader(),lighting.paddyShader(),lighting.foliageShader());
		const Size size{1000,720};const BasicCamera3D camera{size,62_deg,a+Vec3{-.8,1.6,9},a+Vec3{-.8,2.0,100}};
		world.update(camera.getEyePosition());
		const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};
		const Vec3 sun=Vec3{-1,2,-.4}.normalized();
		lighting.update(camera,a+Vec3{0,0,60},sun,1,1,[](Vec3,double){});
		{
			const ScopedRenderTarget3D rt{target.clear(ColorF{.70,.76,.79})};
			const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
			Graphics3D::SetCameraTransform(camera);Graphics3D::SetSunDirection(sun);Graphics3D::SetGlobalAmbientColor(ColorF{.4});lighting.bind();
			const ScopedCustomShader3D shader{lighting.shader()};terrain.render(world,roads,camera);
			renderer.render(roads,world,ViewFrustum{camera,24000},camera.getEyePosition());
		}
		Graphics3D::Flush();Image capture;target.readAsImage(capture);capture.save(directory+U"Screenshot/mountain_forest_reference.png");
		int green=0,dark=0,yellow=0;
		for (const auto pixel : capture) { green+=pixel.g>pixel.r*1.08 && pixel.g>pixel.b*1.1;dark+=pixel.r<100 && pixel.g<100 && pixel.b<100;yellow+=pixel.r>100 && pixel.r>pixel.g*1.1 && pixel.g>pixel.b*1.6; }
		TextWriter{directory+U"TestResults/mountain_forest.txt"}.write(U"greenPixels={} darkPixels={} yellowPixels={} wallTriangles={} railTriangles={}"_fmt(green,dark,yellow,geometry.wall.indices.size(),geometry.rail.indices.size()));
		context.expect(green>10000 && dark>10000 && yellow>100,U"First-person mountain scene includes visible layered vegetation and pavement");
		// Sweep the actual driver camera past several poles with terrain, trees,
		// road furniture and shadow passes all enabled, including a sideways view.
		const Size originalSize=Scene::Size();
		struct RestoreSize { Size size;~RestoreSize(){Scene::Resize(size);} } restoreSize{originalSize};
		Scene::Resize(size);
		GameCamera driverCamera;
		TextWriter driveLog{directory+U"TestResults/mountain_drive.csv"};
		driveLog.writeln(U"frame,z,veryDarkRoadPixels,repeatDifferencePixels");
		int repeatDifferences=0;
		for (int frame=0;frame<32;++frame)
		{
			const Vec3 position=a+Vec3{-1.5,kRoadSurfaceLift,22.0+frame*2.0};
			driverCamera.setDrivingState(position,0,0);
			driverCamera.setDrivingLook({20_deg,0});
			const auto& view=driverCamera.camera3D();
			Image first;
			int darkRoad=0,different=0;
			for (int pass=0;pass<2;++pass)
			{
				world.update(view.getEyePosition());
				lighting.update(view,position,sun,1,terrain.geometryRevision()+renderer.geometryRevision(),[&](Vec3 focus,double radius)
				{
					terrain.renderShadowCasters(focus,radius);renderer.renderShadowCasters(focus,radius);
				});
				{
					const ScopedRenderTarget3D rt{target.clear(ColorF{.70,.76,.79})};
					const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
					Graphics3D::SetCameraTransform(view);Graphics3D::SetSunDirection(sun);Graphics3D::SetGlobalAmbientColor(ColorF{.4});
					lighting.bind();
					const ScopedRenderStates3D sampler{ScopedRenderStates3D::SamplerStateInfo{ShaderStage::Pixel,1,SamplerState::ClampNearest}};
					const ScopedCustomShader3D shader{lighting.shader()};
					terrain.render(world,roads,view);renderer.render(roads,world,ViewFrustum{view,24000},view.getEyePosition());
				}
				Graphics3D::Flush();Image image;target.readAsImage(image);
				if (pass==0)
				{
					image.save(directory+U"Screenshot/mountain_drive_{:02}.png"_fmt(frame));
					first=std::move(image);
				}
				else
				{
					for (int row=0;row<size.y;++row) for (int col=0;col<size.x;++col)
					{
						different+=image[{col,row}]!=first[{col,row}];
						const Color pixel=image[{col,row}];
						darkRoad+=row>size.y/2 && col>size.x/4 && col<size.x*3/4 && pixel.r<25 && pixel.g<25 && pixel.b<25;
					}
				}
			}
			repeatDifferences+=different;
			driveLog.writeln(U"{},{},{},{}"_fmt(frame,position.z,darkRoad,different));
		}
		context.expectEqual(repeatDifferences,0,U"The complete mountain scene remains identical when a driving pose is repeated");
	});
}
