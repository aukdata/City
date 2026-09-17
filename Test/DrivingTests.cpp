#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "../src/traffic/VehicleManager.hpp"
#include "../src/traffic/VehiclePose.hpp"
#include "../src/ui/Camera.hpp"
#include "../src/ui/DrivingControls.hpp"
#include "../src/render/TunnelRenderer.hpp"
#include "../src/render/RoadRenderer.hpp"
#include "../src/render/VehicleRenderer.hpp"
#include "../src/render/WorldRenderer.hpp"
#include "../src/traffic/DrivingController.hpp"
#include "../src/road/RoadGeometry.hpp"
#include "../src/ui/DrivingHud.hpp"
#include "../src/gen/StreetProfile.hpp"
#include "../src/asset/AssetRegistrar.hpp"

namespace
{
	void makeWorld(World& world,float height=70)
	{
		world.reserveChunks();world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		world.installChunkDirect({32,32},HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,height),height,height});
	}
	int road(RoadNetwork& roads,Vec3 a,Vec3 b,int lanes=2)
	{
		const int first=roads.addNode(a),last=roads.addNode(b);
		const int id=*roads.addEdge(first,last,a.lerp(b,1.0/3),a.lerp(b,2.0/3),RoadType::Arterial,lanes);
		auto& edge=*roads.getEdge(id);edge.edgeState=EdgeState::Open;edge.designGrade=true;
		return id;
	}
	void advance(DrivingController& driver,double seconds,DrivingInput input,const World& world,const RoadNetwork& roads,double frame=1.0/60)
	{
		for (int index=0;index<static_cast<int>(Round(seconds/frame));++index) { driver.update(frame,input,world,roads); }
	}
}

void registerDrivingTests(TestRunner& runner)
{
	runner.add(U"Driving.ForgivingContactAndReverseRecovery",[](TestContext& context)
	{
		World world;makeWorld(world);RoadNetwork roads;road(roads,{33280,70,32900},{33280,70,33700});
		DrivingController driver;context.expect(driver.enter({33278,70,33200},world,roads),U"Driver enters the contact fixture");
		const Vehicle start=driver.vehicle();const Vec3 along{Sin(start.heading),0,Cos(start.heading)};
		Vehicle other=start;other.id=7;other.position+=Vec3{Cos(start.heading),0,-Sin(start.heading)}*2;
		const bool mirrorBlocks=DrivingController::overlaps(start,other);
		context.expect(!mirrorBlocks,U"Nearby mirrors alone cannot stop otherwise separated car bodies");
		other.position=start.position+along*4;
		for (int i=0;i<120;++i) { driver.update(1.0/60,{0,1,0,false},world,roads,{other}); }
		const double reverseDistance=(start.position-driver.vehicle().position).dot(along);
		context.expect(reverseDistance>1.0 && driver.reversing(),U"An existing shallow contact must allow backing away without resetting reverse selection");
		context.expect(!DrivingController::overlaps(driver.vehicle(),other),U"Backing out separates the two cars");
		TextWriter{U"TestResults/driving_contact.txt"}.write(U"mirrorBlocks={} reverseDistance={} speed={}"_fmt(mirrorBlocks,reverseDistance,driver.vehicle().speed));
	});
	runner.add(U"Driving.WheelSupportAtRoadEdge",[](TestContext& context)
	{
		World world;makeWorld(world);RoadNetwork roads;const int id=road(roads,{33280,70,32900},{33280,70,33700});
		const auto* edge=roads.getEdge(id);const auto curve=roads.getBezier(id);
		const auto range=RoadGeometry::roadbedRangeAt(*edge,.5f);
		DrivingController driver;context.expect(driver.enter({33278,70,33200},world,roads),U"Driver enters the road-edge fixture");
		// An overhanging bumper/body corner is not a tyre losing the road.
		auto& initial=const_cast<Vehicle&>(driver.vehicle());
		initial.position.x=33280+range.left+.78;initial.heading=0;
		const Vec3 start=initial.position;
		advance(driver,1,{1,0,0,false},world,roads);
		TextWriter{U"TestResults/driving_road_edge.txt"}.write(U"distance={} blocked={} lateral={}"_fmt(driver.distance(),driver.blocked(),initial.position.x-33280));
		context.expect(driver.vehicle().position.distanceFrom(start)>1,U"Tyres on the road can drive along its edge despite slight body overhang");
		advance(driver,40,{1,0,0,false},world,roads);
		context.expect(driver.vehicle().position.z<33700 && driver.blocked(),U"Road ends and drops still stop the vehicle");
	});
	runner.add(U"Driving.EntryAndRoadAvailability",[](TestContext& context)
	{
		World world;makeWorld(world);RoadNetwork roads;DrivingController driver;
		context.expect(!driver.enter({33280,70,33280},world,roads),U"No road gives a recoverable entry failure");
		const int id=road(roads,{33280,70,32900},{33280,70,33700});
		roads.getEdge(id)->edgeState=EdgeState::Planned;
		context.expect(!driver.enter({33280,70,33280},world,roads),U"Unbuilt roads cannot be driven");
		roads.getEdge(id)->edgeState=EdgeState::Open;
		context.expect(driver.enter({33278,70,33200},world,roads),U"A clear open lane is a valid starting position");
		context.expect(Abs(driver.vehicle().position.x-33280)>1,U"Entry starts in a lane instead of straddling the centre line");
		driver.leave();context.expect(!driver.active(),U"Leaving releases the driving state");
	});
	runner.add(U"Driving.AccelerationBrakeReverseAndSteering",[](TestContext& context)
	{
		World world;makeWorld(world);RoadNetwork roads;road(roads,{33280,70,32900},{33280,70,33700},4);
		DrivingController driver;context.expect(driver.enter({33278,70,33200},world,roads),U"Driver enters");
		const Vec3 start=driver.vehicle().position;
		advance(driver,3,{1,0,0,false},world,roads);const double speed=driver.vehicle().speed;
		context.expect(speed>7 && driver.vehicle().position.distanceFrom(start)>10,U"Throttle moves the car and builds speed gradually");
		advance(driver,2,{0,1,0,false},world,roads);
		context.expect(driver.vehicle().speed<=0,U"Braking stops forward travel before selecting reverse");
		advance(driver,1,{0,1,0,false},world,roads);
		context.expect(driver.reversing(),U"Holding the brake at rest engages reverse");
		driver.stopMotion();const auto before=driver.vehicle();
		advance(driver,1.4,{1,0,.5,false},world,roads);
		context.expect(Abs(driver.vehicle().heading-before.heading)>.05,U"Steering changes heading independently of the road centreline");
		context.expect(driver.vehicle().position.distanceFrom(before.position)>1,U"Steering still advances the car");
		driver.update(.1,{1,0,1,false},world,roads,{},false);
		context.expectNear(driver.vehicle().speed,0,.001,U"Blocked input safely holds the car");
	});
	runner.add(U"Driving.FrameRateRoadEdgeAndTraffic",[](TestContext& context)
	{
		World world;makeWorld(world);RoadNetwork roads;road(roads,{33280,70,33100},{33280,70,33500});
		DrivingController slow,fast;slow.enter({33278,70,33200},world,roads);fast.enter({33278,70,33200},world,roads);
		advance(slow,3,{1,0,0,false},world,roads,1.0/30);advance(fast,3,{1,0,0,false},world,roads,1.0/120);
		context.expectNear(slow.vehicle().position.distanceFrom(fast.vehicle().position),0,.02,U"Driving speed is independent of render frame rate");
		Vehicle obstacle=slow.vehicle();const Vec3 along{Sin(obstacle.heading),0,Cos(obstacle.heading)};obstacle.position+=along*12;
		for (int i=0;i<240;++i) { slow.update(1.0/60,{1,0,0,false},world,roads,{obstacle}); }
		context.expect(!DrivingController::overlaps(slow.vehicle(),obstacle),U"The car cannot pass through stopped traffic");
		context.expectNear(slow.vehicle().speed,0,.001,U"Contact stops the car");
		advance(fast,30,{1,0,0,false},world,roads);
		context.expectNear(fast.vehicle().speed,0,.001,U"The road end stops the wheel footprint");
		context.expect(InRange(fast.vehicle().position.z,33101.0,33499.0),U"The car remains on the road after prolonged throttle");
	});
	runner.add(U"Driving.TunnelAndBridgeGrade",[](TestContext& context)
	{
		World world;makeWorld(world,100);RoadNetwork roads;
		const int lower=road(roads,{33280,70,32900},{33280,86,33700});
		roads.getEdge(lower)->useElevation=true;roads.getEdge(lower)->tunnel=true;
		road(roads,{33280,100,32900},{33280,100,33700});
		DrivingController driver;context.expect(driver.enter({33278,76,33200},world,roads),U"Driver can enter the buried road");
		advance(driver,4,{1,0,0,false},world,roads);
		context.expect(driver.vehicle().currentEdge==lower && driver.vehicle().position.y<90,U"Road contact does not jump to terrain or an overlapping upper road");
		context.expectNear(Abs(driver.vehicle().pitch),Atan(.02),.002,U"Vehicle pitch follows the designed grade");
		context.expect(driver.distance()>20,U"The underground vehicle moves freely along its road");
	});
	runner.add(U"Driving.CameraAndInputOwnership",[](TestContext& context)
	{
		World world;makeWorld(world,100);GameCamera camera;
		camera.setDrivingState({33280,70,33280},0,.04f);
		context.expectNear(camera.eyePosition().x,33280.35,.001,U"Driver sits on the right-hand side");
		context.expectNear(camera.eyePosition().y,71.2,.001,U"Driver eye height remains inside the tunnel");
		const auto forward=(camera.camera3D().getFocusPosition()-camera.eyePosition()).normalized();
		context.expectNear(forward.y,Sin(.04),.001,U"Forward view follows the road grade");
		GameInput::buffer=KeyboardActionBuffer{};
		GameInput::buffer.update({{0,1,KeyW.code(),true,false},{0,2,KeyD.code(),true,false},{0,3,KeySpace.code(),true,false}},true);
		const auto input=DrivingControls::read(true);
		context.expect(input.throttle==1 && input.steering==1 && input.handbrake,U"Buffered short taps reach the driving controls");
		const Vec3 before=camera.focusPoint();camera.update(.1,world);
		context.expectNear(camera.focusPoint().distanceFrom(before),0,.001,U"WASD cannot also move the driving camera");
		TextEditState field;GameInput::textInput=&field;
		const auto textInput=DrivingControls::read(true);
		context.expect(textInput.throttle==0 && textInput.steering==0 && !textInput.handbrake,U"Text entry consumes driving keys");
		GameInput::releaseTextFocus();
		context.expect(DrivingControls::read(true).throttle==0,U"The frame that releases text focus stays blocked");
		GameInput::textOwnedFrame=false;GameInput::textInput=nullptr;
		context.expect(DrivingControls::read(false).throttle==0,U"Maps, menus and unfocused windows can hold all driving input");
		GameInput::buffer=KeyboardActionBuffer{};
		camera.setDrivingLook({.5,.1});camera.setDrivingState({33280,70,33285},0,.04f);
		context.expect(camera.camera3D().getFocusPosition().x>camera.eyePosition().x+1,U"Vehicle updates preserve an active look to the right");
		camera.setWalkingState({33280,70,33285},0);context.expect(camera.mode()==CameraMode::FirstPerson,U"Leaving can return to walking immediately");
		camera.cycleMode();context.expect(camera.mode()==CameraMode::Overview,U"Overview remains available after leaving");
	});
	runner.add(U"Driving.FreeJunctionTurn",[](TestContext& context)
	{
		World world;makeWorld(world);RoadNetwork roads;const Vec3 center{33280,70,33280};
		const int junction=roads.addNode(center);Array<int> endpoints;
		for (const Vec3 delta : {Vec3{0,0,-160},Vec3{0,0,160},Vec3{-160,0,0},Vec3{160,0,0}})
		{
			const Vec3 point=center+delta;const int end=roads.addNode(point);endpoints<<end;
			const bool incoming=delta.z<0;const Vec3 a=incoming ? point : center,b=incoming ? center : point;
			const int id=*roads.addEdge(incoming ? end : junction,incoming ? junction : end,a.lerp(b,1.0/3),a.lerp(b,2.0/3),RoadType::Arterial,4);
			roads.getEdge(id)->edgeState=EdgeState::Open;roads.getEdge(id)->designGrade=true;
		}
		roads.updateNodeCutoffs(junction);roads.rebuildLaneConnections(junction);
		DrivingController driver;context.expect(driver.enter(center+Vec3{-1.6,0,-36},world,roads),U"Enter the south approach");
		Array<Vec3> path;
		for (int z=-50;z<=-8;++z) { path<<center+Vec3{-1.6,0,static_cast<double>(z)}; }
		for (int i=1;i<=32;++i) { const double angle=i*Math::HalfPi/32;path<<center+Vec3{6.4-8*Cos(angle),0,-8+8*Sin(angle)}; }
		for (int x=7;x<=65;++x) { path<<center+Vec3{static_cast<double>(x),0,0}; }
		size_t nearest=0;TextWriter trace{U"TestResults/driving_turn.csv"};trace<<U"frame,x,y,z,speed,steer,heading,blocked";
		for (int frame=0;frame<2100 && driver.vehicle().position.x<center.x+42;++frame)
		{
			const auto& vehicle=driver.vehicle();
			for (size_t i=nearest;i<Min(path.size(),nearest+12);++i) { if (vehicle.position.distanceFromSq(path[i])<vehicle.position.distanceFromSq(path[nearest])) { nearest=i; } }
			size_t target=nearest;
			while (target+1<path.size() && vehicle.position.distanceFrom(path[target])<4) { ++target; }
			const Vec3 relative=path[target]-vehicle.position,right{Cos(vehicle.heading),0,-Sin(vehicle.heading)};
			const double requested=Atan(2*DrivingController::kWheelbase*relative.dot(right)/Max(1.0,relative.lengthSq()));
			const double limit=Min(.55,Atan(DrivingController::kWheelbase*5.5/Max(25.0,Square(static_cast<double>(vehicle.speed)))));
			driver.update(1.0/60,{vehicle.speed<3.5 ? .6 : 0,0,Clamp(requested/limit,-1.0,1.0),false},world,roads);
			if (frame%10==0) { trace<<U"{},{},{},{},{},{},{},{}"_fmt(frame,vehicle.position.x,vehicle.position.y,vehicle.position.z,vehicle.speed,driver.steering(),vehicle.heading,driver.blocked()); }
		}
		context.expect(driver.vehicle().position.x>center.x+40,U"Steering drives through the shared junction onto the chosen right-hand branch");
		context.expect(Abs(driver.vehicle().heading-Math::HalfPi)<.15 && driver.distance()>70,U"The player car changes road without an AI route or lane lock");
	});
	runner.add(U"Driving.TrafficStopsAndResumes",[](TestContext& context)
	{
		World world;makeWorld(world);RoadNetwork roads;const int edge=road(roads,{33280,70,32900},{33280,70,33700});
		const SimGraph graph=SimGraph::build(roads);VehicleManager manager;manager.init(graph,roads);
		TrafficDemand demand;demand.targetVehicleCount=0;manager.setTrafficDemand(demand);
		manager.spawnOnEdge(edge,graph);
		context.expect(manager.vehicleCount()==1,U"Controlled traffic fixture spawns one car");if (manager.vehicleCount()!=1) { return; }
		// The public spawn chooses a random arc; fix only the test's initial condition.
		auto& initial=const_cast<Vehicle&>(manager.vehicles().front());initial.arcPos=300;initial.speed=12;
		const auto first=VehiclePose::resolve(initial,roads,&world);context.expect(first.has_value(),U"Traffic world pose resolves");if (!first) { return; }
		Vehicle player=*first;player.id=-2;player.speed=0;
		player.position+=Vec3{Sin(first->heading),0,Cos(first->heading)}*28;
		manager.setDrivenVehicle(player,&world);
		for (int frame=0;frame<600;++frame)
		{
			// Even behind the camera, nearby traffic must not become a dormant teleport.
			manager.update(1.0/60,frame/60.0,graph,roads,{});
			const auto actual=VehiclePose::resolve(manager.vehicles().front(),roads,&world);
			context.expect(actual && !DrivingController::overlaps(*actual,player),U"Following traffic never overlaps the stopped player");
		}
		context.expectNear(manager.vehicles().front().speed,0,.01,U"Traffic waits behind the driver");
		const float stoppedArc=manager.vehicles().front().arcPos;
		manager.setDrivenVehicle(none);
		for (int frame=0;frame<120;++frame) { manager.update(1.0/60,10+frame/60.0,graph,roads,{edge}); }
		context.expect(manager.vehicles().front().speed>1 && Abs(manager.vehicles().front().arcPos-stoppedArc)>1,U"Traffic resumes after the driver leaves");
	});
	runner.add(U"Driving.IntersectionTrafficAndHeightSeparation",[](TestContext& context)
	{
		World world;makeWorld(world);RoadNetwork roads;const Vec3 center{33280,70,33280};
		const int junction=roads.addNode(center);Array<int> ids;
		for (const Vec3 delta : {Vec3{0,0,-150},Vec3{0,0,150},Vec3{150,0,0}})
		{
			const int node=roads.addNode(center+delta);const bool incoming=delta.z<0;
			const Vec3 a=incoming ? center+delta : center,b=incoming ? center : center+delta;
			const int id=*roads.addEdge(incoming ? node : junction,incoming ? junction : node,a.lerp(b,1.0/3),a.lerp(b,2.0/3),RoadType::Arterial,4);
			roads.getEdge(id)->edgeState=EdgeState::Open;roads.getEdge(id)->designGrade=true;ids<<id;
		}
		roads.updateNodeCutoffs(junction);roads.rebuildLaneConnections(junction);
		const auto& connections=roads.getNode(junction)->laneConnections;
		const auto found=std::find_if(connections.begin(),connections.end(),[&](const auto& connection){return connection.fromEdgeId==ids[0] && connection.toEdgeId==ids[1];});
		context.expect(found!=connections.end(),U"Fixture includes an actual intersection connection");if (found==connections.end()) { return; }
		const auto& connection=*found;const SimGraph graph=SimGraph::build(roads);
		VehicleManager manager;manager.init(graph,roads);TrafficDemand demand;demand.targetVehicleCount=0;manager.setTrafficDemand(demand);manager.spawnOnEdge(ids[0],graph);
		auto& initial=const_cast<Vehicle&>(manager.vehicles().front());
		initial.location=VehicleLocation::OnConnection;initial.connectionNodeId=junction;initial.connectionId=connection.id;
		initial.currentLane=connection.fromLaneIndex;initial.arcPos=.5f;initial.speed=3;
		Vehicle stopped=initial;stopped.arcPos=Min(connection.path.totalLength-1,initial.arcPos+8);stopped.speed=0;
		const auto pose=VehiclePose::resolve(stopped,roads,&world);context.expect(pose.has_value(),U"Connection pose resolves");if (!pose) { return; }
		Vehicle player=*pose;player.id=-2;manager.setDrivenVehicle(player,&world);
		for (int frame=0;frame<240;++frame)
		{
			manager.update(1.0/60,frame/60.0,graph,roads,{ids[0],ids[1],ids[2]});
			const auto current=VehiclePose::resolve(manager.vehicles().front(),roads,&world);
			context.expect(current && !DrivingController::overlaps(*current,player),U"Intersection traffic stops without entering the player car");
		}
		context.expectNear(manager.vehicles().front().speed,0,.01,U"A vehicle can wait inside a connection");
		const float stoppedArc=manager.vehicles().front().arcPos;
		player.position.y+=10;manager.setDrivenVehicle(player,&world);
		for (int frame=0;frame<60;++frame) { manager.update(1.0/60,4+frame/60.0,graph,roads,{ids[0],ids[1],ids[2]}); }
		context.expect(manager.vehicles().front().speed>1 && manager.vehicles().front().arcPos>stoppedArc+.5,U"The stopped connection accelerates again and an upper-level car does not block it");
	});
	runner.add(U"Driving.VehicleModelContactEnvelope",[](TestContext& context)
	{
		const FilePath directory=FileSystem::CurrentDirectory();
		struct Restore { FilePath path;~Restore(){FileSystem::ChangeCurrentDirectory(path);} } restore{directory};
		FileSystem::ChangeCurrentDirectory(directory+U"../../App/");
		Vehicle player;player.position={0,0,0};
		for (const auto& entry : Array<std::pair<String,VehicleType>>{{U"sedan",VehicleType::PassengerCar},{U"kei_wagon",VehicleType::KeiCar},
			{U"city_bus",VehicleType::Bus},{U"delivery_truck",VehicleType::SmallTruck},{U"cargo_truck",VehicleType::LargeTruck},
			{U"fire_engine",VehicleType::Emergency},{U"patrol_car",VehicleType::Emergency}})
		{
			const Model model{U"assets/vehicles/{}.obj"_fmt(entry.first)};
			context.expect(!model.isEmpty(),U"Contact fixture model loads");if (model.isEmpty()) { continue; }
			Vehicle other;other.type=entry.second;other.id=entry.first==U"fire_engine" ? 1 : 2;
			const auto size=model.boundingBox().size;
			other.position={1.075+size.z*.5-.02,0,0};
			context.expect(!DrivingController::overlaps(player,other),U"Mirror-only proximity is forgiving: "+entry.first);
			other.position.x=.5;
			context.expect(DrivingController::overlaps(player,other),U"Real body overlap still blocks: "+entry.first);
			other.position={0,0,2.311+size.x*.5-.02};
			context.expect(DrivingController::overlaps(player,other),U"Bumpers are inside the longitudinal contact envelope: "+entry.first);
			other.position.y=10;context.expect(!DrivingController::overlaps(player,other),U"Separate bridge levels do not collide");
		}
	});
	runner.add(U"Driving.TunnelSceneGpu",[](TestContext& context)
	{
		const FilePath directory=FileSystem::CurrentDirectory();
		struct Restore { FilePath path;~Restore(){FileSystem::ChangeCurrentDirectory(path);} } restore{directory};
		FileSystem::ChangeCurrentDirectory(directory+U"../../App/");RegisterAssets();
		World world;makeWorld(world,90);RoadNetwork roads;TrainNetwork rails;
		const int id=road(roads,{33280,70,32900},{33280,78,33700});
		auto& edge=*roads.getEdge(id);GeneratedStreet::apply(edge,GeneratedStreet::describe(GeneratedStreet::Role::Mountain));
		edge.useElevation=true;edge.tunnel=true;edge.designGrade=true;
		DrivingController driver;context.expect(driver.enter({33278,73,33200},world,roads),U"Enter an actual rendered tunnel");
		RoadRenderer roadRenderer;context.expect(roadRenderer.loadAssets(),U"Road assets load");
		TunnelRenderer tunnel;tunnel.build(world,roads,rails);
		WorldRenderer terrain;terrain.setAsyncTerrain(false);terrain.setTunnelOpenings(tunnel.openings);
		VehicleRenderer vehicles;GameCamera camera;const Size size=Scene::Size();
		const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};Array<Image> images;
		for (int shot=0;shot<2;++shot)
		{
			if (shot) { advance(driver,3,{1,0,0,false},world,roads); }
			const auto car=driver.vehicle();camera.setDrivingState(car.position,car.heading,car.pitch);world.update(car.position);
			Vehicle ahead=car;ahead.id=24;ahead.position+=Vec3{Sin(car.heading),0,Cos(car.heading)}*18;
			{
				const ScopedRenderTarget3D rt{target.clear(ColorF{.14,.21,.27})};const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
				Graphics3D::SetCameraTransform(camera.camera3D());Graphics3D::SetGlobalAmbientColor(ColorF{.7});
				terrain.render(world,roads,camera.camera3D());roadRenderer.render(roads,world,ViewFrustum{camera.camera3D(),24000},camera.eyePosition());
				tunnel.draw(camera.eyePosition());vehicles.render({ahead},camera.eyePosition());
			}
			Graphics3D::Flush();
			{const ScopedRenderTarget2D rt{target};DrivingHud::draw(FontAsset(Asset::CJK14),size,{car.speed,driver.steering(),driver.distance(),40,false,false});}
			Graphics2D::Flush();Image image;target.readAsImage(image);image.save(directory+U"Screenshot/driving_tunnel_{}.png"_fmt(shot));images<<std::move(image);
			context.expect(camera.eyePosition().y<80 && driver.vehicle().position.y<79,U"Road and driver remain underground in the combined scene");
		}
		int changed=0,dark=0,bright=0;
		for (int y=80;y<size.y-170;++y) for (int x=50;x<size.x-50;++x)
		{
			changed+=images[0][y][x]!=images[1][y][x];dark+=images[1][y][x].r<100;bright+=images[1][y][x].r>180;
		}
		TextWriter{directory+U"TestResults/driving_scene.txt"}.write(U"movedPixels={} darkPixels={} brightPixels={} distance={}"_fmt(changed,dark,bright,driver.distance()));
		context.expect(changed>2000 && dark>5000 && bright>200,U"Moving cockpit shows a changing road scene with lit lining and nearby traffic");
	});
	runner.add(U"Driving.CockpitGpu",[](TestContext& context)
	{
		RegisterAssets();const Font font=FontAsset(Asset::CJK14);
		for (const Size size : {Size{800,600},Size{1280,768},Size{1920,1080}})
		{
			const auto panel=DrivingHud::instrumentBounds(size);
			context.expect(RectF{Vec2{0,0},size}.contains(panel.pos) && RectF{Vec2{0,0},size}.contains(panel.br()),U"Speed and gear fit on every supported viewport");
			Array<Image> images;
			for (int variant=0;variant<2;++variant)
			{
				const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm};
				{const ScopedRenderTarget2D rt{target.clear(ColorF{.4,.55,.65})};DrivingHud::draw(font,size,{variant ? -2.0 : 18.0,variant ? .4 : 0,1250,50,false,variant==1});}
				Graphics2D::Flush();Image image;target.readAsImage(image);image.save(U"Screenshot/driving_cockpit_{}_{}.png"_fmt(size.x,variant));images<<std::move(image);
			}
			int differences=0,bright=0;
			for (int y=static_cast<int>(panel.y);y<panel.y+panel.h;++y) for (int x=static_cast<int>(panel.x);x<panel.x+panel.w;++x)
			{
				differences+=images[0][y][x]!=images[1][y][x];bright+=images[0][y][x].r>170;
			}
			context.expect(differences>70 && bright>80,U"Speed, reverse gear and steering state are actually drawn");
			context.expect(images[0][size.y/2][size.x/2]==Color{102,140,166},U"The cockpit leaves the forward road view unobstructed");
		}
	});
}
