#include "GameScene.hpp"
#include "../gen/RoadPathfinder.hpp"
#include "../save/RoadBinary.hpp"
#include "../sim/SimGraph.hpp"
#include "../asset/AssetRegistrar.hpp"
#include <thread>

// =============================================================================
// 初期化
// =============================================================================

GameScene::GameScene(const InitData& init)
	: IScene{ init }
{
	m_renderTexture = RenderTexture{ Scene::Size(), TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes };
	m_trainManager.init(&m_trainNetwork);

	m_roadRenderer.loadAssets();

	m_sandboxActive = getData().sandboxMode;

	initScene();
}

GameScene::~GameScene()
{
	m_simThread.stop();

	if (m_generationFuture.valid())
		m_generationFuture.wait();
}

void GameScene::initScene()
{
	m_panelManager.registerPanel(U"edge_info", Vec2{374, static_cast<double>(Scene::Height() - 20)}, true, true);
	m_panelManager.registerPanel(U"node_info", Vec2{312, static_cast<double>(Scene::Height() - 20)}, true, true);
	m_panelManager.registerPanel(U"name_list", Vec2{250, static_cast<double>(Scene::Height() - 20)}, false, true);
	m_panelManager.registerPanel(U"vehicle_info", Vec2{280, static_cast<double>(Scene::Height() - 20)}, true, true);
	m_panelManager.registerPanel(U"draw_template", Vec2{374, static_cast<double>(Scene::Height() - 20)}, true, true);
	{
		const double side = Min(Scene::Width(), Scene::Height()) - 80.0;
		m_panelManager.registerPanel(U"minimap_expanded", Vec2{side, side}, true);
	}
	m_panelManager.registerPanel(U"signal_edit", Vec2{700, 550}, true, true);

	// 道路設置テンプレートの初期値（LocalRoad, 2車線）
	m_drawTemplate.roadType   = RoadType::LocalRoad;
	m_drawTemplate.speedLimit = 60.0f;
	m_drawTemplate.lanes      = RoadNetwork::buildDefaultLanes(2, RoadType::LocalRoad);
	RoadNetwork::buildDefaultParts(m_drawTemplate);

	if (getData().isNewGame)
		initNewGame();
	else
		initLoadGame();
}

void GameScene::initLoadGame()
{
	startLoadingPhase(U"ロード中...", U"セーブデータを読み込み中...", [this]()
	{
		m_loadGameResult = loadGame();
	});
}

void GameScene::initNewGame()
{
	const auto initResult = MapGenerator::initWorld(getData().seed, m_world);
	m_placeNames = std::move(initResult.placeNames);

	m_world.reserveChunks();

	const float worldCenter = static_cast<float>(WORLD_CHUNKS) * CHUNK_SIZE * 0.5f;
	m_camera.setFocus(Vec3{ worldCenter, 0.0, worldCenter });

	m_totalInitChunks = WORLD_CHUNKS * WORLD_CHUNKS;

	startLoadingPhase(U"マップ生成中...", U"地形生成中", [this]()
	{
		generateAllTerrain();
		placeAllSettlements();
		generateAllRoads();
		generateDistrictRoads();
		postProcessRoads();
	});
	Logger << U"[Loading] {} チャンク生成開始"_fmt(m_totalInitChunks);
}

// =============================================================================
void GameScene::startLoadingPhase(StringView title, StringView status,
                                  std::function<void()> pipeline)
{
	m_loadingTimer.restart();
	m_loadingTitle  = title;
	m_loadingStatus = status;
	m_genProgress   = 0.0f;
	m_generationFuture = std::async(std::launch::async, std::move(pipeline));
	m_phase = GamePhase::Loading;
}

// =============================================================================
// バックグラウンド生成パイプライン
// =============================================================================

static constexpr float kProgressTerrain      = 0.00f;
static constexpr float kProgressSettlement   = 0.03f;
static constexpr float kProgressRoads        = 0.25f;
static constexpr float kProgressDistrict     = 0.74f;
static constexpr float kProgressPostProcess  = 0.79f;
static constexpr float kProgressDone         = 0.91f;

void GameScene::generateAllTerrain()
{
	m_loadingStatus = U"地形生成中";
	const Stopwatch sw{ StartImmediately::Yes };

	const int total = WORLD_CHUNKS * WORLD_CHUNKS;
	const int nThreads = Max(1, static_cast<int>(std::thread::hardware_concurrency()));

	Array<HeightMapResult> buffer(total);

	std::atomic<int> counter{ 0 };
	Array<std::thread> threads;
	threads.reserve(nThreads);

	for (int t = 0; t < nThreads; ++t)
	{
		threads.emplace_back([this, &buffer, &counter, total]()
		{
			while (true)
			{
				const int idx = counter.fetch_add(1);
				if (idx >= total) break;
				const int cx = idx % WORLD_CHUNKS;
				const int cy = idx / WORLD_CHUNKS;
				buffer[idx] = m_world.buildHeightMap(Point{ cx, cy });
				m_genProgress.store(kProgressTerrain +
					(kProgressSettlement - kProgressTerrain) * static_cast<float>(idx + 1) / total);
			}
		});
	}

	for (auto& th : threads) th.join();

	for (int idx = 0; idx < total; ++idx)
	{
		const int cx = idx % WORLD_CHUNKS;
		const int cy = idx / WORLD_CHUNKS;
		m_world.installChunkDirect(Point{ cx, cy }, std::move(buffer[idx]));
	}

	m_genProgress.store(kProgressSettlement);
	Logger << U"[Phase1] 地形生成完了: {} チャンク ({:.1f}秒)"_fmt(total, sw.sF());
}

void GameScene::placeAllSettlements()
{
	m_loadingStatus = U"地区配置中";

	auto settlements = MapGenerator::placeAllSettlements(getData().seed, m_world);

	Array<BiomeType> biomes;
	biomes.reserve(settlements.size());
	for (const auto& s : settlements)
		biomes << m_world.getBiome(static_cast<float>(s.center.x), static_cast<float>(s.center.y));

	PlaceNameGenerator placeGen;
	placeGen.load(U"assets/placenames/placenames.toml");
	m_placeNames = placeGen.generateWithBiomes(
		static_cast<int>(settlements.size()), biomes, getData().seed);

	for (int i = 0; i < static_cast<int>(settlements.size()); ++i)
	{
		settlements[i].name    = m_placeNames.settlementName(i);
		settlements[i].reading = m_placeNames.settlementReading(i);
	}

	m_districts = settlements;
	m_urbanCenters.clear();
	for (const auto& s : m_districts)
	{
		if (s.type == MapGenerator::SettlementType::Urban)
			m_urbanCenters << s.center;
	}

	m_genProgress.store(kProgressRoads);
}

void GameScene::generateAllRoads()
{
	m_loadingStatus = U"道路生成中";

	MapGenerator::generateGlobalRoads(
		getData().seed, m_districts, m_world, m_network,
		[this](float fraction) {
			m_genProgress.store(kProgressRoads +
				(kProgressDistrict - kProgressRoads) * fraction);
		});
}

void GameScene::generateDistrictRoads()
{
	m_loadingStatus = U"地区内道路生成中";
	m_genProgress.store(kProgressDistrict);

	MapGenerator::generateDistrictRoads(
		getData().seed, m_districts, m_world, m_network,
		[this](float fraction) {
			m_genProgress.store(kProgressDistrict +
				(kProgressPostProcess - kProgressDistrict) * fraction);
		});
}

void GameScene::postProcessRoads()
{
	m_loadingStatus = U"道路ポスト処理中";
	m_genProgress.store(kProgressPostProcess);

	const Stopwatch total{ StartImmediately::Yes };
	Stopwatch step{ StartImmediately::Yes };

	Logger << U"[PostProcess] 開始 (nodes={}, edges={})"_fmt(
		m_network.nodes().size(), m_network.edges().size());

	{
		int iter = 0;
		constexpr int kMaxIter = 1000;
		while (iter < kMaxIter && m_network.fixSharpAngles(45.0f))
			++iter;
		Logger << U"[PostProcess] fixSharpAngles: {} 回, {:.0f}ms"_fmt(iter, step.msF());
		step.restart();
	}

	m_network.smoothAllCurves();
	Logger << U"[PostProcess] smoothAllCurves: {:.0f}ms"_fmt(step.msF());
	step.restart();

	m_network.resolveIntersections();
	Logger << U"[PostProcess] resolveIntersections: {:.0f}ms"_fmt(step.msF());
	step.restart();

	m_network.spreadIntersectionTangents();
	Logger << U"[PostProcess] spreadIntersectionTangents: {:.0f}ms"_fmt(step.msF());
	step.restart();

	m_network.removeDuplicateEdges(getData().seed);
	Logger << U"[PostProcess] removeDuplicateEdges: {:.0f}ms"_fmt(step.msF());
	step.restart();

	{
		const int n = m_network.mergeShortEdges(30.0f);
		Logger << U"[PostProcess] mergeShortEdges: {} 結合, {:.0f}ms"_fmt(n, step.msF());
	}

	m_genProgress.store(kProgressDone);
	Logger << U"[Phase4] PostProcess 完了 ({:.0f}ms)"_fmt(total.msF());
}

// =============================================================================
// Loading フェーズ更新
// =============================================================================

void GameScene::updateLoading()
{
	if (m_generationFuture.valid())
	{
		if (m_generationFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			m_generationFuture.get();

			if (m_loadGameResult)
			{
				Logger << U"[Load] 完了 ({:.1f}秒)"_fmt(m_loadingTimer.sF());
				m_minimapRenderer.buildTerrainTexture(m_world);
				m_minimapRenderer.updateRoadOverlay(m_network, m_world);
				m_phase = GamePhase::Playing;
				return;
			}

			Logger << U"[Loading] 全パイプライン完了 ({:.1f}秒)"_fmt(m_loadingTimer.sF());
			m_loadingStatus = U"初期化中...";

			applyZonesGlobal();
			placeInitialBuildings();
			m_roadRenderer.invalidateAllCaches();
			MapGenerator::setupTrain(m_trainNetwork, m_world, m_districts);

			const float worldCenter = static_cast<float>(WORLD_CHUNKS) * CHUNK_SIZE * 0.5f;
			Vec3 cameraFocus{ worldCenter, 0.0, worldCenter };
			for (const auto& s : m_districts)
			{
				if (s.type == MapGenerator::SettlementType::Urban)
				{
					const float y = m_world.computeHeight(
						static_cast<float>(s.center.x), static_cast<float>(s.center.y));
					cameraFocus = Vec3{ s.center.x, y, s.center.y };
					break;
				}
			}
			m_camera.setFocus(cameraFocus);
			m_world.update(m_camera.focusPoint());

			startSimThread();

			m_minimapRenderer.buildTerrainTexture(m_world);
			m_minimapRenderer.updateRoadOverlay(m_network, m_world);

			Logger << U"[Phase] 全体 {:.1f}秒 → Playing へ遷移"_fmt(m_loadingTimer.sF());
			m_phase = GamePhase::Playing;
			return;
		}
	}

	drawLoadingScreen(Clamp(m_genProgress.load(), 0.0f, 1.0f));
}

// =============================================================================
// SimThread 起動
// =============================================================================

void GameScene::startSimThread()
{
	m_simGraph = std::make_shared<const SimGraph>(SimGraph::build(m_network));
	m_vehicleManager.init(*m_simGraph, m_network);
	m_simThread.start(m_simGraph);
}

// =============================================================================
// ローディング画面描画
// =============================================================================

void GameScene::drawLoadingScreen(float progress)
{
	Scene::Rect().draw(ColorF{ 0.07, 0.11, 0.16 });

	const auto& titleFont = FontAsset(Asset::TitleBold48);
	const auto& uiFont    = FontAsset(Asset::UI20);
	const auto& smallFont = FontAsset(Asset::Small16);

	const Vec2 center = Scene::Center();

	const int elapsedSec = static_cast<int>(m_loadingTimer.sF());
	const int min = elapsedSec / 60;
	const int sec = elapsedSec % 60;
	uiFont(U"{}:{:0>2}"_fmt(min, sec)).drawAt(
		center.movedBy(0, -110), ColorF{ 0.7 });

	titleFont(m_loadingTitle).drawAt(center.movedBy(0, -60), ColorF{ 0.9 });

	const RectF barBg{ center.x - 200, center.y, 400, 24 };
	barBg.draw(ColorF{ 0.2 });
	RectF{ barBg.pos, barBg.w * progress, barBg.h }.draw(ColorF{ 0.3, 0.7, 1.0 });

	uiFont(U"{}%"_fmt(static_cast<int>(progress * 100))).drawAt(barBg.center(), ColorF{ 1.0 });

	smallFont(m_loadingStatus).drawAt(
		center.movedBy(0, 70), ColorF{ 0.5 });
}

// =============================================================================
// セーブ / ロード
// =============================================================================

void GameScene::saveGame()
{
	if (getData().saveName.isEmpty())
		getData().saveName = U"default";

	const String kRoot = U"saves/{}"_fmt(getData().saveName);
	FileSystem::CreateDirectories(U"{}/global"_fmt(kRoot));

	// meta.json
	JSON meta;
	meta[U"version"]      = 3;
	meta[U"seed"]         = getData().seed;
	meta[U"worldChunks"]  = WORLD_CHUNKS;
	meta[U"gameNow"]      = m_clock.now;
	meta[U"timeScale"]    = static_cast<int>(m_clock.speed);
	meta[U"nextNodeId"]   = m_network.nextNodeId();
	meta[U"nextEdgeId"]   = m_network.nextEdgeId();
	meta[U"cameraFocusX"]  = m_camera.focusPoint().x;
	meta[U"cameraFocusY"]  = m_camera.focusPoint().y;
	meta[U"cameraFocusZ"]  = m_camera.focusPoint().z;
	meta[U"cameraDistance"] = m_camera.distance();
	meta[U"cameraYaw"]     = m_camera.yaw();
	meta[U"cameraPitch"]   = m_camera.pitch();
	meta.save(U"{}/meta.json"_fmt(kRoot));

	// economy.json
	JSON eco;
	eco[U"funds"]      = m_economy.funds;
	eco[U"population"] = m_economy.population;
	eco[U"happiness"]  = m_economy.happiness;
	eco.save(U"{}/global/economy.json"_fmt(kRoot));

	// roads.bin
	RoadBinary::writeGlobal(U"{}/global/roads.bin"_fmt(kRoot), m_network);

	// districts.json
	JSON dist;
	dist[U"count"] = static_cast<int>(m_districts.size());
	for (int i = 0; i < static_cast<int>(m_districts.size()); ++i)
	{
		const auto& s = m_districts[i];
		dist[U"type_{}"_fmt(i)]    = static_cast<int>(s.type);
		dist[U"cx_{}"_fmt(i)]      = s.center.x;
		dist[U"cy_{}"_fmt(i)]      = s.center.y;
		dist[U"name_{}"_fmt(i)]    = s.name;
		dist[U"reading_{}"_fmt(i)] = s.reading;
	}
	dist.save(U"{}/global/districts.json"_fmt(kRoot));

	// 地形データ（チャンクごと）
	const String chunksDir = U"{}/chunks"_fmt(kRoot);
	for (int cy = 0; cy < WORLD_CHUNKS; ++cy)
	{
		for (int cx = 0; cx < WORLD_CHUNKS; ++cx)
		{
			const Chunk* chunk = m_world.getChunk(Point{ cx, cy });
			if (!chunk || chunk->heightMap.isEmpty()) continue;

			const String dir = U"{}/{}_{}"_fmt(chunksDir, cx, cy);
			FileSystem::CreateDirectories(dir);

			BinaryWriter w{ U"{}/terrain.bin"_fmt(dir) };
			if (!w) continue;

			const int gridSize = HEIGHT_CELLS + 1;
			w.write(static_cast<int32>(gridSize));
			for (int r = 0; r < gridSize; ++r)
				for (int c = 0; c < gridSize; ++c)
					w.write(chunk->heightMap[{ c, r }]);
		}
	}

	Console << U"[Save] Saved to " << kRoot;
}

bool GameScene::loadGame()
{
	const Stopwatch loadTotal{ StartImmediately::Yes };
	Stopwatch step{ StartImmediately::Yes };

	const String kRoot = U"saves/{}"_fmt(getData().saveName);

	// meta.json
	const JSON meta = JSON::Load(U"{}/meta.json"_fmt(kRoot));
	if (!meta) return false;

	getData().seed    = meta[U"seed"].get<uint64>();
	const double gameNow    = meta[U"gameNow"].get<double>();
	const int    timeScale  = meta[U"timeScale"].get<int>();
	const int    nextNodeId = meta[U"nextNodeId"].get<int>();
	const int    nextEdgeId = meta[U"nextEdgeId"].get<int>();
	const double focusX     = meta[U"cameraFocusX"].get<double>();
	const double focusY     = meta[U"cameraFocusY"].get<double>();
	const double focusZ     = meta[U"cameraFocusZ"].get<double>();
	const float  camDist    = meta[U"cameraDistance"].getOr<float>(600.0f);
	const float  camYaw     = meta[U"cameraYaw"].getOr<float>(0.0f);
	const float  camPitch   = meta[U"cameraPitch"].getOr<float>(static_cast<float>(40.0_deg));

	m_world.setGenerationParams(getData().seed,
		static_cast<float>(WORLD_CHUNKS) * CHUNK_SIZE,
		static_cast<float>(WORLD_CHUNKS) * CHUNK_SIZE);
	m_world.reserveChunks();
	Console << U"[Load] meta+init: {:.0f}ms"_fmt(step.msF());
	step.restart();

	// 地形データ（チャンクごとに並列読み込み + バルク I/O）
	const int totalChunks = WORLD_CHUNKS * WORLD_CHUNKS;
	m_loadingStatus = U"地形データ読み込み中...";
	{
		Array<HeightMapResult> buffer(totalChunks);
		std::atomic<int> counter{ 0 };
		std::atomic<int> loadedAtomic{ 0 };
		const int nThreads = Max(1, static_cast<int>(std::thread::hardware_concurrency()));
		Array<std::thread> threads;
		threads.reserve(nThreads);

		for (int t = 0; t < nThreads; ++t)
		{
			threads.emplace_back([this, &buffer, &counter, &loadedAtomic, totalChunks, &kRoot]()
			{
				while (true)
				{
					const int idx = counter.fetch_add(1);
					if (idx >= totalChunks) break;
					const int cx = idx % WORLD_CHUNKS;
					const int cy = idx / WORLD_CHUNKS;

					const String terrainPath = U"{}/chunks/{}_{}/terrain.bin"_fmt(kRoot, cx, cy);
					bool loaded = false;

					if (FileSystem::Exists(terrainPath))
					{
						BinaryReader r{ terrainPath };
						if (r)
						{
							int32 gridSize = 0;
							r.read(gridSize);
							if (gridSize == HEIGHT_CELLS + 1)
							{
								HeightMapResult hmr;
								hmr.heightMap = Grid<float>(gridSize, gridSize);
								// バルク読み込み: gridSize² 個の float を1 回で取得
								const size_t cellCount = static_cast<size_t>(gridSize) * gridSize;
								r.read(hmr.heightMap.data(), cellCount * sizeof(float));
								// min/max は読み込み完了後にまとめて算出
								float mn =  1e30f, mx = -1e30f;
								const float* p = hmr.heightMap.data();
								for (size_t i = 0; i < cellCount; ++i) { mn = Min(mn, p[i]); mx = Max(mx, p[i]); }
								hmr.heightMin = mn;
								hmr.heightMax = mx;
								buffer[idx] = std::move(hmr);
								loaded = true;
							}
						}
					}

					if (!loaded)
					{
						buffer[idx] = m_world.buildHeightMap(Point{ cx, cy });
					}
					const int done = loadedAtomic.fetch_add(1) + 1;
					m_genProgress = static_cast<float>(done) / static_cast<float>(totalChunks) * 0.7f;
				}
			});
		}
		for (auto& th : threads) th.join();

		// installChunkDirect は m_world 内部状態を変更するためメインスレッドで直列に
		for (int idx = 0; idx < totalChunks; ++idx)
		{
			const int cx = idx % WORLD_CHUNKS;
			const int cy = idx / WORLD_CHUNKS;
			m_world.installChunkDirect(Point{ cx, cy }, std::move(buffer[idx]));
		}
	}

	Console << U"[Load] terrain: {:.0f}ms"_fmt(step.msF());
	step.restart();

	// 経済
	if (const JSON eco = JSON::Load(U"{}/global/economy.json"_fmt(kRoot)))
	{
		m_economy.funds      = eco[U"funds"].get<double>();
		m_economy.population = eco[U"population"].get<int>();
		m_economy.happiness  = eco[U"happiness"].get<double>();
	}

	// 道路ネットワーク
	{
		const String roadPath = U"{}/global/roads.bin"_fmt(kRoot);
		const bool roadOk = RoadBinary::readGlobal(roadPath, m_network);
		Console << U"[Load] roads: " << (roadOk ? U"OK" : U"FAILED (format mismatch? re-save needed)")
		        << U" nodes=" << m_network.nodes().size()
		        << U" edges=" << m_network.edges().size();
	}
	m_network.setNextIds(nextNodeId, nextEdgeId);

	m_genProgress = 0.8f;
	m_loadingStatus = U"道路・経済データ復元完了";
	Console << U"[Load] economy+roads: {:.0f}ms"_fmt(step.msF());
	step.restart();

	// 集落
	m_districts.clear();
	m_urbanCenters.clear();
	if (const JSON dist = JSON::Load(U"{}/global/districts.json"_fmt(kRoot)))
	{
		const int count = dist[U"count"].get<int>();
		Array<MapGenerator::Settlement> settlements;
		for (int i = 0; i < count; ++i)
		{
			MapGenerator::Settlement s;
			s.type   = static_cast<MapGenerator::SettlementType>(dist[U"type_{}"_fmt(i)].get<int>());
			s.center = Vec2{ dist[U"cx_{}"_fmt(i)].get<double>(), dist[U"cy_{}"_fmt(i)].get<double>() };
			s.name   = dist[U"name_{}"_fmt(i)].get<String>();
			const String readingKey = U"reading_{}"_fmt(i);
			if (dist.hasElement(readingKey))
				s.reading = dist[readingKey].get<String>();
			settlements << s;
		}
		addDistricts(settlements);
	}

	m_genProgress = 0.9f;
	m_loadingStatus = U"ゾーン・建物を復元中...";

	applyZonesGlobal();
	placeInitialBuildings();

	m_roadRenderer.invalidateAllCaches();

	m_clock.now   = gameNow;
	m_clock.speed = static_cast<TimeSpeed>(timeScale);
	m_clock.syncCalendar();

	m_camera.setState(Vec3{ focusX, focusY, focusZ }, camDist, camYaw, camPitch);

	MapGenerator::setupTrain(m_trainNetwork, m_world, m_districts);

	m_world.update(m_camera.focusPoint());
	startSimThread();

	Console << U"[Load] finish: {:.0f}ms"_fmt(step.msF());
	m_genProgress = 1.0f;
	Console << U"[Load] TOTAL: {:.0f}ms from {}"_fmt(loadTotal.msF(), kRoot);
	return true;
}

// =============================================================================
// 地区リスト更新
// =============================================================================

void GameScene::addDistricts(const Array<MapGenerator::Settlement>& newDistricts)
{
	for (auto s : newDistricts)
	{
		if (s.name.isEmpty())
		{
			const int idx = static_cast<int>(m_districts.size());
			s.name    = m_placeNames.settlementName(idx);
			s.reading = m_placeNames.settlementReading(idx);
		}
		m_districts << s;
		if (s.type == MapGenerator::SettlementType::Urban)
			m_urbanCenters << s.center;
	}
}

// =============================================================================
// ゾーン一括割り当て
// =============================================================================

void GameScene::applyZonesGlobal()
{
	const Stopwatch sw{ StartImmediately::Yes };
	constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;

	for (const auto& s : m_districts)
	{
		const float innerDist = (s.type == MapGenerator::SettlementType::Urban)    ? 400.0f
		                      : (s.type == MapGenerator::SettlementType::Suburbs)  ? 200.0f : 100.0f;
		const float midDist   = (s.type == MapGenerator::SettlementType::Urban)    ? 800.0f
		                      : (s.type == MapGenerator::SettlementType::Suburbs)  ? 600.0f : 300.0f;
		const float outerDist = (s.type == MapGenerator::SettlementType::Urban)    ? 1200.0f
		                      : midDist;

		const float innerSq = innerDist * innerDist;
		const float midSq   = midDist   * midDist;
		const float outerSq = outerDist * outerDist;

		const float scx = static_cast<float>(s.center.x);
		const float scz = static_cast<float>(s.center.y);

		const int chunkRadius = static_cast<int>(Ceil(outerDist / CHUNK_SIZE));
		const int ccx = static_cast<int>(Math::Floor(scx / CHUNK_SIZE));
		const int ccz = static_cast<int>(Math::Floor(scz / CHUNK_SIZE));

		for (int dcy = -chunkRadius; dcy <= chunkRadius; ++dcy)
		{
			for (int dcx = -chunkRadius; dcx <= chunkRadius; ++dcx)
			{
				const Point cc{ ccx + dcx, ccz + dcy };
				Chunk* chunk = m_world.getChunk(cc);
				if (!chunk) continue;

				const float chunkOriginX = static_cast<float>(cc.x * CHUNK_SIZE);
				const float chunkOriginZ = static_cast<float>(cc.y * CHUNK_SIZE);

				const int gxMin = Max(0, static_cast<int>((scx - outerDist - chunkOriginX) / cellSize));
				const int gxMax = Min(ZONE_CELLS - 1, static_cast<int>((scx + outerDist - chunkOriginX) / cellSize));
				const int gzMin = Max(0, static_cast<int>((scz - outerDist - chunkOriginZ) / cellSize));
				const int gzMax = Min(ZONE_CELLS - 1, static_cast<int>((scz + outerDist - chunkOriginZ) / cellSize));

				if (gxMin > gxMax || gzMin > gzMax) continue;

				for (int gz = gzMin; gz <= gzMax; ++gz)
				{
					for (int gx = gxMin; gx <= gxMax; ++gx)
					{
						const float wx = chunkOriginX + (gx + 0.5f) * cellSize;
						const float wz = chunkOriginZ + (gz + 0.5f) * cellSize;
						const float dx = wx - scx;
						const float dz = wz - scz;
						const float dSq = dx * dx + dz * dz;

						if (dSq > outerSq) continue;

						const float h = sampleHeightMap(chunk->heightMap, cc, wx, wz);
						if (h < 0.0f) continue;

						ZoneType zt;
						if (dSq <= innerSq)
							zt = (s.type == MapGenerator::SettlementType::Urban)
								? ZoneType::Commercial : ZoneType::Residential;
						else if (dSq <= midSq)
							zt = ZoneType::Residential;
						else
							zt = ZoneType::LowResidential;

						chunk->zoneMap[{ gx, gz }] = zt;
					}
				}
			}
		}
	}

	Logger << U"[applyZonesGlobal] {:.0f}ms"_fmt(sw.msF());
}

// =============================================================================
// 初期建物配置
// =============================================================================

namespace
{
	struct RoadSample { float x; float z; float angle; float halfWidth; };

	Array<RoadSample> collectRoadSamples(
		float cx, float cz, float radius,
		const RoadNetwork& network)
	{
		const float radiusSq = radius * radius;
		Array<RoadSample> samples;

		for (const auto& edge : network.edges())
		{
			if (edge.id < 0) continue;
			const RoadNode* na = network.getNode(edge.nodeA);
			const RoadNode* nb = network.getNode(edge.nodeB);
			if (!na || !nb) continue;

			const float exMin = static_cast<float>(Min({na->position.x, edge.ctrlA.x, edge.ctrlB.x, nb->position.x}));
			const float exMax = static_cast<float>(Max({na->position.x, edge.ctrlA.x, edge.ctrlB.x, nb->position.x}));
			const float ezMin = static_cast<float>(Min({na->position.z, edge.ctrlA.z, edge.ctrlB.z, nb->position.z}));
			const float ezMax = static_cast<float>(Max({na->position.z, edge.ctrlA.z, edge.ctrlB.z, nb->position.z}));

			if (exMax < cx - radius || exMin > cx + radius) continue;
			if (ezMax < cz - radius || ezMin > cz + radius) continue;

			const float edgeHalfWidth = edge.totalWidth() * 0.5f;

			const float p0x = static_cast<float>(na->position.x);
			const float p0z = static_cast<float>(na->position.z);
			const float p1x = static_cast<float>(edge.ctrlA.x);
			const float p1z = static_cast<float>(edge.ctrlA.z);
			const float p2x = static_cast<float>(edge.ctrlB.x);
			const float p2z = static_cast<float>(edge.ctrlB.z);
			const float p3x = static_cast<float>(nb->position.x);
			const float p3z = static_cast<float>(nb->position.z);

			constexpr int kSamples = 8;
			for (int i = 0; i <= kSamples; ++i)
			{
				const float t  = static_cast<float>(i) / kSamples;
				const float t1 = 1.0f - t;
				const float c0 = t1*t1*t1;
				const float c1 = 3.0f*t1*t1*t;
				const float c2 = 3.0f*t1*t*t;
				const float c3 = t*t*t;
				const float sx = c0*p0x + c1*p1x + c2*p2x + c3*p3x;
				const float sz = c0*p0z + c1*p1z + c2*p2z + c3*p3z;

				const float ddx = sx - cx;
				const float ddz = sz - cz;
				if (ddx*ddx + ddz*ddz > radiusSq) continue;

				const float tanx = t1*t1*(p1x-p0x) + 2.0f*t1*t*(p2x-p1x) + t*t*(p3x-p2x);
				const float tanz = t1*t1*(p1z-p0z) + 2.0f*t1*t*(p2z-p1z) + t*t*(p3z-p2z);
				samples.push_back({ sx, sz, std::atan2(tanz, tanx), edgeHalfWidth });
			}
		}
		return samples;
	}

	struct NearestRoadResult { float dist; float angle; float halfWidth; };

	NearestRoadResult nearestFromSamples(
		float wx, float wz, const Array<RoadSample>& samples)
	{
		float bestDistSq  = 1e12f;
		float bestAngle   = 0.0f;
		float bestHW      = 3.5f;
		for (const auto& s : samples)
		{
			const float dx = wx - s.x;
			const float dz = wz - s.z;
			const float dSq = dx*dx + dz*dz;
			if (dSq < bestDistSq)
			{
				bestDistSq = dSq;
				bestAngle  = s.angle;
				bestHW     = s.halfWidth;
			}
		}
		return { Math::Sqrt(bestDistSq), bestAngle, bestHW };
	}
}

void GameScene::placeInitialBuildings()
{
	const Stopwatch sw{ StartImmediately::Yes };
	constexpr float cellSize       = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
	constexpr float kNearDist      = 32.0f;
	constexpr float kFarDist       = 64.0f;
	int placed = 0;

	for (const auto& s : m_districts)
	{
		const float scx = static_cast<float>(s.center.x);
		const float scz = static_cast<float>(s.center.y);
		const float searchRadius = s.radius * 2.0f;
		const float radiusSq = searchRadius * searchRadius;

		const auto roadSamples = collectRoadSamples(scx, scz, searchRadius, m_network);
		if (roadSamples.isEmpty()) continue;

		const int chunkRadius = static_cast<int>(Ceil(searchRadius / CHUNK_SIZE));
		const int ccx = static_cast<int>(Math::Floor(scx / CHUNK_SIZE));
		const int ccz = static_cast<int>(Math::Floor(scz / CHUNK_SIZE));

		for (int dcy = -chunkRadius; dcy <= chunkRadius; ++dcy)
		{
			for (int dcx = -chunkRadius; dcx <= chunkRadius; ++dcx)
			{
				const Point cc{ ccx + dcx, ccz + dcy };
				Chunk* chunk = m_world.getChunk(cc);
				if (!chunk) continue;

				const float chunkOriginX = static_cast<float>(cc.x * CHUNK_SIZE);
				const float chunkOriginZ = static_cast<float>(cc.y * CHUNK_SIZE);

				const int gxMin = Max(0, static_cast<int>((scx - searchRadius - chunkOriginX) / cellSize));
				const int gxMax = Min(ZONE_CELLS - 1, static_cast<int>((scx + searchRadius - chunkOriginX) / cellSize));
				const int gzMin = Max(0, static_cast<int>((scz - searchRadius - chunkOriginZ) / cellSize));
				const int gzMax = Min(ZONE_CELLS - 1, static_cast<int>((scz + searchRadius - chunkOriginZ) / cellSize));
				if (gxMin > gxMax || gzMin > gzMax) continue;

				for (int gz = gzMin; gz <= gzMax; ++gz)
				{
					for (int gx = gxMin; gx <= gxMax; ++gx)
					{
						const ZoneType zone = chunk->zoneMap[{ gx, gz }];
						if (zone == ZoneType::Unzoned) continue;
						if (chunk->buildingGrid[{ gx, gz }].type != BuildingType::None) continue;

						const float wx = chunkOriginX + (gx + 0.5f) * cellSize;
						const float wz = chunkOriginZ + (gz + 0.5f) * cellSize;

						const float dx = wx - scx;
						const float dz = wz - scz;
						const float distFromCenterSq = dx*dx + dz*dz;
						if (distFromCenterSq > radiusSq) continue;

						const float h = sampleHeightMap(chunk->heightMap, cc, wx, wz);
						if (h < 0.0f) continue;

						const auto [roadDist, angle, roadHW] = nearestFromSamples(wx, wz, roadSamples);
						if (roadDist < roadHW * 2.0f) continue;

						float roadScore;
						if      (roadDist < kNearDist) roadScore = 1.0f;
						else if (roadDist < kFarDist)  roadScore = 1.0f - (roadDist - kNearDist) / (kFarDist - kNearDist);
						else                           roadScore = 0.0f;

						const float distFromCenter = Math::Sqrt(distFromCenterSq);
						const float densityFactor = Clamp(0.25f * (1.0f - distFromCenter / (s.radius * 2.0f)), 0.0f, 0.25f);

						const float score = roadScore * densityFactor;
						if (score < 0.02f) continue;

						const uint32 cellHash = static_cast<uint32>(
							(gx * 73856093) ^ (gz * 19349663) ^ (cc.x * 83492791) ^ (cc.y * 41729581));
						const float roll = (cellHash % 1000) / 1000.0f;
						if (roll > score) continue;

						Building b = m_zoneManager.spawnBuilding(zone, 0.0);
						if (b.type == BuildingType::None) continue;
						b.angle = angle;

						chunk->buildingGrid[{ gx, gz }] = b;
						chunk->meshDirty = true;
						++placed;
					}
				}
			}
		}
	}

	Logger << U"[placeInitialBuildings] {} 棟配置 ({:.0f}ms)"_fmt(placed, sw.msF());
}

// =============================================================================
// 毎フレーム更新
// =============================================================================

void GameScene::update()
{
	if (m_phase == GamePhase::Loading)
	{
		updateLoading();
		return;
	}

	const double dt = Scene::DeltaTime();
	m_lockWaitMs = 0.0;

	// Sim レスポンスを処理
	for (auto& resp : m_simThread.drainResponses())
	{
		std::visit([&](auto& r)
		{
			using T = std::decay_t<decltype(r)>;
			if constexpr (std::is_same_v<T, RouteResponse>)
				m_vehicleManager.applyRouteResponse(r);
			else if constexpr (std::is_same_v<T, PerfUpdate>)
				m_simPerfHistory.push(r.stats);
		}, resp);
	}

	// ゲーム時計を進める
	if (m_clock.speed != TimeSpeed::Paused)
	{
		const double gameDt = dt * m_clock.speedMultiplier() * 60.0; // 物理用: 実時間1秒=ゲーム内60秒相当の移動
		const double vehicleDt = gameDt / 60.0;
		m_clock.advance(dt);

		if (m_simGraph)
			m_vehicleManager.update(vehicleDt, m_clock.now, *m_simGraph,
			                        m_network, m_roadRenderer.visibleEdges());

		m_trainManager.update(gameDt, m_clock.now);
		m_eventSystem.update(m_clock.now, m_clock.month, dt);
	}

	// 経路リクエストはゲーム内時間停止中でも送信する
	for (auto& req : m_vehicleManager.collectRequests())
		m_simThread.pushRequest(std::move(req));

	// メインスレッドのロジック
	const Stopwatch swLogic{ StartImmediately::Yes };
	m_world.update(m_camera.focusPoint());
	if (!m_showPauseMenu)
		m_panelManager.handleInput();
	m_camera.setBlockInput(m_showPauseMenu || m_panelManager.isMouseOnAnyPanel());
	m_camera.update(dt, m_world);
	m_logicMs = swLogic.msF();

	// 車両追跡
	if (m_trackingVehicle && m_selectedVehicleId)
	{
		if (KeyW.pressed() || KeyA.pressed() || KeyS.pressed() || KeyD.pressed())
		{
			m_trackingVehicle = false;
		}
		else
		{
			for (const auto& rv : m_renderVehicles)
			{
				if (rv.id == *m_selectedVehicleId)
				{
					m_camera.setFocus(rv.position);
					break;
				}
			}
		}
	}

	if (m_selectedVehicleId && !m_panelManager.isVisible(U"vehicle_info"))
	{
		m_selectedVehicleId = none;
		m_trackingVehicle = false;
	}

	handleInput();
	updateCursor();
	m_debugRenderer.handleInput();

	renderWorld();
}
