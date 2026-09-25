#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/world/World.hpp"
#include "src/gen/SettlementPlacement.hpp"
#include "src/gen/SettlementNames.hpp"
#include "src/gen/VillageConnections.hpp"
#include "src/ui/StartScreenControls.hpp"
#include "src/ui/RailInfoPanel.hpp"
#include "src/ui/FrameRateGraph.hpp"
#include "src/save/SaveCatalog.hpp"
#include "src/render/SubsurfaceView.hpp"
#include "src/gen/SettlementDevelopment.hpp"
#include "src/gen/StreetProfile.hpp"
#include "src/gen/AgriculturalLayout.hpp"
#include "src/road/RoadGeometry.hpp"
#include "src/asset/AssetRegistrar.hpp"

void registerGenerationRevisionTests(TestRunner& runner)
{
	runner.add(U"GenerationRevision.Benchmark", [](TestContext& context)
	{
		World world;
		const Stopwatch timer{StartImmediately::Yes};
		world.setGenerationParams(42, WORLD_SIZE, WORLD_SIZE);
		const double setup = timer.msF();
		double checksum = 0;
		for (int z = 0; z < 16; ++z)
		{
			for (int x = 0; x < 16; ++x)
			{
				const auto result = world.buildHeightMap({x + 24, z + 24});
				checksum += result.heightMin + result.heightMax;
			}
		}
		JSON report;
		report[U"setupMs"] = setup;
		report[U"terrainMs"] = timer.msF() - setup;
		report[U"checksum"] = checksum;
		int plain = 0;
		for (int z = 0; z < 256; ++z)
		{
			for (int x = 0; x < 256; ++x)
			{
				const auto biome = world.getBiome((x + .5f) * 256, (z + .5f) * 256);
				plain += biome == BiomeType::Basin || biome == BiomeType::Plain || biome == BiomeType::CoastalPlain;
			}
		}
		report[U"plainFraction"] = plain / 65536.0;
		Array<SettlementPlacement::Candidate> candidates;
		for (int z = 2000; z < 64000; z += 1200)
		{
			for (int x = 2000; x < 64000; x += 1200)
			{
				candidates << SettlementPlacement::Candidate{{x, z}, 1};
			}
		}
		const Stopwatch placement{StartImmediately::Yes};
		const auto towns =
			SettlementPlacement::generate(42, candidates, {0, 0, WORLD_SIZE, WORLD_SIZE}, [](Vec2) { return 30.0; });
		int cities = 0;
		for (const auto& town : towns)
		{
			cities += town.kind == MapGenerator::SettlementKind::RegionalCity;
		}
		report[U"placementMs"] = placement.msF();
		report[U"cities"] = cities;
		report[U"settlements"] = towns.size();
		report.save(U"TestResults/generation_revision_benchmark.json");
		context.expect(
			std::isfinite(checksum) && !towns.isEmpty(), U"Benchmark executes terrain and settlement generation");
	});
	runner.add(U"GenerationRevision.StartAndRailUi", [](TestContext& context)
	{
		RegisterAssets();
		const auto font = FontAsset(Asset::Small16);
		GenerationOptions options;
		const Vec2 origin{24, 40};
		StartScreenControls::selectOption(origin, options, StartScreenControls::optionBounds(origin, 1).center(), true);
		context.expect(!options.enabled(GenerationOptions::Element::Roads) &&
						   !options.enabled(GenerationOptions::Element::Railway),
			U"町を外すと依存する道路と鉄道も無効");
		context.expect(
			options.enabled(GenerationOptions::Element::Rivers) && options.enabled(GenerationOptions::Element::Trees),
			U"自然は独立して選べる");
		const auto restored = GenerationOptions::load(options.save());
		context.expect(restored.selected == options.selected, U"生成要素の選択を保存・復元");
		StartScreenControls::SaveList saves;
		saves.names = {U"新しい街", U"非常に長いセーブデータの名前を使用した市街地"};
		saves.selected = 1;
		const Vec2 saveOrigin{424, 40};
		StartScreenControls::interactSaves(
			saveOrigin, saves, StartScreenControls::deleteButton(saveOrigin).center(), true);
		context.expect(saves.confirming == saves.names[1], U"削除の確認は一覧の番号ではなく名前を保持");
		context.expect(
			StartScreenControls::interactSaves(saveOrigin, saves, StartScreenControls::loadButton(saveOrigin).center(),
				true) == StartScreenControls::Action::None,
			U"削除確認中のクリックは背後へ流さない");
		TrainNetwork rails;
		Train vehicle;
		vehicle.id = 4;
		vehicle.speed = 12;
		vehicle.passengerCount = 68;
		const auto summary = RailInfoPanel::train(vehicle, rails);
		context.expect(summary.canFollow && summary.lines.size() >= 4, U"列車情報と追跡操作がある");
		for (const auto label : GenerationOptions::labels)
		{
			font.preload(label);
		}
		for (const auto& line : summary.lines)
		{
			font.preload(line);
		}
		for (const auto& name : saves.names)
		{
			font.preload(name);
		}
		font.preload(U"ロード削除するキャンセルをしますか追跡やめるダイヤ開く地下駅");
		for (int i = 0; i < 3; ++i)
		{
			System::Update();
		}
		RenderTexture target{800, 600, ColorF{.04, .06, .09}};
		{
			const ScopedRenderTarget2D render{target};
			StartScreenControls::drawOptions(origin, options, font);
			StartScreenControls::drawSaves(saveOrigin, saves, font);
			StartScreenControls::drawLayerButton({800, 600}, true, font);
			const Transformer2D transform{Mat3x2::Translate(24, 260)};
			RailInfoPanel::draw(summary, true, font);
			Graphics2D::Flush();
		}
		Image pixels;
		target.readAsImage(pixels);
		pixels.save(U"Screenshot/generation_controls_review.png");
		int text = 0;
		for (const auto& pixel : pixels)
		{
			if (pixel.r > 160 && pixel.g > 160 && pixel.b > 160)
			{
				++text;
			}
		}
		context.expect(text > 300, U"チェックボックス・保存確認・列車ウィンドウをGPUで描画");
		for (size_t i = 0; i < GenerationOptions::kCount; ++i)
		{
			const auto rect = StartScreenControls::optionBounds(origin, i);
			context.expect(rect.br().x < 400 && rect.br().y < 250, U"選択肢が左側に収まる");
		}
		JSON report;
		report[U"textPixels"] = text;
		report.save(U"TestResults/generation_ui_review.json");
	});
	runner.add(U"GenerationRevision.PlainLimitAndCityCap", [](TestContext& context)
	{
		JSON report;
		for (uint64 seed : {0ULL, 1ULL, 2ULL, 7ULL, 42ULL, 130ULL, 2026ULL})
		{
			World world;
			world.setGenerationParams(seed, WORLD_SIZE, WORLD_SIZE);
			int plain = 0;
			for (int z = 0; z < 256; ++z)
			{
				for (int x = 0; x < 256; ++x)
				{
					const auto biome = world.getBiome((x + .37f) * 256, (z + .61f) * 256);
					plain += biome == BiomeType::Basin || biome == BiomeType::Plain || biome == BiomeType::CoastalPlain;
				}
			}
			const double fraction = plain / 65536.0;
			report[Format(seed)] = fraction;
			context.expect(fraction <= .5, U"平野がマップ半分以下 seed {} fraction {}"_fmt(seed, fraction));
		}
		report.save(U"TestResults/plain_limits.json");
		Array<SettlementPlacement::Candidate> candidates;
		for (int z = 2000; z < 64000; z += 1600)
		{
			for (int x = 2000; x < 64000; x += 1600)
			{
				candidates << SettlementPlacement::Candidate{{x, z}, 1};
			}
		}
		for (uint64 seed : {1ULL, 42ULL, 2026ULL})
		{
			const auto towns = SettlementPlacement::generate(
				seed, candidates, {0, 0, WORLD_SIZE, WORLD_SIZE}, [](Vec2) { return 30.0; });
			int cities = 0;
			for (const auto& town : towns)
			{
				cities += town.kind == MapGenerator::SettlementKind::RegionalCity;
			}
			context.expect(cities <= 4 && cities > 0, U"広大な適地があっても都市は4つ以下");
		}
		MapGenerator::Settlement place;
		place.name = U"山里";
		place.reading = U"yamazato";
		context.expect(SettlementNames::reading(place) == U"Yamazato Vill.", U"村の英語接尾辞");
		place.kind = MapGenerator::SettlementKind::LocalTown;
		context.expect(SettlementNames::reading(place) == U"Yamazato Towm", U"町は指定されたTowm表記");
	});
	runner.add(U"GenerationRevision.CapitalizedPlaceReadings", [](TestContext& context)
	{
		PlaceNameGenerator generator;
		context.expect(generator.load(U"../../App/assets/placenames/placenames.toml"), U"Place name dictionary loads");
		const auto basic=generator.generate(8,42);
		const auto biomes=generator.generateWithBiomes(8,Array<BiomeType>(8,BiomeType::Plain),42);
		for (int i=0;i<8;++i)
		{
			const String a=basic.settlementReading(i),b=biomes.settlementReading(i);
			context.expect(!a.isEmpty() && a[0]>=U'A' && a[0]<=U'Z' &&
				!b.isEmpty() && b[0]>=U'A' && b[0]<=U'Z', U"Generated romanized names start with a capital");
		}
	});
	runner.add(U"GenerationRevision.VillageShortcuts", [](TestContext& context)
	{
		World world;
		world.reserveChunks();
		for (int z = 30; z <= 34; ++z)
		{
			for (int x = 30; x <= 34; ++x)
			{
				world.installChunkDirect({x, z}, {Grid<float>(HEIGHT_CELLS + 1, HEIGHT_CELLS + 1, 30), 30, 30});
			}
		}
		RoadNetwork roads;
		const int a = roads.addNode({32000, 30, 32000}), b = roads.addNode({32500, 30, 32000}),
				  c = roads.addNode({32000, 30, 33500}), d = roads.addNode({32500, 30, 33500});
		const auto join = [&](int from, int to)
		{
			const Vec3 p = roads.getNode(from)->position, q = roads.getNode(to)->position;
			roads.addEdge(from, to, p + (q - p) / 3, p + (q - p) * 2 / 3, RoadType::LocalRoad, 2);
		};
		join(a, c);
		join(c, d);
		join(d, b);
		context.expectNear(
			VillageConnections::shortestDistance(roads, a, b), 3500, 1, U"道路の最短距離は直線ではなく実長");
		Array<MapGenerator::Settlement> villages(2);
		villages[0].center = {32000, 32000};
		villages[1].center = {32500, 32000};
		const auto result = VillageConnections::improve(42, villages, world, roads);
		context.expect(
			result.trials >= 12 && result.trials <= 19 && result.connected > 0, U"十数回の試行で3倍以上の迂回を接続");
		context.expect(VillageConnections::shortestDistance(roads, a, b) < 750, U"追加道路は実際のネットワークを短絡");
		const auto again = VillageConnections::improve(42, villages, world, roads);
		context.expect(again.connected == 0, U"既に短い経路には重複道路を作らない");
	});

	runner.add(U"GenerationRevision.SaveDelete", [](TestContext& context)
	{
		const FilePath root = U"TestResults/delete_fixture";
		FileSystem::CreateDirectories(root + U"/街1");
		FileSystem::CreateDirectories(root + U"/街2");
		JSON meta;
		meta[U"seed"] = 42;
		meta.save(root + U"/街1/meta.json");
		meta.save(root + U"/街2/meta.json");
		context.expect(SaveCatalog::list(root).size() == 2, U"完成したセーブだけを列挙");
		context.expect(!SaveCatalog::remove(root, U"../街1").success && !SaveCatalog::remove(root, U".").success,
			U"親・ルート・パス入りの削除を拒否");
		context.expect(SaveCatalog::remove(root, U"街1").success, U"指定したセーブを削除");
		context.expect(SaveCatalog::list(root) == Array<String>{U"街2"}, U"隣のセーブを残す");
		context.expect(!SaveCatalog::remove(root, U"街1").success, U"既に無い保存の削除失敗を返す");
		context.expect(SaveCatalog::remove(root, U"街2").success, U"テスト保存を片付ける");
	});

	runner.add(U"GenerationRevision.UndergroundGeometry", [](TestContext& context)
	{
		World world;
		world.reserveChunks();
		for (int z = 30; z <= 33; ++z)
		{
			for (int x = 30; x <= 33; ++x)
			{
				world.installChunkDirect({x, z}, {Grid<float>(HEIGHT_CELLS + 1, HEIGHT_CELLS + 1, 30), 30, 30});
			}
		}
		RoadNetwork roads;
		const auto addRoad = [&](double height)
		{
			const Vec3 from{32500, height, 32600}, to{32900, height, 32600};
			const int a = roads.addNode(from), b = roads.addNode(to);
			const auto id =
				roads.addEdge(a, b, from + (to - from) / 3, from + (to - from) * 2 / 3, RoadType::LocalRoad, 2);
			auto* edge = roads.getEdge(*id);
			edge->edgeState = EdgeState::Existing;
			edge->useElevation = height < 30;
			edge->tunnel = height < 30;
			return *id;
		};
		const int surface = addRoad(30), tunnel = addRoad(10);
		TrainNetwork railway;
		railway.bind(&roads);
		const int station = railway.addStation({32700, 10, 32700}, U"谷中");
		railway.getNode(station)->stationKind = StationKind::Underground;
		railway.getNode(station)->entrance = Vec3{32712, 30, 32708};
		const int before = railway.addNode({32400, 10, 32700}), after = railway.addNode({33000, 10, 32700});
		for (const auto pair : {std::pair<int, int>{before, station}, {station, after}})
		{
			const Vec3 a = railway.getNode(pair.first)->position, b = railway.getNode(pair.second)->position;
			const int id = railway.addEdge(pair.first, pair.second, a + (b - a) / 3, a + (b - a) * 2 / 3, 80, true);
			auto* edge = railway.getEdge(id);
			edge->edgeState = EdgeState::Existing;
			edge->tunnel = true;
			edge->useElevation = true;
		}
		SubsurfaceView view;
		view.prepare(world, roads, railway);
		context.expect(
			!view.containsEdge(surface) && view.containsEdge(tunnel), U"地上の道を除外し、同じXZの地下道路を表示");
		const Ray ray{Vec3{32700, 100, 32600}, Vec3{0, -1, 0}};
		const auto hit = view.hit(ray);
		context.expect(hit && hit->edge == tunnel && hit->position.y < 11, U"見えている地下路盤を同じメッシュで選択");
		const auto stationParts = SubsurfaceView::stationGeometry(world, railway, station, true);
		context.expect(!stationParts.isEmpty(), U"地下駅のプラットホームを表示");
		for (const auto& part : stationParts)
		{
			for (const auto& vertex : part.vertices)
			{
				context.expect(vertex.pos.y <= 29.501f, U"地上の入口・屋根を地下表示に混ぜない");
			}
		}
		const auto frame = RailwaySite::stationFrame(railway, station);
		const Vec3 platform = frame->point(-5.8, 1.1, 8);
		const auto stationHit =
			SubsurfaceView::stationHit(world, railway, Ray{platform + Vec3{0, 80, 0}, Vec3{0, -1, 0}}, true);
		context.expect(stationHit && stationHit->station == station, U"実際の駅形状をクリックして選択できる");
		const auto summary = RailInfoPanel::station(station, railway);
		context.expect(summary.title == U"谷中駅" && !summary.canFollow, U"駅情報は列車追跡と混同しない");
		RenderTexture target{640, 360, TextureFormat::R8G8B8A8_Unorm, HasDepth::Yes};
		{
			const ScopedRenderTarget3D render{target.clear(ColorF{.02})};
			const ScopedRenderStates3D depth{DepthStencilState::DepthTestWrite};
			Graphics3D::SetCameraTransform(BasicCamera3D{{640, 360}, 40_deg, {32700, 420, 32300}, {32700, 10, 32700}});
			Graphics3D::SetGlobalAmbientColor(ColorF{.8});
			view.draw({32700, 420, 32300});
			Graphics3D::Flush();
		}
		Image image;
		target.readAsImage(image);
		image.save(U"Screenshot/underground_geometry.png");
		int changed = 0;
		for (const auto pixel : image)
		{
			changed += pixel.r > 30 || pixel.g > 30 || pixel.b > 30;
		}
		context.expect(changed > 400, U"地下交通網と駅の断面をGPUで描画");
		JSON metrics;
		metrics[U"pixels"] = changed;
		metrics[U"stationParts"] = stationParts.size();
		metrics.save(U"TestResults/underground_review.json");
	});
	runner.add(U"GenerationRevision.DisabledElements", [](TestContext& context)
	{
		World world;
		world.reserveChunks();
		for (int z = 31; z <= 32; ++z)
		{
			for (int x = 31; x <= 32; ++x)
			{
				world.installChunkDirect({x, z}, {Grid<float>(HEIGHT_CELLS + 1, HEIGHT_CELLS + 1, 30), 30, 30});
			}
		}
		RoadNetwork roads;
		TrainNetwork rail;
		rail.bind(&roads);
		const int a = roads.addNode({32200, 30, 32700}), b = roads.addNode({33000, 30, 32700});
		const int id = *roads.addEdge(a, b, {32466, 30, 32700}, {32733, 30, 32700}, RoadType::LocalRoad, 2);
		roads.getEdge(id)->edgeState = EdgeState::Existing;
		MapGenerator::Settlement village;
		village.center = {32600, 32700};
		village.radius = 300;
		village.plan = UrbanMorphology::makePlan(UrbanMorphology::Origin::Rural, 2, {}, 42, false);
		const Array<MapGenerator::Settlement> places{village};
		GenerationOptions options;
		options.selected[static_cast<size_t>(GenerationOptions::Element::Farms)] = false;
		SettlementDevelopment development{world, roads, rail, places, 42, options};
		development.applyZonesGlobal();
		development.placeInitialBuildings();
		int fields = 0, access = 0;
		for (const auto& edge : roads.edges())
		{
			access += edge.farmAccess;
		}
		for (int z = 31; z <= 32; ++z)
		{
			for (int x = 31; x <= 32; ++x)
			{
				for (const auto zone : world.getChunk({x, z})->zoneMap)
				{
					fields += zone == ZoneType::Agriculture;
				}
				for (const auto& patch : world.getChunk({x, z})->landPatches)
				{
					fields += patch.type == LandPatchType::FarmField || patch.type == LandPatchType::PaddyField;
				}
			}
		}
		context.expect(fields == 0 && access == 0, U"田畑を外すと農業用途・畦道・ほ場を生成しない");
	});

	runner.add(U"GenerationRevision.MountainVillages", [](TestContext& context)
	{
		Array<SettlementPlacement::Candidate> sites;
		for (int z = 5000; z < 58000; z += 3000)
		{
			sites << SettlementPlacement::Candidate{{32768, z}, .2f};
		}
		const auto valley = [](Vec2 p) { return 450 + Max(0.0, Abs(p.x - 32768) - 50) * .3; };
		int villages = 0;
		for (uint64 seed : {1ULL, 42ULL, 2026ULL})
		{
			const auto towns = SettlementPlacement::generate(seed, sites, {24000, 0, 16000, 64000}, valley);
			for (const auto& town : towns)
			{
				context.expect(town.kind == MapGenerator::SettlementKind::RuralSettlement && valley(town.center) >= 450,
					U"山間の狭い谷には都市ではなく村が成立");
				++villages;
			}
		}
		context.expect(villages > 0, U"耕地が2方向にしかない谷底にも山村を生成");
	});
	runner.add(U"GenerationRevision.FarmDeadEnd", [](TestContext& context)
	{
		World world;
		world.reserveChunks();
		for (int z = 31; z <= 32; ++z)
		{
			for (int x = 31; x <= 32; ++x)
			{
				world.installChunkDirect({x, z}, {Grid<float>(65, 65, 30), 30, 30});
			}
		}
		RoadNetwork roads;
		const auto line = [&](Vec3 a, Vec3 b, bool farm)
		{
			const int first = roads.addNode(a), last = roads.addNode(b);
			const int edge =
				*roads.addEdge(first, last, a.lerp(b, 1.0 / 3), a.lerp(b, 2.0 / 3), RoadType::LocalRoad, 2);
			GeneratedStreet::apply(*roads.getEdge(edge),
				GeneratedStreet::describe(farm ? GeneratedStreet::Role::FarmAccess : GeneratedStreet::Role::Local));
			roads.getEdge(edge)->farmAccess = farm;
			roads.getEdge(edge)->edgeState = EdgeState::Existing;
			return last;
		};
		const int deadEnd = line({32000, 30, 32300}, {32300, 30, 32300}, true);
		line({32450, 30, 32100}, {32450, 30, 32500}, false);
		const auto stats = AgriculturalLayout::prepare(world, roads, 42);
		context.expect(stats.connections >= 1 && roads.getNode(deadEnd)->attachments.size() == 2,
			U"150m先の道へ既存畦道の行き止まりを接続");
		JSON report;
		report[U"connections"] = stats.connections;
		report.save(U"TestResults/farm_dead_end.json");
	});
	runner.add(U"GenerationRevision.TrainFollow", [](TestContext& context)
	{
		TrainNetwork rails;
		const int a = rails.addStation({100, 30, 100}, U"東駅"), b = rails.addStation({1000, 45, 100}, U"西");
		const int edge = rails.addEdge(a, b, {400, 35, 100}, {700, 40, 100}, 80);
		Train train;
		train.currentEdge = edge;
		train.routeEdges = {edge};
		train.forward = true;
		train.arcPos = 300;
		const auto first = RailInfoPanel::followPosition(train, rails);
		train.arcPos = 400;
		const auto second = RailInfoPanel::followPosition(train, rails);
		context.expect(first.distanceFrom(second) > 99 && first.distanceFrom(second) < 101,
			U"追跡先は走行中の車体と同じ距離だけ進む");
		context.expect(RailInfoPanel::station(a, rails).title == U"東駅", U"入力済みの駅接尾辞を二重に付けない");
	});
	runner.add(U"GenerationRevision.LayerAndFpsLayout", [](TestContext& context)
	{
		for (const Size size : {Size{800, 600}, Size{1280, 768}})
		{
			const auto button = StartScreenControls::layerButton(size, true);
			const FrameRateGraph graph;
			context.expect(!button.intersects(graph.bounds(size)), U"FPSグラフの上に地下切替を重ねない");
			const RenderTexture target{size};
			{
				const ScopedRenderTarget2D render{target.clear(ColorF{0})};
				StartScreenControls::drawLayerButton(size, true, FontAsset(Asset::Small16), true);
			}
			Graphics2D::Flush();
			Image pixels;
			target.readAsImage(pixels);
			int occupied = 0;
			for (int y = static_cast<int>(button.y); y < button.br().y; ++y)
			{
				for (int x = static_cast<int>(button.x); x < button.br().x; ++x)
				{
					occupied += pixels[y][x].b > 30;
				}
			}
			context.expect(occupied > 2000, U"FPS表示時の移動先にも切替ボタンを描画");
		}
	});
	runner.add(U"GenerationRevision.FlatFarmMeanders", [](TestContext& context)
	{
		World world;
		world.reserveChunks();
		for (int z = 31; z <= 33; ++z)
		{
			for (int x = 31; x <= 33; ++x)
			{
				world.installChunkDirect({x, z}, {Grid<float>(65, 65, 30), 30, 30});
				world.getChunk({x, z})->zoneMap.fill(ZoneType::Agriculture);
			}
		}
		RoadNetwork roads;
		const Vec3 a{31900, 30, 33000}, b{34400, 30, 33000};
		const int first = roads.addNode(a), last = roads.addNode(b);
		const int edge = *roads.addEdge(first, last, a.lerp(b, 1.0 / 3), a.lerp(b, 2.0 / 3), RoadType::LocalRoad, 2);
		roads.getEdge(edge)->edgeState = EdgeState::Existing;
		AgriculturalLayout::Frame frame;
		frame.center = {33150, 33000};
		const auto stats = AgriculturalLayout::prepare(world, roads, 42, {frame});
		int curved = 0;
		for (const auto& road : roads.edges())
		{
			if (road.id < 0 || !road.farmAccess)
			{
				continue;
			}
			const auto curve = *roads.getBezier(road.id);
			const Vec2 arm{curve.p1.x - curve.p0.x, curve.p1.z - curve.p0.z},
				span{curve.p3.x - curve.p0.x, curve.p3.z - curve.p0.z};
			curved += Abs(arm.x * span.y - arm.y * span.x) > 1;
		}
		context.expect(curved >= 3, U"完全な平地でも高さの変化に頼らず水平に曲がる畦道を生成");
		JSON report;
		report[U"curvedRoads"] = curved;
		report[U"tracks"] = stats.tracks;
		report[U"connections"] = stats.connections;
		report.save(U"TestResults/flat_farm_meanders.json");
		TrainNetwork rail;
		const int n0 = rail.addStation({32000, 10, 32800}, U"西"), n1 = rail.addStation({33000, 10, 32800}, U"東");
		const int track = rail.addEdge(n0, n1, {32333, 10, 32800}, {32666, 10, 32800}, 60);
		Train train;
		train.currentEdge = track;
		train.routeEdges = {track};
		train.arcPos = 500;
		train.forward = true;
		const auto pose = TrainConsist::carPose(train, rail, 0);
		const Ray ray{pose->position + Vec3{0, 50, 0}, Vec3{0, -1, 0}};
		context.expect(TrainConsist::hitDistance(train, rail, ray).has_value() &&
						   !TrainConsist::hitDistance(train, rail, ray, [](Vec3) { return false; }),
			U"非表示車両を選択候補に残さない");
	});
}
