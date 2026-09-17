#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/ui/TrainTimetableEditor.hpp"
#include "src/ui/KeyboardActions.hpp"
#include "src/render/TrainRenderer.hpp"
#include "src/railway/RailDepotBuilder.hpp"
#include "src/railway/TrainConsist.hpp"

namespace
{
	TrainNetwork sampleNetwork()
	{
		TrainNetwork network;
		const int a = network.addStation({100,20,200}, U"青葉"), b = network.addStation({500,20,200}, U"川原"), c = network.addStation({900,20,200}, U"御嶺");
		network.addEdge(a, b, {233,20,200}, {367,20,200}, 60);
		network.addEdge(c, b, {767,20,200}, {633,20,200}, 60);
		network.addSchedule(RailTimetable::makeDefault(network, a, c));
		return network;
	}
}
void registerRailwayTimetableTests(TestRunner& runner)
{
	runner.add(U"Railway.DefaultDailyTimetable", [](TestContext& context)
	{
		auto network = sampleNetwork(); const auto& schedule = network.schedules().front();
		context.expect(!schedule.name.isEmpty() && RailTimetable::validate(network, schedule).isEmpty(), U"Generated rail connections receive a named and runnable default timetable");
		context.expect(schedule.headwaySec >= RailTimetable::journeySeconds(network, schedule), U"The default interval accommodates the measured route length and dwell");
		context.expectEqual(RailTimetable::departures(schedule, 0).size(), size_t{5}, U"A daily service has a useful next-departure preview");
		TrainManager manager; manager.init(&network); manager.update(0, 0);
		context.expect(manager.trains().isEmpty(), U"Paused new games do not dispatch trains");
		manager.update(.1, .1); context.expectEqual(manager.trains().size(), size_t{1}, U"The 08:00 default starts when the new game runs");
		context.expectNear(*RailTimetable::nextDeparture(network.schedules().front(), 1), schedule.headwaySec, .001, U"A frame-late departure does not shift subsequent clock times");
	});
	runner.add(U"Railway.OvernightAndNoDepartureBacklog", [](TestContext& context)
	{
		TrainSchedule schedule; schedule.firstDepartureMinute = 22 * 60; schedule.lastDepartureMinute = 2 * 60; schedule.headwaySec = 60;
		context.expect(!RailTimetable::dueDeparture(schedule, 0), U"An overnight service waits during the daytime");
		context.expectNear(*RailTimetable::nextDeparture(schedule, 0), 14 * 60, .001, U"22:00 uses the same 24-minute day as the game clock");
		context.expect(RailTimetable::dueDeparture(schedule, 16 * 60 + .2).has_value(), U"The previous day's service continues after midnight");
		schedule.lastSpawnAt = 16 * 60 + .2;
		context.expectNear(*RailTimetable::nextDeparture(schedule, 16 * 60 + .3), 17 * 60, .001, U"Midnight dispatch still keeps the hourly timetable anchored");
		context.expect(!RailTimetable::dueDeparture(schedule, 19 * 60), U"After the service window closes, missed trains do not spawn in a burst");
		context.expectNear(*RailTimetable::nextDeparture(schedule, 19 * 60), 38 * 60, .001, U"The next service restarts at 22:00 the following day");
		schedule.enabled = false; context.expect(!RailTimetable::nextDeparture(schedule, 38 * 60), U"Disabled service has no departure");
		context.expect(!RailTimetable::parseTime(U"24:10") && !RailTimetable::parseTime(U"8:80") && RailTimetable::parseTime(U"08:30") == Optional<int>{510}, U"Clock entry validates hours and minutes");
		context.expect(RailTimetable::parseTime(U"09:09")==Optional<int>{549},U"Leading zeroes never turn decimal clock entries into octal values");
		schedule.enabled=true; schedule.lastSpawnAt=-9999;
		context.expect(RailTimetable::dueDeparture(schedule,18*60+.3).has_value(),U"The final departure is not lost when a frame crosses its exact time");
	});
	runner.add(U"Railway.LiveEditKeepsCurrentService", [](TestContext& context)
	{
		auto network = sampleNetwork(); auto& original = network.schedules().front();
		original.stops = {{0,1},{1,2},{2,3}}; original.headwaySec = 1;
		TrainManager manager; manager.init(&network); manager.update(.1, 0);
		context.expectEqual(manager.trains().size(), size_t{1}, U"An editable service starts");
		TrainTimetableEditor editor; editor.select(original);
		editor.stops.remove_at(1); editor.interval.text = U"60"; editor.enabled = false;
		context.expect(editor.apply(network), U"The user can remove an intermediate stop and suspend future services");
		context.expectEqual(manager.trains().front().serviceStops.size(), size_t{3}, U"A launched train keeps its own stop list after editing");
		bool stoppedAtMiddle = false;
		for (int frame = 1; frame < 1400; ++frame)
		{
			manager.update(.1, frame * .1);
			if (!manager.trains().isEmpty())
			{
				const auto& train = manager.trains().front();
				stoppedAtMiddle |= train.state == TrainState::WaitingStation && train.nextStopIdx == 1;
			}
		}
		context.expect(stoppedAtMiddle && manager.trains().isEmpty(), U"Suspending a line lets its train serve the original intermediate stop and finish");
		for (const auto& edge : network.edges()) { context.expect(edge.occupiedBy < 0, U"Completing the old service releases the route"); }
		context.expectEqual(network.schedules().front().stops.size(), size_t{2}, U"Next departures use the edited stop list");
	});
	runner.add(U"Railway.InvalidEditsAreAtomic", [](TestContext& context)
	{
		auto network = sampleNetwork(); const auto original = network.saveState().formatMinimum();
		TrainTimetableEditor editor; editor.select(network.schedules().front());
		editor.interval.text = U"nan"; context.expect(!editor.apply(network), U"Nonfinite intervals are rejected");
		editor.interval.text = U"1441"; context.expect(!editor.apply(network), U"A daily timetable rejects intervals longer than a day");
		editor.interval.text = U"60"; editor.stops[1].station = editor.stops[0].station;
		context.expect(!editor.apply(network), U"Repeating the same station is rejected");
		context.expect(network.saveState().formatMinimum() == original, U"Invalid edits never modify the live railway");
		TrainSchedule backtrack = network.schedules().front(); backtrack.stops = {{0,1},{2,1},{1,1}};
		context.expect(!RailTimetable::validate(network, backtrack).isEmpty(), U"Intermediate turnbacks that would reserve the same track twice are rejected");
		const int remote = network.addStation({2000,20,200}, U"未接続"); backtrack.stops = {{0,1},{remote,1}};
		context.expect(!RailTimetable::validate(network, backtrack).isEmpty(), U"Disconnected stations provide an error before applying");
	});
	runner.add(U"Railway.TrackAndTimetableSaveRestore", [](TestContext& context)
	{
		auto network = sampleNetwork(); TrainTimetableEditor editor; editor.select(network.schedules().front());
		editor.name.text = U"山あい線"; editor.first.text = U"06:30"; editor.last.text = U"00:30";
		editor.interval.text = U"90"; editor.type = TrainType::Express; editor.stops.insert(editor.stops.begin()+1, {1,TextEditState{U"4"}});
		context.expect(editor.apply(network), U"A custom named overnight line is accepted");
		network.getEdge(0)->occupiedBy = 9;
		context.expect(network.saveState().save(U"TestResults/railway.json"), U"The railway snapshot writes to disk");
		TrainNetwork restored; const bool loaded = restored.restoreState(JSON::Load(U"TestResults/railway.json"));
		context.expect(loaded, U"User-built tracks and edited timetable restore together"); if (!loaded) { return; }
		context.expect(restored.saveState().formatMinimum() == network.saveState().formatMinimum(), U"Names, graph geometry, stop order and dispatch state round-trip exactly");
		context.expect(restored.getEdge(0)->occupiedBy < 0, U"Transient occupancy is not resurrected without a train");
		JSON bad = network.saveState(); bad[U"edges"][0][U"a"] = 9999;
		const auto prior = restored.saveState().formatMinimum();
		context.expect(!restored.restoreState(bad) && restored.saveState().formatMinimum() == prior, U"Invalid saved links leave the current railway intact");
	});
	runner.add(U"Railway.EditorVisualReview", [](TestContext& context)
	{
		auto network = sampleNetwork(); TrainManager manager; manager.init(&network);
		const Font font{FontMethod::MSDF,14}, bold{FontMethod::MSDF,14,Typeface::Bold};
		TrainTimetableEditor editor; editor.select(network.schedules().front());
		for (int variant = 0; variant < 3; ++variant)
		{
			if (variant == 1) { editor.interval.text = U"invalid"; }
			if (variant == 2)
			{
				editor.select(network.schedules().front());
				for (int i = 0; i < 10; ++i) { editor.stops << TrainTimetableEditor::StopRow{1,TextEditState{U"2"}}; }
			}
			RenderTexture target{Size{TrainTimetableEditor::kWidth,1100}};
			{
				const ScopedRenderTarget2D scope{target.clear(ColorF{.055,.075,.09})};
				editor.draw(font,bold,network,manager,0);
			}
			Graphics2D::Flush(); Image image; target.readAsImage(image);
			context.expect(image.save(U"Screenshot/rail_timetable_{}.png"_fmt(variant)), U"The production editor is rendered locally before main-game integration");
			int contrast = 0;
			for (const auto& pixel : image) { contrast += pixel.r > 100 || pixel.g > 100 || pixel.b > 100; }
			context.expect(contrast > 14000 && editor.contentHeight() < image.height(), U"Controls and validation text render with visible contrast within the scrollable content bounds");
		}
		GameInput::textInput = &editor.name; editor.name.active = true;
		context.expect(GameInput::keyboardBlocked(), U"Timetable text owns WASD and all global shortcut input");
		editor.select(network.schedules().front());
		context.expect(GameInput::textInput == nullptr, U"Switching drafts releases focus before replacing text fields");
		context.expect(font(U"現実24分で1日。終発が早い場合は翌日です").region().w < TrainTimetableEditor::kWidth-16, U"Time-scale guidance fits the panel");
	});
	runner.add(U"Railway.StationDepotGeometryAndStorage", [](TestContext& context)
	{
		World world; world.reserveChunks(); world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		Grid<float> height(HEIGHT_CELLS+1,HEIGHT_CELLS+1,10);
		world.installChunkDirect({0,0},HeightMapResult{height,10,10});
		auto network = sampleNetwork(); RoadNetwork roads; String error;
		const auto originalRoute = network.findRoute(0,2);
		context.expect(RailDepotBuilder::add(network,world,roads,0,error), U"A terminal station gains a connected depot on suitable empty land");
		context.expectEqual(network.depots().size(),size_t{1},U"Exactly one depot is created");
		if (network.depots().isEmpty()) { return; }
		context.expect(network.findRoute(0,2)==originalRoute,U"A depot siding cannot divert a passenger route");
		const auto before = network.saveState().formatMinimum();
		context.expect(!RailDepotBuilder::add(network,world,roads,0,error) && before==network.saveState().formatMinimum(),U"A second depot at the same station is rejected atomically");
		TrainNetwork restored; context.expect(restored.restoreState(network.saveState()),U"Depot throat, sidings and name survive saving");
		const auto parked = RailFacilities::parkedTrains(restored);
		context.expectEqual(parked.size(),size_t{2},U"Both sidings contain parked Japanese EMU formations");
		for (const auto& train : parked)
		{
			const auto profile = TrainConsist::profile(train.type);
			for (int car=0;car<profile.cars;++car)
			{
				context.expect(TrainConsist::behind(train,restored,(car+.5f)*20-6.75f).has_value()
					&& TrainConsist::behind(train,restored,(car+.5f)*20+6.75f).has_value(),U"Every parked bogie is on a real siding");
			}
		}
		for (const auto geometry : {RailFacilities::station(network,world,0),RailFacilities::depot(network,world,network.depots().front())})
		{
			size_t triangles=0;
			for (const auto& mesh : geometry.parts)
			{
				triangles+=mesh.indices.size();
				for (const auto& vertex : mesh.vertices)
				{
					context.expect(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y) && std::isfinite(vertex.pos.z)
						&& vertex.normal.lengthSq()>.9,U"Facility mesh vertices and face normals are finite");
				}
			}
			context.expect(triangles>200 && !geometry.signs.isEmpty(),U"Stations and depots have detailed solid geometry and actual names");
		}
		const auto station = RailFacilities::station(network,world,0);
		// The straight fixture runs along +X. All platform geometry must remain outside the 3.1m car width.
		for (const auto& vertex : station.parts[RailFacilities::Tactile].vertices)
		{
			context.expect(Abs(vertex.pos.z-200)>1.75,U"Tactile strips and platform edges stay outside the train body envelope");
		}
		ParcelRoadIndex sites{roads}; sites.addRailway(network);
		const auto yard = RailwaySite::depotFrame(network,network.depots().front());
		const Vec3 shed = yard->point(4,0,70);
		context.expect(sites.overlaps(ParcelGeometry::footprint({shed.x,shed.z},3,0)),U"The full depot site is reserved against procedural houses and new zoning");
	});
	runner.add(U"Railway.DepotRejectsRoadAndTerrain", [](TestContext& context)
	{
		for (int variant=0;variant<2;++variant)
		{
			World world; world.reserveChunks(); world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
			const float ground = variant==0 ? 10.0f : 50.0f;
			Grid<float> height(HEIGHT_CELLS+1,HEIGHT_CELLS+1,ground); world.installChunkDirect({0,0},HeightMapResult{height,ground,ground});
			auto network=sampleNetwork(); RoadNetwork roads;
			if (variant==0)
			{
				for (int x=160;x<=720;x+=40)
				{
					const int a=roads.addNode({static_cast<double>(x),10,110}),b=roads.addNode({static_cast<double>(x),10,195});
					roads.addEdge(a,b,{static_cast<double>(x),10,138},{static_cast<double>(x),10,168},RoadType::LocalRoad,2);
				}
			}
			const auto original=network.saveState().formatMinimum(); String error;
			context.expect(!RailDepotBuilder::add(network,world,roads,0,error) && !error.isEmpty()
				&& network.saveState().formatMinimum()==original,U"Road or terrain conflicts leave no partial track or station mutations");
		}
	});
	runner.add(U"Railway.StationAndDepotGpuReview", [](TestContext& context)
	{
		const FilePath directory=FileSystem::CurrentDirectory();
		struct RestoreDirectory { FilePath path; ~RestoreDirectory() { FileSystem::ChangeCurrentDirectory(path); } } restore{directory};
		FileSystem::ChangeCurrentDirectory(directory+U"../../App/");
		World world; world.reserveChunks(); world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		Grid<float> height(HEIGHT_CELLS+1,HEIGHT_CELLS+1,10); world.installChunkDirect({0,0},HeightMapResult{height,10,10});
		auto network=sampleNetwork(); RoadNetwork roads; String error; RailDepotBuilder::add(network,world,roads,0,error);
		TrainRenderer renderer;
		for (int view=0;view<3;++view)
		{
			const Size size{1280,720}; const Color background{35,42,48};
			const Vec3 eye = view==0 ? Vec3{55,75,280} : view==1 ? Vec3{130,34,230} : Vec3{150,62,110};
			const Vec3 look = view==2 ? Vec3{255,23,178} : Vec3{135,23,205};
			const BasicCamera3D camera{size,40_deg,eye,look};
			RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};
			for (int frame=0;frame<2;++frame)
			{
				renderer.prepareFacilityTextures();
				{
					const ScopedRenderTarget3D scope{target.clear(background)};
					const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
					Graphics3D::SetCameraTransform(camera); Graphics3D::SetGlobalAmbientColor(ColorF{.6});
					renderer.renderTracks(network,world,eye,roads);
				}
				Graphics3D::Flush();
			}
			Image image; target.readAsImage(image); int pixels=0, yellow=0; const Color clear=image[0][0];
			for (const auto& pixel : image) { pixels+=pixel!=clear; yellow+=pixel.r>pixel.b*1.8 && pixel.g>pixel.b*1.3 && pixel.r>70; }
			context.expect(pixels>9000,U"Production station and depot models are visible from aerial, platform and depot approaches");
			if (view<2) { context.expect(yellow>30,U"Platform guidance remains visible in the actual GPU frame"); }
			context.expect(image.save(directory+U"Screenshot/rail_facilities_{}.png"_fmt(view)),U"Station, platform and depot views are saved locally for review");
		}
	});

	runner.add(U"Railway.BrakesBeforeLowerLimit", [](TestContext& context)
	{
		auto network=sampleNetwork(); network.getEdge(0)->speedLimit=90; network.getEdge(1)->speedLimit=20;
		TrainManager trains; trains.init(&network); bool entered=false; double speed=0;
		for (int frame=0;frame<1500 && !entered;++frame)
		{
			trains.update(.05,frame*.05);
			if (!trains.trains().isEmpty() && trains.trains().front().currentEdge==1)
			{
				entered=true; speed=trains.trains().front().speed;
			}
		}
		context.expect(entered && speed<=20/3.6+.15,U"The train brakes on the approach and reaches the lower limit before entering the next track");
	});

	runner.add(U"Railway.DepotSearchesBeyondStationFrontage", [](TestContext& context)
	{
		World world; world.reserveChunks(); world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		Grid<float> height(HEIGHT_CELLS+1,HEIGHT_CELLS+1,10); world.installChunkDirect({0,0},HeightMapResult{height,10,10});
		auto network=sampleNetwork(); RoadNetwork roads;
		const int a=roads.addNode({230,10,175}),b=roads.addNode({400,10,175});
		roads.addEdge(a,b,{285,10,175},{345,10,175},RoadType::LocalRoad,2);
		String error; context.expect(RailDepotBuilder::add(network,world,roads,0,error),U"A blocked adjacent pad triggers a search for usable land farther from the station");
	});

	runner.add(U"Railway.DepotApproachVerticalClearance", [](TestContext& context)
	{
		for (int scenario=0;scenario<2;++scenario)
		{
			World world; world.reserveChunks(); world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
			Grid<float> height(HEIGHT_CELLS+1,HEIGHT_CELLS+1,10); world.installChunkDirect({0,0},HeightMapResult{height,10,10});
			auto network=sampleNetwork(); RoadNetwork roads; const double roadHeight=scenario==0 ? 10 : 19;
			const int a=roads.addNode({140,roadHeight,80}),b=roads.addNode({140,roadHeight,240});
			const int id=*roads.addEdge(a,b,{140,roadHeight,133},{140,roadHeight,187},RoadType::LocalRoad,2);
			roads.getEdge(id)->useElevation=true; String error;
			const bool built=RailDepotBuilder::add(network,world,roads,0,error);
			context.expect(built==(scenario==0),U"An elevated depot approach may cross a road only with the required vertical clearance");
		}
	});

}
