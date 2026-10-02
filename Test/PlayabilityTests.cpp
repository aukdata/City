#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/ui/KeyboardActions.hpp"
#include "src/ui/CommandPalette.hpp"
#include "src/ui/PanelWidget.hpp"
#include "src/ui/PauseMenu.hpp"
#include "src/ui/NavigationHelp.hpp"
#include "src/asset/AssetRegistrar.hpp"
#include "src/ui/Camera.hpp"
#include "src/ui/WorldMapView.hpp"
#include "src/economy/Economy.hpp"
#include "src/render/RoadRenderer.hpp"
#include "src/scene/WorldSelection.hpp"
#include "src/render/RenderQuality.hpp"

void registerPlayabilityTests(TestRunner& runner)
{
	runner.add(U"WorldSelection.RoofOccludesGroundSignal", [](TestContext& context)
	{
		const Size native{1280, 768};
		const BasicCamera3D camera{native, 40_deg, Vec3{0, 20, -30}, Vec3{0, 6, 0}};
		const Ray ray = camera.screenToRay(Vec2{640, 384});
		const auto roofHit = Box{Vec3{0, 3, 0}, Vec3{8, 6, 8}}.intersects(ray);
		const Optional<double> roofDistance = roofHit ? Optional<double>{static_cast<double>(*roofHit)} : none;
		const auto ground = ray.intersectsAt(InfinitePlane{Float3{0, 0, 0}, Float3{0, 1, 0}});
		context.expect(roofDistance.has_value() && ground.has_value(), U"The fixture ray hits the roof before flat terrain");
		if (!roofDistance || !ground) { return; }
		context.expectNear(*roofDistance, Sqrt(1096.0), .001, U"The roof center has the expected ray depth");
		context.expectNear(ground->z, 90.0 / 7.0, .001, U"The terrain cursor lies behind the roof");
		const Vec3 signalAnchor{0, 0, 90.0 / 7.0};
		context.expect(Vec2{ground->x - signalAnchor.x, ground->z - signalAnchor.z}.length() < 10,
			U"The existing ten-meter signal proximity search accepts this hidden signal");
		const MeshData signal = MeshData::Box(Float3{0, 3, 0}, Float3{.4f, 6, .4f});
		const auto signalDistance = WorldSelection::meshDistance(ray, signal.vertices, signal.indices, Mat4x4::Translate(signalAnchor));
		context.expect(signalDistance && *signalDistance > *roofDistance,
			U"The actual signal mesh is hit only behind the roof");
		context.expect(WorldSelection::choose(*roofDistance, false, none, true, signalDistance) == WorldSelection::Surface::Building,
			U"A roof ray selects the near building instead of infrastructure reached through the roof");
		context.expect(WorldSelection::choose(*roofDistance, true, signalDistance, false, none) == WorldSelection::Surface::Building,
			U"A hidden guide sign follows the same visible-depth rule");
	});
	runner.add(U"WorldSelection.VisibleSignalAndEasyGroundPick", [](TestContext& context)
	{
		const BasicCamera3D camera{Size{1280, 768}, 40_deg, Vec3{0, 20, -30}, Vec3{0, 6, 0}};
		const Ray ray = camera.screenToRay(Vec2{640, 384});
		const auto roofHit = Box{Vec3{0, 3, 0}, Vec3{8, 6, 8}}.intersects(ray);
		const Optional<double> roofDistance = roofHit ? Optional<double>{static_cast<double>(*roofHit)} : none;
		const MeshData lamp = MeshData::Box(Float3{0, 0, 0}, Float3{1, 1, 1});
		const auto foregroundSignal = WorldSelection::meshDistance(ray, lamp.vertices, lamp.indices, Mat4x4::Translate(Vec3{0, 7.4, -3}));
		context.expect(roofDistance && foregroundSignal && *foregroundSignal < *roofDistance,
			U"A foreground signal fixture has a genuine earlier mesh hit");
		context.expect(WorldSelection::choose(roofDistance, false, none, true, foregroundSignal) == WorldSelection::Surface::Signal,
			U"A directly hit foreground signal keeps priority over a building behind it");
		context.expect(WorldSelection::choose(none, false, none, true, none) == WorldSelection::Surface::Signal,
			U"Unobstructed ground retains the forgiving ten-meter signal selection");
		context.expect(WorldSelection::choose(none, true, none, true, none) == WorldSelection::Surface::GuideSign,
			U"Unobstructed guide/sign priority remains unchanged");
		context.expect(WorldSelection::choose(roofDistance, false, none, true, none) == WorldSelection::Surface::Building,
			U"An off-ray proximity signal cannot steal a roof click");
		context.expect(WorldSelection::choose(none, false, none, false, none) == WorldSelection::Surface::None,
			U"An empty click still falls through to road and parcel selection");
	});
	runner.add(U"WorldSelection.NativeRayAcrossRasterProfiles", [](TestContext& context)
	{
		const Size native{1280, 768};
		const BasicCamera3D camera{native, 40_deg, Vec3{0, 20, -30}, Vec3{0, 6, 0}};
		const Box building{Vec3{0, 3, 0}, Vec3{8, 6, 8}};
		for (const Vec2 cursor : {Vec2{640, 384}, Vec2{655, 379}})
		{
			const Ray expected = camera.screenToRay(cursor);
			for (const bool lowSpec : {false, true})
			{
				const Size targetSize = RenderQuality::targetSize(native, lowSpec);
				const RenderTexture target{targetSize, TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes};
				const ScopedRenderTarget3D scope{target};
				Graphics3D::SetCameraTransform(camera);
				const Ray actual = camera.screenToRay(cursor);
				context.expectNear(Vec3{actual.getOrigin()}.distanceFrom(Vec3{expected.getOrigin()}), 0, .000001,
					U"Raster target size never changes the native ray origin");
				context.expectNear(Vec3{actual.direction.xyz()}.distanceFrom(Vec3{expected.direction.xyz()}), 0, .000001,
					U"Off-center picking retains native coordinates in both raster profiles");
				const auto expectedHit = building.intersects(expected), actualHit = building.intersects(actual);
				context.expect(expectedHit && actualHit && Abs(*expectedHit - *actualHit) < .0001,
					U"The same building is hit at the same depth under both raster profiles");
			}
		}
	});

	runner.add(U"Input.PaletteBufferedBackspaceRegression", [](TestContext& context)
	{
		GameInput::buffer = KeyboardActionBuffer{};
		GameInput::textInput = nullptr;
		GameInput::textOwnedFrame = false;
		CommandPalette palette;
		palette.open();
		(void)palette.update();
		GameInput::buffer.update({{0,0,KeyBackspace.code(),true,false},
			{1,1,KeyBackspace.code(),false,true}}, true);
		(void)palette.update();
		context.expect(palette.input.isEmpty(), U"A complete buffered Backspace tap deletes the initial slash");
		palette.open();
		(void)palette.update();
		GameInput::buffer.update({{0,0,KeyBackspace.code(),true,false},
			{1,1,KeyBackspace.code(),false,true}}, true);
		(void)palette.update();
		context.expect(palette.input == U"/", U"Retained input history cannot replay palette deletion after reopening");
		palette.close();
		GameInput::buffer = KeyboardActionBuffer{};
		GameInput::textOwnedFrame = false;
	});
	runner.add(U"Input.BufferedEditing.UnicodeAndBounds", [](TestContext& context)
	{
		BufferedTextEdit editor;
		String text = U"/日本🚉駅";
		size_t cursor = editor.update(text, 4, U"", false, {1,false,0}, {});
		context.expect(text == U"/日本駅" && cursor == 3,
			U"Backspace deletes a whole non-BMP Unicode codepoint before the caret");
		cursor = editor.update(text, cursor, U"", false, {}, {1,false,0});
		context.expect(text == U"/日本" && cursor == 3, U"Delete removes the codepoint after the caret");
		cursor = editor.update(text, 99, U"", false, {}, {1,false,0});
		context.expect(text == U"/日本" && cursor == 3, U"A stale caret clamps to the end and Delete is bounded");
		cursor = editor.update(text, 0, U"", false, {4,false,0}, {});
		context.expect(text == U"/日本" && cursor == 0, U"Backspace at the beginning is bounded");
		cursor = editor.update(text, 0, U"", false, {}, {40,false,0});
		context.expect(text.isEmpty() && cursor == 0, U"Repeated Delete cannot cross the end of the string");
	});
	runner.add(U"Input.BufferedEditing.NativeControlsAreAuthoritative", [](TestContext& context)
	{
		BufferedTextEdit editor;
		// These strings are the result already produced by Siv3D's raw text update.
		String text = U"/日本";
		size_t cursor = editor.update(text, 3, U"\b", false, {1,false,0}, {});
		context.expect(text == U"/日本" && cursor == 3, U"Native plus buffered Backspace cannot delete twice");
		cursor = editor.update(text, 1, U"\x7F", false, {}, {1,false,0});
		context.expect(text == U"/日本" && cursor == 1, U"Native plus buffered Delete cannot delete twice");
		cursor = editor.update(text, 3, U"\b\b\b", false, {0,true,.5}, {});
		context.expect(text == U"/日本" && cursor == 3, U"Native repeated Backspaces do not receive an extra held-key edit");
		cursor = editor.update(text, cursor, U"", false, {0,true,.7}, {});
		context.expect(text == U"/日本" && cursor == 3,
			U"Fallback does not add repeats between native repeat events during the same hold");
		cursor = editor.update(text, 3, U"\x7F", false, {1,false,0}, {1,false,0});
		context.expect(text == U"/日" && cursor == 2, U"A native Delete does not suppress a missing Backspace");
	});
	runner.add(U"Input.BufferedEditing.PressCountsAndNoReplay", [](TestContext& context)
	{
		KeyboardActionBuffer buffer;
		const Array<KeyEvent> events{{0,0,KeyBackspace.code(),true,false},
			{1,1,KeyBackspace.code(),false,true},{2,2,KeyBackspace.code(),true,false},
			{3,3,KeyBackspace.code(),false,true},{4,4,KeyDelete.code(),true,false},
			{5,5,KeyDelete.code(),false,true},{6,6,KeyR.code(),true,false}};
		buffer.update(events, true);
		context.expect(buffer.down(KeyR.code()) && buffer.editPressCount(KeyR.code()) == 0,
			U"Gameplay key actions retain their previous boolean semantics");
		BufferedTextEdit editor;
		String text = U"/日本駅前";
		size_t cursor = editor.update(text, 3, U"", false,
			{buffer.editPressCount(KeyBackspace.code()),false,0},
			{buffer.editPressCount(KeyDelete.code()),false,0});
		context.expect(text == U"/前" && cursor == 1, U"All short edit taps received between two frames are preserved");
		buffer.update(events, true);
		cursor = editor.update(text, cursor, U"", false,
			{buffer.editPressCount(KeyBackspace.code()),false,0},
			{buffer.editPressCount(KeyDelete.code()),false,0});
		context.expect(text == U"/前" && cursor == 1, U"Retained key events cannot replay editing");
		const Array<KeyEvent> unfocused{{7,7,KeyBackspace.code(),true,false}};
		buffer.update(unfocused, false);
		buffer.update(unfocused, true);
		context.expect(buffer.editPressCount(KeyBackspace.code()) == 0,
			U"Edit presses received without focus cannot replay after focus returns");
	});
	runner.add(U"Input.BufferedEditing.ImeAndHeldRepeat", [](TestContext& context)
	{
		BufferedTextEdit editor;
		String text = U"/日本駅前通り";
		size_t cursor = editor.update(text, text.size(), U"", false, {1,true,0}, {});
		context.expect(text == U"/日本駅前通", U"A held Backspace starts with one edit");
		cursor = editor.update(text, cursor, U"", false, {0,true,.32}, {});
		context.expect(text == U"/日本駅前通", U"Held fallback respects its initial repeat delay");
		cursor = editor.update(text, cursor, U"", false, {0,true,.34}, {});
		context.expect(text == U"/日本駅前", U"Held Backspace repeats when native controls are absent");
		cursor = editor.update(text, cursor, U"", false, {0,true,.36}, {});
		context.expect(text == U"/日本駅前", U"Held fallback respects its repeat interval");
		cursor = editor.update(text, cursor, U"", false, {0,true,.41}, {});
		context.expect(text == U"/日本駅", U"Held Backspace continues repeating");
		cursor = editor.update(text, cursor, U"", true, {1,true,.5}, {1,true,.5});
		context.expect(text == U"/日本駅", U"IME composition owns both edit keys");
		cursor = editor.update(text, cursor, U"", false, {0,true,.8}, {0,true,.8});
		context.expect(text == U"/日本駅", U"An IME-owned held key cannot delete committed text before release");
		cursor = editor.update(text, cursor, U"", false, {}, {});
		cursor = editor.update(text, 1, U"", false, {}, {1,true,0});
		context.expect(text == U"/本駅" && cursor == 1, U"Fresh Delete works after IME-owned keys are released");
		cursor = editor.update(text, cursor, U"", false, {}, {0,true,.34});
		context.expect(text == U"/駅" && cursor == 1, U"Held Delete also repeats when native controls are absent");
	});
	runner.add(U"Clock.TwentyFourMinuteDayAndCalendarBoundaries",[](TestContext& context)
	{
		GameClock clock;
		clock.advance(60);
		context.expectNear(clock.hour,9,.0001,U"One real minute advances one calendar hour");
		clock.advance(23*60);
		context.expect(clock.day==2 && clock.hour==8,U"24 real minutes advance exactly one day");
		context.expectNear(clock.now,1440,.0001,U"Vehicle simulation still uses elapsed seconds");
		for (const auto speed : {TimeSpeed::x2,TimeSpeed::x4})
		{
			GameClock fast;fast.speed=speed;
			fast.advance(1440/fast.speedMultiplier());
			context.expect(fast.day==2 && fast.hour==8,U"Accelerated clocks preserve the same day length ratio");
		}
		clock.speed=TimeSpeed::Paused;clock.advance(1440);
		context.expect(clock.day==2 && clock.hour==8,U"Pause freezes calendar and simulation time");
		clock.speed=TimeSpeed::x1;
		clock.advance(28*1440+16*60-1);
		context.expect(clock.month==4 && clock.day==30 && clock.monthIndex()==3,U"Month processing cannot fire before midnight");
		clock.advance(1);
		context.expect(clock.month==5 && clock.day==1 && clock.hour==0 && clock.monthIndex()==4,U"Month changes once at the 30-day boundary");
		context.expectNear(GameClock::TimeFromMonthIndex(4),clock.now,.0001,U"Construction and economy share the calendar boundary");
	});
	runner.add(U"Input.TextFocusMustBlockAllActions",[](TestContext& context)
	{
		TextEditState text;
		PanelWidget::activeTextInput = &text;
		GameInput::buffer.update({{0,7000,KeyW.code(),true,false},{1,7001,KeyR.code(),true,false}},true);
		const bool leakedMovement = GameInput::pressed(KeyW);
		const bool leakedTool = GameInput::down(KeyR);
		TextWriter{U"TestResults/text_focus.txt"}.write(U"typing=true movement={} roadTool={}"_fmt(leakedMovement,leakedTool));
		context.expect(!leakedMovement && !leakedTool,U"Text focus exclusively owns keyboard actions, including buffered taps");
		GameInput::releaseTextFocus();
		context.expect(!GameInput::pressed(KeyW),U"Finishing text entry cannot replay the same frame into movement");
		GameInput::textOwnedFrame = false;
		GameInput::buffer = KeyboardActionBuffer{};
	});
	runner.add(U"Input.SaveShortcutHonorsFocus", [](TestContext& context)
	{
		GameInput::buffer = KeyboardActionBuffer{};
		GameInput::textInput = nullptr;
		GameInput::textOwnedFrame = false;
		const Array<KeyEvent> chord{{0,0,KeyControl.code(),true,false},
			{0,1,KeyShift.code(),true,false},{0,2,KeyS.code(),true,false}};
		GameInput::buffer.update(chord,true);
		context.expect(GameInput::saveShortcutActive(false),U"A buffered save chord is available independently of the active map or camera view");
		context.expect(!GameInput::saveShortcutActive(true),U"The pause menu retains its own save action");
		TextEditState text;
		GameInput::textInput = &text;
		context.expect(!GameInput::saveShortcutActive(false),U"Typing in a field cannot invoke global save");
		GameInput::releaseTextFocus();
		context.expect(!GameInput::saveShortcutActive(false),U"Closing text input cannot replay its keys as a save");
		GameInput::textOwnedFrame = false;
		GameInput::buffer.update(chord,true);
		context.expect(!GameInput::saveShortcutActive(false),U"Retained input history cannot reactivate a released save chord");
		GameInput::buffer = KeyboardActionBuffer{};
		GameInput::buffer.update({{0,0,KeyS.code(),true,false}},true);
		context.expect(!GameInput::saveShortcutActive(false),U"S without modifiers cannot save");
		GameInput::buffer = KeyboardActionBuffer{};
	});
	runner.add(U"Input.ShortTapsAndFocus",[](TestContext& context)
	{
		KeyboardActionBuffer buffer;
		const Array<KeyEvent> events{{100,0,KeyM.code(),true,false},{102,1,KeyM.code(),false,true}};
		buffer.update(events,true);
		context.expect(buffer.down(KeyM.code()),U"A complete tap between rendered frames is preserved");
		buffer.update(events,true);
		context.expect(!buffer.down(KeyM.code()),U"The engine's retained event history cannot replay an action");
		buffer.update({{104,2,KeyR.code(),true,false}},false);
		buffer.update({{104,2,KeyR.code(),true,false}},true);
		context.expect(!buffer.down(KeyR.code()),U"Events received without focus do not activate tools on return");
		buffer.update({{105,3,KeyControl.code(),true,false},{106,4,KeyZ.code(),true,false}},true);
		context.expect(buffer.down(KeyControl.code()) && buffer.down(KeyZ.code()),U"Fast modifier chords preserve both keys");
	});
	runner.add(U"Camera.ModalFocusAndWalkingPace",[](TestContext& context)
	{
		World world;world.reserveChunks();world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		world.installChunkDirect({0,0},HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,20),20,20});
		GameCamera camera;camera.setWalkingState({500,20,500},0);
		GameInput::buffer.update({{0,1000,KeyW.code(),true,false}},true);
		camera.setKeyboardBlocked(true);camera.update(.1,world);
		context.expectNear(camera.focusPoint().z,500,1e-6,U"Typing W in a panel or a pause menu cannot move the player");
		camera.setKeyboardBlocked(false);camera.update(.1,world);
		context.expectNear(camera.focusPoint().z,500.15,.001,U"Normal movement is 1.5 metres per second");
		const Vec3 before=camera.focusPoint();camera.update(10,world);
		context.expectNear(camera.focusPoint().distanceFrom(before),.15,.001,U"A stalled frame cannot teleport the walking camera");
		camera.cycleMode();camera.setKeyboardBlocked(true);const Vec3 overview=camera.focusPoint();camera.update(.1,world);
		context.expectNear(camera.focusPoint().distanceFrom(overview),0,.001,U"Overview also respects keyboard focus");
		context.expectNear(GameCamera::walkingSpeed(true,false),4.5,.001,U"Shift runs");
		context.expectNear(GameCamera::walkingSpeed(false,true),15,.001,U"Ctrl speeds up exploration");
		context.expectNear(GameCamera::walkingSpeed(true,true),15,.001,U"Boost modifiers do not compound into extreme speeds");
		GameInput::buffer=KeyboardActionBuffer{};
	});
	runner.add(U"Camera.ImmediateModeChangesAndMapJump", [](TestContext& context)
	{
		GameCamera camera;
		camera.setState({500, 20, 500}, 600, 0, static_cast<float>(40_deg));
		camera.cycleMode();
		context.expect(camera.mode() == CameraMode::FirstPerson, U"F enters walking mode");
		context.expectNear(camera.eyePosition().y, 21.5, .001,
			U"The first walking frame already uses eye height, without an overview flash");
		camera.setFocus({800, 30, 900});
		context.expect(camera.mode() == CameraMode::FirstPerson, U"Map jumps preserve walking mode");
		context.expectNear(camera.eyePosition().distanceFrom(Vec3{800, 31.5, 900}), 0, .001,
			U"The map destination is rendered immediately with the walking camera");
		camera.cycleMode();
		context.expect(camera.eyePosition().y > 300, U"The first overview frame already uses overview distance");
		camera.setState({600, 40, 700}, 600, 0, static_cast<float>(40_deg));
		context.expectNear(camera.focusPoint().distanceFrom(Vec3{600, 40, 700}), 0, .001,
			U"Saved overview state remains supported");
	});
	runner.add(U"Rendering.IncrementalRoadCaches", [](TestContext& context)
	{
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
		world.installChunkDirect({0, 0}, HeightMapResult{Grid<float>(HEIGHT_CELLS + 1, HEIGHT_CELLS + 1, 20), 20, 20});
		RoadNetwork roads;
		Grid<int> nodes{4, 4};
		for (int z = 0; z < 4; ++z)
		{
			for (int x = 0; x < 4; ++x) { nodes[z][x] = roads.addNode({360.0 + x * 100, 20, 360.0 + z * 100}); }
		}
		const auto connect = [&](int first, int last)
		{
			const Vec3 a = roads.getNode(first)->position, b = roads.getNode(last)->position;
			const int id = *roads.addEdge(first, last, a.lerp(b, 1.0 / 3), a.lerp(b, 2.0 / 3), RoadType::Arterial, 2);
			roads.getEdge(id)->edgeState = EdgeState::Open;
		};
		for (int z = 0; z < 4; ++z)
		{
			for (int x = 0; x < 4; ++x)
			{
				if (x < 3) { connect(nodes[z][x], nodes[z][x + 1]); }
				if (z < 3) { connect(nodes[z][x], nodes[z + 1][x]); }
			}
		}
		RoadRenderer reference, incremental;
		context.expect(reference.loadAssets() && incremental.loadAssets(), U"Use the same production road assets");
		incremental.setCacheBuildBudget(.01);
		const Size size{640, 480};
		const BasicCamera3D camera{size, 50_deg, Vec3{510, 470, 210}, Vec3{510, 20, 510}};
		const ViewFrustum frustum{camera, 9000};
		const ColorF background{.05, .1, .15};
		const RenderTexture target{size, TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes};
		const auto draw = [&](RoadRenderer& renderer)
		{
			{
				const ScopedRenderTarget3D rt{target.clear(background)};
				const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
				Graphics3D::SetCameraTransform(camera);
				Graphics3D::SetGlobalAmbientColor(ColorF{.5});
				renderer.render(roads, world, frustum, camera.getEyePosition());
			}
			Graphics3D::Flush();
		};
		draw(reference);
		Image expected;
		target.readAsImage(expected);
		draw(incremental);
		context.expect(incremental.cacheBuildStats().deferred > 0, U"Cold geometry is distributed across frames");
		context.expectEqual(incremental.visibleEdges().size(), reference.visibleEdges().size(), U"Deferred roads remain visible for picking and traffic");
		Image first;
		target.readAsImage(first);
		first.save(directory + U"Screenshot/road_streaming_first.png");
		int missingRoadPixels = 0, roadPixels = 0;
		const Color clear = expected[0][0];
		for (int y = 0; y < size.y; ++y)
		{
			for (int x = 0; x < size.x; ++x)
			{
				if (expected[y][x] == clear) { continue; }
				++roadPixels;
				missingRoadPixels += first[y][x] == clear;
			}
		}
		context.expect(roadPixels > 1000 && missingRoadPixels < roadPixels * .04, U"Fallback surfaces retain at least 96 percent of the road silhouette");
		int frames = 1;
		while (incremental.cacheBuildStats().deferred > 0 && frames < 300) { draw(incremental); ++frames; }
		context.expect(frames < 300, U"Visible work converges without starvation, even with a very small budget");
		Image actual;
		target.readAsImage(actual);
		int differences = 0;
		for (int y = 0; y < size.y; ++y)
		{
			for (int x = 0; x < size.x; ++x) { differences += expected[y][x] != actual[y][x]; }
		}
		context.expectEqual(differences, 0, U"Finished incremental geometry is pixel-identical to the synchronous renderer");
		actual.save(directory + U"Screenshot/road_streaming_complete.png");
		incremental.invalidateAllCaches();
		draw(incremental);
		context.expect(incremental.cacheBuildStats().deferred > 0, U"Invalidation restarts bounded work instead of retaining stale caches");
		TextWriter{directory + U"TestResults/road_streaming.txt"}.write(
			U"frames={} roadPixels={} missingRoadPixels={} finalDifferences={}"_fmt(frames, roadPixels, missingRoadPixels, differences));
	});
	runner.add(U"Rendering.SignalDistanceAndCost", [](TestContext& context)
	{
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
		world.installChunkDirect({0, 0}, HeightMapResult{Grid<float>(HEIGHT_CELLS + 1, HEIGHT_CELLS + 1, 20), 20, 20});
		RoadNetwork roads;
		const Vec3 focus{512, 20, 512};
		const int center = roads.addNode(focus);
		for (const Vec3 delta : {Vec3{-80, 0, 0}, Vec3{80, 0, 0}, Vec3{0, 0, -80}, Vec3{0, 0, 80}})
		{
			const int end = roads.addNode(focus + delta);
			roads.addEdge(center, end, focus + delta / 3, focus + delta * 2 / 3, RoadType::Arterial, 4);
		}
		roads.getNode(center)->signalPlacement = SignalPlacement{U"signal_3lamp"};
		for (auto& attachment : roads.getNode(center)->attachments) { attachment.control = TrafficControl::Signal; }
		const SimGraph graph = SimGraph::build(roads);
		RoadRenderer renderer;
		context.expect(renderer.loadAssets(), U"Signal test uses the production assets");
		const Size size{640, 480};
		const BasicCamera3D camera{size, 40_deg, focus + Vec3{45, 30, -65}, focus + Vec3{0, 4, 0}};
		const RenderTexture target{size, TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes};
		Array<Image> images;
		for (const double distance : {0.0, 499.9, 500.0, 500.01})
		{
			{
				const ScopedRenderTarget3D rt{target.clear(ColorF{.05, .1, .15})};
				const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
				Graphics3D::SetCameraTransform(camera);
				Graphics3D::SetGlobalAmbientColor(ColorF{.5});
				renderer.drawSignals(roads, graph, world, {}, 0, focus + Vec3{distance, 0, 0});
			}
			Graphics3D::Flush();
			Image image;
			target.readAsImage(image);
			images << std::move(image);
		}
		int boundaryDifferences = 0, visiblePixels = 0;
		for (int y = 0; y < size.y; ++y)
		{
			for (int x = 0; x < size.x; ++x)
			{
				boundaryDifferences += images[1][y][x] != images[2][y][x];
				visiblePixels += images[2][y][x] != images[3][y][x];
			}
		}
		context.expectEqual(boundaryDifferences, 0, U"The same far LOD remains visible at the inclusive 500m cutoff");
		context.expect(visiblePixels > 50, U"Nearby signals are rendered and distant signals are culled");
		images[0].save(directory + U"Screenshot/signals_playability.png");
		RoadNetwork distant;
		constexpr int kDistantSignals = 20000;
		for (int index = 0; index < kDistantSignals; ++index)
		{
			const int id = distant.addNode({2000.0 + index, 20, 2000});
			distant.getNode(id)->signalPlacement = SignalPlacement{U"signal_3lamp"};
		}
		Array<double> samples;
		constexpr int kSamples = 120;
		for (int sample = 0; sample < kSamples; ++sample)
		{
			const Stopwatch timer{StartImmediately::Yes};
			renderer.drawSignals(distant, graph, world, {}, 0, focus);
			samples << timer.msF();
		}
		samples.sort();
		TextWriter{directory + U"TestResults/signal_cost.txt"}.write(
			U"signals={} samples={} p50Ms={:.4f} p95Ms={:.4f} visiblePixels={} boundaryDifferences={}"_fmt(
				kDistantSignals, kSamples, samples[kSamples / 2], samples[kSamples * 95 / 100], visiblePixels, boundaryDifferences));
	});

	runner.add(U"Economy.GeneratedPopulationAndMaintenance",[](TestContext& context)
	{
		Economy economy;economy.initializeGeneratedCity(786838);
		context.expectEqual(economy.population,566523,U"Population is derived from the generated housing supply");
		context.expect(economy.monthlyGrant()>36.26,U"The measured seed-42 region can maintain its existing roads");
		RoadNetwork roads;const int a=roads.addNode({0,0,0}),b=roads.addNode({1000,0,0});
		const int edge=*roads.addEdge(a,b,{333,0,0},{667,0,0},RoadType::LocalRoad,2);
		roads.getEdge(edge)->edgeState=EdgeState::Planned;
		context.expectNear(economy.roadMaintenanceCost(roads),0,1e-8,U"Unbuilt plans do not incur road maintenance");
		roads.getEdge(edge)->edgeState=EdgeState::Open;
		context.expect(economy.roadMaintenanceCost(roads)>0,U"Open roads remain an ongoing expense");
		economy.initializeGeneratedCity(0);
		context.expectEqual(economy.population,0,U"An empty sandbox does not invent residents");
	});
	runner.add(U"Map.CacheAndResume",[](TestContext& context)
	{
		WorldMapView map;const Size size{1000,600};const Font font{16};map.open({32768,32768});map.zoom=128;
		const WorldMapView::Stroke road{{{32000,32768},{33500,32768}},RectF{32000,32758,1500,20},10,1};
		map.streets<<road;
		const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm};
		const auto draw=[&]()
		{
			{const ScopedRenderTarget2D rt{target};map.draw(size,Texture{},font,{0,0});}Graphics2D::Flush();
			Image image;target.readAsImage(image);return image;
		};
		const Image first=draw();const uint64 builds=map.cartographyBuilds();
		const Image reused=draw();
		context.expectEqual(map.cartographyBuilds(),builds,U"An unchanged map reuses its cartography");
		int differences=0;for(int y=70;y<500;++y)for(int x=10;x<940;++x){differences+=first[y][x]!=reused[y][x];}
		context.expectEqual(differences,0,U"Cache reuse preserves every map pixel");
		map.streets.clear();map.invalidateCartography();const Image removed=draw();
		const Point sample=map.toScreen({33000,32768},size).asPoint();
		context.expect(first[sample]!=removed[sample],U"Changed roads appear immediately after invalidation");
		map.pan({23,41},size);const Vec2 center=map.center;const double zoom=map.zoom;map.close();map.open({10000,10000});
		context.expectNear(map.center.distanceFrom(center),0,.001,U"Reopening resumes the explored area");
		context.expectNear(map.zoom,zoom,.001,U"Reopening keeps the map scale");
		map.streets<<road;map.invalidateCartography();draw().save(U"Screenshot/map_playability.png");
		map.showContext({100, 100}, size);
		GameInput::buffer.update({{0, 2000, KeyHome.code(), true, false}}, true);
		map.update(size, Vec2{10000, 12000});
		context.expectNear(map.center.distanceFrom(Vec2{10000, 12000}), 0, .001,
			U"Home finds the player after exploring a different part of the map");
		context.expectNear(map.zoom, zoom, .001, U"Returning to the player preserves zoom");
		context.expect(!map.contextWorld, U"Recentering dismisses the old jump menu");
		GameInput::buffer = KeyboardActionBuffer{};
	});
	runner.add(U"UI.PauseMenuAndControlHelp",[](TestContext& context)
	{
		RegisterAssets();const Font& font=FontAsset(Asset::CJK14);const Size size{1280,768};PauseMenu menu;
		for (int index=0;index<PauseMenu::kActionCount;++index)
		{
			const auto button=PauseMenu::button(size,index);const auto panel=PauseMenu::panel(size);
			context.expect(panel.contains(button.pos) && panel.contains(button.br()),U"Every menu action fits inside its panel");
		}
		const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm};
		{const ScopedRenderTarget2D rt{target.clear(ColorF{.4,.5,.3})};NavigationHelp::draw(font,size);menu.draw(font,size);}Graphics2D::Flush();
		Image image;target.readAsImage(image);image.save(U"Screenshot/pause_menu_playability.png");
		const auto last=PauseMenu::button(size,PauseMenu::kActionCount-1);
		context.expect(image[last.center().asPoint()].r>20,U"The last action is actually drawn");
		GameInput::buffer.update({{0,1000,KeyDown.code(),true,false}},true);
		{const ScopedRenderTarget2D rt{target};menu.draw(font,size);}
		context.expectEqual(menu.selected,1,U"Down selects Save");
		GameInput::buffer.update({{0,1001,KeyEnter.code(),true,false}},true);
		Optional<PauseMenu::Action> action;{const ScopedRenderTarget2D rt{target};action=menu.draw(font,size);}
		context.expect(action==PauseMenu::Action::Save,U"Enter invokes the selected menu action");
		GameInput::buffer=KeyboardActionBuffer{};
		Graphics2D::Flush();
		for (const bool walking : {false, true})
		{
			const Color background{40, 50, 60};
			{ const ScopedRenderTarget2D rt{target.clear(background)}; NavigationHelp::draw(font, size, walking); }
			Graphics2D::Flush();
			Image help;
			target.readAsImage(help);
			int overflow = 0, textPixels = 0;
			for (int y = 0; y < size.y; ++y)
			{
				for (int x = 0; x < size.x; ++x)
				{
					const bool inside = NavigationHelp::bounds(size).stretched(2).contains(Point{x, y});
					overflow += !inside && help[y][x] != background;
					textPixels += inside && help[y][x].r > 180;
				}
			}
			context.expectEqual(overflow, 0, U"Both modes keep the control hints inside the panel");
			context.expect(textPixels > 200, U"Control hints actually draw readable text pixels");
			help.save(walking ? U"Screenshot/help_walking.png" : U"Screenshot/help_overview.png");
		}
	});
}
