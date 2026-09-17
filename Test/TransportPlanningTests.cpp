#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/road/RoadPlanDraft.hpp"
#include "src/road/RoadPlanConstruction.hpp"
#include "src/gen/RoadAutoPlace.hpp"
#include "src/gen/RoadDesignLimits.hpp"
#include "src/road/LocationSigns.hpp"
#include "src/road/SignArtwork.hpp"
#include "src/road/RoadSign.hpp"
#include "src/railway/TrainConsist.hpp"
#include "src/railway/TrainManager.hpp"
#include "src/render/TrainRenderer.hpp"
#include "src/render/RoadRenderer.hpp"
#include "src/asset/AssetRegistrar.hpp"

void registerTransportPlanningTests(TestRunner& runner)
{
	runner.add(U"Planning.GenerateEditAndConstruct",[](TestContext& context)
	{
		World world; world.reserveChunks(); world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		Grid<float> height(HEIGHT_CELLS+1,HEIGHT_CELLS+1,20);
		for (int z=22;z<43;++z) { for (int x=23;x<40;++x) { height[z][x]=95; } }
		world.installChunkDirect({0,0},HeightMapResult{height,20,95});
		RoadPlanDraft draft; const RoadEdge road=RoadPlanDraft::makeRoadTemplate(0);
		draft.place({160,20,512}); draft.place({880,20,512});
		context.expect(!draft.valid() && !draft.generated(),U"Selecting endpoints alone never generates or builds a road");
		context.expect(draft.generate(world,road),U"Explicit generation uses the production terrain pathfinder");
		const auto generated=draft.points();
		context.expect(generated==RoadAutoPlace::findWaypoints(world,generated.front(),generated.back()),U"The editor receives the actual shared pathfinder candidate");
		context.expect(generated.any([](Vec3 point) { return Abs(point.z-512)>100; }),U"The route detours around the measured steep hill");
		context.expect(draft.undo() && !draft.generated() && draft.points().size()==2,U"Undo generation returns to endpoint selection");
		context.expect(draft.redo() && draft.generated(),U"Redo restores the generated candidate and its stage");
		context.expect(draft.rebuild(world,road),U"Redo restores the original valid curve geometry");
		TextWriter dragReport{U"TestResults/road_drag_radius.txt"};
		dragReport << U"anchors={} limit={}"_fmt(generated.size(),RoadDesignLimits::forType(road.roadType).minimumRadius);
		for(double shift:{1.0,3.0,10.0,35.0})
		{
			double radius=Math::Inf;int index=0;
			for(const auto& edge:draft.preview().edges())
			{
				if(edge.id<0) { continue; }const auto curve=draft.preview().getBezier(edge.id);
				const Vec3 aShift{0,0,index==static_cast<int>(generated.size()/2) ? shift : 0};
				const Vec3 bShift{0,0,index+1==static_cast<int>(generated.size()/2) ? shift : 0};
				const CubicBezier moved{curve->p0+aShift,curve->p1+aShift,curve->p2+bShift,curve->p3+bShift};
				radius=Min(radius,moved.minimumHorizontalRadius());++index;
			}
			dragReport << U"shift={} radius={}"_fmt(shift,radius);
		}
		Array<Vec3> edited=generated;
		edited[edited.size()/2].z+=1;
		context.expect(draft.revise(edited) && draft.rebuild(world,road),U"A completed drag adjusts the candidate without rerouting it");
		context.expect(draft.points()==edited,U"Editing does not silently replace the user's points with another A-star route");
		const double length=draft.length();
		RoadNetwork live; const auto ids=draft.apply(live,world,road);
		double builtLength=0; for (int id:ids) { builtLength+=live.getEdge(id)->length; }
		context.expectNear(builtLength,length,.01,U"Construction uses the reviewed geometry exactly");
		const auto audit=RoadDesignLimits::measure(live,world);
		context.expect(audit.gradeViolations==0 && audit.radiusViolations==0,U"Manual planning retains road-class grade and curvature limits");
		context.expect(draft.undo() && draft.points()==generated,U"One undo reverses a whole drag");
		Array<Vec3> tooSharp=generated;tooSharp[tooSharp.size()/2].z+=35;
		context.expect(draft.revise(tooSharp) && !draft.rebuild(world,road),U"Moving a dense 10 m anchor by 35 m cannot create an illegal hairpin");
		context.expect(draft.undo() && draft.points()==generated,U"An invalid large drag can be undone without losing the generated route");
		Array<Vec3> invalid=draft.points(); invalid[1]=invalid[0];
		context.expect(!draft.revise(invalid) && draft.points()==generated,U"Collapsed segments are rejected without losing the candidate");
		RoadPlanDraft impossible; impossible.place({100,20,100}); impossible.place({110,70,100});
		context.expect(!impossible.generate(world,road),U"An impossible fixed-endpoint slope cannot be confirmed");
	});
	runner.add(U"Planning.JapaneseTrainsRoundTripAndExclusion",[](TestContext& context)
	{
		TrainNetwork network; Array<int> nodes,edges;
		for (int index=0;index<=8;++index) { nodes << network.addNode({100.0+index*55,20.0+index*.5,200.0+Sin(index*.2)*40},index==0 || index==8 ? TrackNodeType::Station : TrackNodeType::Joint); }
		for (int index=0;index<8;++index)
		{
			const Vec3 a=network.getNode(nodes[index])->position,b=network.getNode(nodes[index+1])->position;
			edges << (index%2 ? network.addEdge(nodes[index+1],nodes[index],b.lerp(a,1.0/3),b.lerp(a,2.0/3),60)
				: network.addEdge(nodes[index],nodes[index+1],a.lerp(b,1.0/3),a.lerp(b,2.0/3),60));
		}
		TrainSchedule service; service.id=0;service.type=TrainType::Express;service.headwaySec=1;service.stops={StopEntry{nodes.front(),1},StopEntry{nodes.back(),1}};network.addSchedule(service);
		TrainManager trains;trains.init(&network);trains.update(0,0);
		context.expect(trains.trains().isEmpty(),U"Paused time never spawns or moves a train");
		bool reversed=false,stopped=false,bodySpannedSections=false;double maximumStep=0;
		Optional<Vec3> previous; int previousId=-1;
		for (int frame=0;frame<1600;++frame)
		{
			trains.update(.1,frame*.1);
			context.expect(trains.trains().size()<=1,U"An occupied single-track route cannot spawn an opposing or overlapping train");
			if (trains.trains().isEmpty()) { previous=none; continue; }
			const auto& train=trains.trains().front();reversed|=train.reverseService;stopped|=train.state==TrainState::WaitingStation;
			if (previous && previousId==train.id) { maximumStep=Max(maximumStep,train.position.distanceFrom(*previous)); }
			previous=train.position;previousId=train.id;
			for (int car=0;car<4;++car)
			{
				const auto front=TrainConsist::behind(train,network,(car+.5f)*20-6.75f),rear=TrainConsist::behind(train,network,(car+.5f)*20+6.75f);
				context.expect(front && rear,U"Every bogie lies on an actual route section, including at departure");
				if (front && rear) { context.expect(front->distanceFrom(*rear)>13.4 && front->distanceFrom(*rear)<13.51,U"Bogies retain their wheelbase while following a curve"); }
			}
			bodySpannedSections|=train.arcPos<60 && train.routeProgress>0;
			for (int id:edges) { context.expect(network.getEdge(id)->occupiedBy==train.id,U"The whole consist's route stays reserved until arrival and dwell finish"); }
			if (reversed && stopped) { break; }
		}
		context.expect(reversed && stopped && bodySpannedSections,U"The service stops, releases its route and returns in the opposite direction");
		context.expect(maximumStep<1.68,U"60 km/h yields at most 1.67 m per tenth-second, with no 60x time multiplier");
		TextWriter{U"TestResults/japanese_trains.txt"}.write(U"reversed={} stopped={} maxStepM={:.5f}"_fmt(reversed,stopped,maximumStep));
	});
	runner.add(U"Planning.SignsMatchTheirLocationsAndRules",[](TestContext& context)
	{
		const CubicBezier curve{{0,20,0},{100,20,0},{200,20,0},{300,20,0}};
		const auto boundaries=LocationSigns::boundaries(curve,[](Vec2 point) { return point.x<133 ? U"青葉市" : U"川原町"; });
		context.expectEqual(boundaries.size(),size_t{1},U"Only an actual municipal boundary receives a country sign");
		context.expectNear(boundaries.front().arc,133,.02,U"The sign position follows the geographic boundary");
		context.expect(boundaries.front().before==U"青葉市" && boundaries.front().after==U"川原町",U"Opposing approaches name the municipality they enter");
		context.expect(LocationSigns::boundaries(curve,[](Vec2) { return U"青葉市"; }).isEmpty(),U"No fake boundary signs within one municipality");
		RoadNetwork roads;const int a=roads.addNode({0,20,0}),b=roads.addNode({200,20,0});
		const int id=*roads.addEdge(a,b,{67,20,0},{133,20,0},RoadType::LocalRoad,2);
		for (Vec3 end:{Vec3{200,20,-100},Vec3{200,20,100}}) { const int node=roads.addNode(end);roads.addEdge(b,node,Vec3{200,20,0}.lerp(end,.33),Vec3{200,20,0}.lerp(end,.67),RoadType::LocalRoad,2); }
		roads.getEdge(id)->speedLimit=25;
		roads.getNode(b)->getAttachment(id)->control=TrafficControl::Yield;
		const auto signs=RoadSign::InferAutoForEdge(*roads.getEdge(id),roads);
		context.expect(signs.any([](const RoadSignPlacement& sign) { return sign.type==RoadSignType::SpeedLimit && sign.auxValue==25; }),U"The posted speed is the actual limit, never rounded down to a different rule");
		context.expect(signs.any([](const RoadSignPlacement& sign) { return sign.type==RoadSignType::Yield; }),U"A slow approach receives the matching sign");
		context.expect(SignArtwork::textureKey(RoadSignType::PrefectureRoute,40,U"")!=SignArtwork::textureKey(RoadSignType::NationalRoute,40,U""),U"Equal national and prefectural route numbers cannot collide in the texture cache");
	});
	runner.add(U"Planning.ConstructionTransactionPreservesTheCity", [](TestContext& context)
	{
		World world;
		world.reserveChunks();
		world.setGenerationParams(42, WORLD_SIZE, WORLD_SIZE);
		Grid<float> height(HEIGHT_CELLS + 1, HEIGHT_CELLS + 1, 20);
		world.installChunkDirect({0, 0}, HeightMapResult{height, 20, 20});
		RoadNetwork network;
		const int from = network.addNode({200, 20, 512});
		const int to = network.addNode({800, 20, 512});
		const int existingEdge = *network.addEdge(from, to, {400, 20, 512}, {600, 20, 512});
		const int routeId = network.addRoute(RoadRouteKind::Named, U"既存通り", {existingEdge});
		RoadPlan original;
		original.routeId = routeId;
		original.edgeIds = {existingEdge};
		const int originalPlanId = network.addPlan(original);
		const int nextNode = network.nextNodeId();
		const int nextEdge = network.nextEdgeId();

		RoadPlanDraft draft;
		draft.place({500, 20, 512});
		draft.place({500, 20, 800});
		const RoadEdge road = RoadPlanDraft::makeRoadTemplate(0);
		context.expect(draft.generate(world, road), U"A reviewed branch can join an existing named street");
		const auto points = draft.points();
		const double previewLength = draft.length();
		RoadPlanConstruction::Request request{U"駅前への延伸", U"使用しない名称", routeId};
		const auto refused = RoadPlanConstruction::commit(network, world, draft, road, request, 0, 120);
		const auto* error = std::get_if<RoadPlanConstruction::Error>(&refused);
		context.expect(error && *error == RoadPlanConstruction::Error::InsufficientFunds,
			U"The real connection is priced before a no-funds request is refused");
		context.expect(network.getEdge(existingEdge) && network.nextEdgeId() == nextEdge && network.nextNodeId() == nextNode,
			U"A refused branch rolls back existing-road splits and ID allocation");
		context.expect(network.getRoute(routeId)->edgeIds == Array<int>{existingEdge}
			&& network.getPlan(originalPlanId)->edgeIds == Array<int>{existingEdge}
			&& network.plans().size() == 1, U"A refused branch preserves route and construction-plan membership");
		context.expect(draft.points() == points && draft.valid(), U"Refusal keeps the reviewed candidate available");

		request.existingRouteId = routeId + 999;
		const auto missing = RoadPlanConstruction::commit(network, world, draft, road, request, 30, 120);
		const auto* missingError = std::get_if<RoadPlanConstruction::Error>(&missing);
		context.expect(missingError && *missingError == RoadPlanConstruction::Error::MissingRoute
			&& network.nextNodeId() == nextNode, U"A deleted destination route cannot silently become a new route");
		request.existingRouteId = routeId;
		const auto result = RoadPlanConstruction::commit(network, world, draft, road, request, 30, 120);
		const auto* receipt = std::get_if<RoadPlanConstruction::Receipt>(&result);
		context.expect(receipt != nullptr, U"The funded candidate commits as one construction transaction");
		if (!receipt) { return; }
		const auto* built = network.getPlan(receipt->planId);
		context.expect(built && built->state == PlanState::UnderConstruction && built->constructionStart == 120,
			U"The committed plan is already under construction at the requested simulation time");
		context.expect(built && built->name == request.planName && built->routeName == U"既存通り"
			&& built->routeId == routeId, U"Appending preserves the actual destination route's identity");
		context.expectNear(built ? built->totalLength : 0, previewLength, .01,
			U"The constructed length still matches the reviewed preview");
		context.expect(receipt->removedEdgeIds.includes(existingEdge) && !receipt->affectedNodeIds.isEmpty(),
			U"The scene receives the removed road and affected junctions for terrain and traffic updates");
		context.expectEqual(network.getPlan(originalPlanId)->edgeIds.size(), 2,
			U"The original plan keeps both split pieces");
		for (const int id : receipt->edgeIds)
		{
			const auto* edge = network.getEdge(id);
			context.expect(edge && edge->edgeState == EdgeState::UnderConstruction && edge->planId == receipt->planId,
				U"Every constructed edge belongs to the started plan");
			context.expect(network.getRoute(routeId)->edgeIds.includes(id), U"Every new edge is indexed in the existing route");
		}
		context.expect(!network.startPlanConstruction(receipt->planId, 121), U"The same work cannot start twice");

		RoadPlanDraft separate;
		separate.place({300, 20, 100});
		separate.place({600, 20, 100});
		context.expect(separate.generate(world, road), U"A separate candidate is generated for a new route");
		const auto named = RoadPlanConstruction::commit(network, world, separate, road, {U"", U"並木通り", none}, 30, 200);
		const auto* namedReceipt = std::get_if<RoadPlanConstruction::Receipt>(&named);
		context.expect(namedReceipt != nullptr, U"A new named route can also be constructed");
		if (namedReceipt)
		{
			const auto* plan = network.getPlan(namedReceipt->planId);
			context.expect(plan && plan->routeName == U"並木通り" && !plan->name.isEmpty(),
				U"New-route naming and the default plan name are retained");
		}
	});
	runner.add(U"Planning.TrainIntermediateStopsAndDepartureEligibility", [](TestContext& context)
	{
		TrainNetwork network;
		const int first = network.addStation({0, 20, 0}, U"始発");
		const int middle = network.addStation({200, 20, 0}, U"途中");
		const int last = network.addStation({400, 20, 0}, U"終点");
		const int outbound = network.addEdge(first, middle, {200.0/3, 20, 0}, {400.0/3, 20, 0}, 60);
		const int returning = network.addEdge(last, middle, {1000.0/3, 20, 0}, {800.0/3, 20, 0}, 60);
		TrainSchedule schedule;
		schedule.type = TrainType::Local;
		schedule.headwaySec = 1;
		schedule.stops = {{first, 1}, {middle, 2}, {last, 3}};
		network.addSchedule(schedule);
		TrainManager manager;
		manager.init(&network);
		network.getEdge(returning)->electrified = false;
		manager.update(.1, 0);
		context.expect(manager.trains().isEmpty() && network.getEdge(outbound)->occupiedBy < 0,
			U"An unelectrified later leg prevents departure without reserving the earlier leg");
		network.getEdge(returning)->electrified = true;
		network.tryOccupy(returning, 99);
		manager.update(.1, 1);
		context.expect(manager.trains().isEmpty() && network.getEdge(outbound)->occupiedBy < 0,
			U"An occupied later leg prevents partial route reservation");
		network.releaseOccupy(returning, 99);

		Array<int> arrivals;
		int previousTrain = -1, previousStop = -1;
		for (int frame = 0; frame < 3000 && arrivals.size() < 4; ++frame)
		{
			manager.update(.1, 2 + frame * .1);
			if (manager.trains().isEmpty()) { continue; }
			const auto& train = manager.trains().front();
			if (train.state != TrainState::WaitingStation
				|| (train.id == previousTrain && train.nextStopIdx == previousStop)) { continue; }
			const auto* edge = network.getEdge(train.currentEdge);
			const int node = train.forward ? edge->nodeB : edge->nodeA;
			arrivals << node;
			previousTrain = train.id;
			previousStop = train.nextStopIdx;
			context.expectNear(train.waitRemaining, node == middle ? 2 : node == last ? 3 : 1, .001,
				U"Each approach selects that station's dwell time, including the reversed service");
			const float remaining = train.waitRemaining;
			const Vec3 position = train.position;
			manager.update(0, 99999);
			context.expect(manager.trains().front().position == position && manager.trains().front().waitRemaining == remaining,
				U"Pausing at a station never consumes dwell time or moves the consist");
		}
		context.expect(arrivals == Array<int>{middle, last, middle, first},
			U"Both services visit their intermediate and terminal stations in the correct order");
	});
	runner.add(U"Planning.GuideSignSelectionMatchesRenderedBoard", [](TestContext& context)
	{
		RegisterAssets();
		const FilePath directory = FileSystem::CurrentDirectory();
		struct RestoreDirectory
		{
			FilePath path;
			~RestoreDirectory() { FileSystem::ChangeCurrentDirectory(path); }
		} restore{directory};
		FileSystem::ChangeCurrentDirectory(directory + U"../../App/");
		World world;
		world.reserveChunks();
		world.setGenerationParams(42, WORLD_SIZE, WORLD_SIZE);
		Grid<float> height(HEIGHT_CELLS + 1, HEIGHT_CELLS + 1, 20);
		world.installChunkDirect({0, 0}, HeightMapResult{height, 20, 20});
		RoadNetwork network;
		const int from = network.addNode({500, 20, 300});
		const int to = network.addNode({500, 20, 700});
		const int edge = *network.addEdge(from, to, {500, 20, 1300.0/3}, {500, 20, 1700.0/3});
		network.getEdge(edge)->edgeState = EdgeState::Existing;
		GuideSignPlacement sign;
		sign.parentEdgeId = edge;
		sign.nodeEndId = to;
		sign.arcOffset = 200;
		sign.lateralOffset = -8;
		sign.widthOverride = 4;
		sign.heightOverride = 2;
		SignElement label;
		label.text = U"青葉駅";
		sign.elements = {label};
		const int signId = network.addGuideSign(sign);
		RoadRenderer renderer;
		context.expect(renderer.loadAssets(), U"The sign review uses production road and pole assets");
		renderer.prepareGuideSignTextures(network);
		context.expect(renderer.getGuideSignCachedTexture(*network.getGuideSign(signId)) != nullptr,
			U"The guide board's real texture is prepared before the 3D pass");
		const Size size{640, 480};
		const BasicCamera3D camera{size, 40_deg, Vec3{489.5, 26, 545}, Vec3{489.5, 25.75, 500}};
		RenderTexture target{size, TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes};
		Array<Image> images;
		for (const bool selection : {false, true})
		{
			{
				const ScopedRenderTarget3D scope{target.clear(ColorF{.1})};
				const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
				Graphics3D::SetCameraTransform(camera);
				Graphics3D::SetGlobalAmbientColor(ColorF{.6});
				if (selection) { renderer.drawGuideSignSilhouette(signId, network, world, ColorF{1}); }
				else { renderer.render(network, world, ViewFrustum{camera, 24000}, camera.getEyePosition()); }
			}
			Graphics3D::Flush();
			Image image;
			target.readAsImage(image);
			images << std::move(image);
		}
		int bluePixels = 0, outsideSelection = 0;
		const Color clear = images[1][0][0];
		for (int y = 0; y < size.y; ++y)
		{
			for (int x = 0; x < size.x; ++x)
			{
				const Color pixel = images[0][y][x];
				if (pixel.b <= pixel.r + 30 || pixel.g <= pixel.r + 15) { continue; }
				++bluePixels;
				outsideSelection += images[1][y][x] == clear;
			}
		}
		context.expect(bluePixels > 500, U"The production renderer draws the guide board's blue face");
		context.expectEqual(outsideSelection, 0, U"The selected silhouette covers the actual board at the same position");
		images[0].save(directory + U"Screenshot/guide_sign_normal.png");
		const BasicCamera3D reverseCamera{size,40_deg,Vec3{489.5,26,455},Vec3{489.5,25.75,500}};
		{
			const ScopedRenderTarget3D scope{target.clear(ColorF{.1})};
			const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
			Graphics3D::SetCameraTransform(reverseCamera);
			renderer.render(network,world,ViewFrustum{reverseCamera,24000},reverseCamera.getEyePosition());
		}Graphics3D::Flush();Image reverse;target.readAsImage(reverse);int reverseBlue=0;
		for (const auto pixel : reverse) {reverseBlue+=pixel.b>pixel.r+30 && pixel.g>pixel.r+15;}
		context.expectEqual(reverseBlue,0,U"The opposing approach sees only the plain back of the guide sign");
		reverse.save(directory+U"Screenshot/guide_sign_reverse.png");
		images[1].save(directory + U"Screenshot/guide_sign_selection.png");
		TextWriter{directory + U"TestResults/guide_sign_selection.txt"}.write(
			U"bluePixels={} outsideSelection={}"_fmt(bluePixels, outsideSelection));
	});
	runner.add(U"Planning.JapaneseTrainAndSignVisualReview",[](TestContext& context)
	{
		RegisterAssets(); const FilePath current=FileSystem::CurrentDirectory();
		struct Restore {FilePath path;~Restore(){FileSystem::ChangeCurrentDirectory(path);}} restore{current};
		FileSystem::ChangeCurrentDirectory(current+U"../../App/");
		const Font font=FontAsset(Asset::CJK14);
		const Array<std::pair<RoadSignType,int>> signs={{RoadSignType::SpeedLimit,25},{RoadSignType::OneWay,0},{RoadSignType::DirectionalRestriction,3},{RoadSignType::NationalRoute,12},{RoadSignType::PrefectureRoute,12},{RoadSignType::SteepGrade,-8},{RoadSignType::NarrowRoad,0},{RoadSignType::CurveWarning,1}};
		for (const auto& [type,value]:signs)
		{
			RenderTexture target{256,256,ColorF{0,0}};
			{ const ScopedRenderTarget2D scope{target};const ScopedRenderStates2D blend{BlendState::Opaque};SignArtwork::draw(type,value,font); }Graphics2D::Flush();
			Image image;target.readAsImage(image);int colored=0;
			for (const auto& pixel:image) { colored+=pixel.a>0 && Max(pixel.r,Max(pixel.g,pixel.b))-Min(pixel.r,Min(pixel.g,pixel.b))>40; }
			context.expect(colored>5000,U"Each regulatory and warning face has a readable coloured symbol");
			context.expect(!RoadSign::CreateBoardMesh(type).vertices.isEmpty(),U"Each sign has an actual renderable board mesh");
			image.save(current+U"Screenshot/new_sign_{}.png"_fmt(static_cast<int>(type)));
		}
		for (RoadSignType type:{RoadSignType::RoadName,RoadSignType::Municipality})
		{
			RenderTexture target{768,256,ColorF{0,0}};
			{ const ScopedRenderTarget2D scope{target};const ScopedRenderStates2D blend{BlendState::Opaque};SignArtwork::draw(type,0,font,type==RoadSignType::RoadName ? U"青葉駅前通り" : U"川原町"); }Graphics2D::Flush();
			Image image;target.readAsImage(image);image.save(current+U"Screenshot/new_sign_{}.png"_fmt(static_cast<int>(type)));
			context.expect(image[Point{384,140}].a>0,U"Wide plaques render into their full-size texture");
		}
		TrainNetwork network;Array<int> nodes,edges;
		for (int index=0;index<=6;++index) {nodes << network.addNode({index*30.0,0,index*index*1.2});}
		for (int index=0;index<6;++index) {const Vec3 a=network.getNode(nodes[index])->position,b=network.getNode(nodes[index+1])->position;edges << network.addEdge(nodes[index],nodes[index+1],a.lerp(b,.33),a.lerp(b,.67));}
		TrainRenderer renderer;
		for (TrainType type:{TrainType::Local,TrainType::Express})
		{
			Train train;train.type=type;train.routeEdges=edges;train.routeProgress=4;train.currentEdge=edges[4];train.arcPos=25;
			const auto profile=TrainConsist::profile(type);
			for (StringView stem:{profile.cab,profile.trailer}) { Model model{U"assets/railway/{}.obj"_fmt(stem)};context.expect(!model.isEmpty(),U"Production EMU assets load successfully"); }
			const Size size{1280,720};const Color background{35,42,48};
			const BasicCamera3D camera{size,40_deg,Vec3{160,65,-90},Vec3{100,1,15}};
			RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};
			{
				const ScopedRenderTarget3D scope{target.clear(background)};const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};Graphics3D::SetCameraTransform(camera);Graphics3D::SetGlobalAmbientColor(ColorF{.6});
				renderer.renderTrains({train},network);
			}Graphics3D::Flush();
			Image image;target.readAsImage(image);int pixels=0;const Color clear=image[0][0];for (const auto& pixel:image) {pixels+=pixel!=clear;}
			context.expect(pixels>3000,U"The complete production formation is actually rendered by the GPU");
			image.save(current+U"Screenshot/japanese_train_{}.png"_fmt(static_cast<int>(type)));
		}
	});
}
