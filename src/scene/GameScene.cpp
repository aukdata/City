#include "GameScene.hpp"
#include "../gen/RoadPathfinder.hpp"
#include "../gen/DistrictRoads.hpp"
#include "../save/RoadBinary.hpp"
#include "../save/GuideSignStorage.hpp"
#include "../sim/SimGraph.hpp"
#include "../asset/AssetRegistrar.hpp"
#include <exception>
#include <thread>

// =============================================================================
// 初期化
// =============================================================================

GameScene::GameScene(const InitData& init)
	: IScene{ init }
{
	m_renderTexture = RenderTexture{ Scene::Size(), TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes };
	m_outlineMask   = RenderTexture{ Scene::Size(), TextureFormat::R8G8B8A8_Unorm,      HasDepth::Yes };
	m_outlinePS     = HLSL{ U"shaders/hlsl/selection_outline.hlsl", U"PS" };
	if (not m_outlinePS)
	{
		Print << U"[WARN] selection_outline.hlsl load failed";
	}
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
	// シーン全体で使う UI パネルと、編集系の初期テンプレートをここでまとめて準備する。
	m_panelManager.registerPanel(U"edge_info", Vec2{374, static_cast<double>(Scene::Height() - 20)}, true, true);
	m_panelManager.registerPanel(U"node_info", Vec2{312, static_cast<double>(Scene::Height() - 20)}, true, true);
	m_panelManager.registerPanel(U"name_list", Vec2{250, static_cast<double>(Scene::Height() - 20)}, false, true);
	m_panelManager.registerPanel(U"vehicle_info", Vec2{280, static_cast<double>(Scene::Height() - 20)}, true, true);
	m_panelManager.registerPanel(U"building_info", Vec2{320, static_cast<double>(Scene::Height() - 20)}, true, true);
	m_panelManager.registerPanel(U"draw_template", Vec2{374, static_cast<double>(Scene::Height() - 20)}, true, true);
	{
		const double side = Min(Scene::Width(), Scene::Height()) - 80.0;
		m_panelManager.registerPanel(U"minimap_expanded", Vec2{side, side}, true);
	}
	m_panelManager.registerPanel(U"signal_edit", Vec2{700, 550}, true, true);
	m_panelManager.registerPanel(U"guide_sign_edit", Vec2{360, 600}, true, true);
	m_panelManager.registerPanel(U"guide_sign_editor", Vec2{500, 600}, true, true);
	m_panelManager.registerPanel(U"route_info", Vec2{312, static_cast<double>(Scene::Height() - 20)}, true, true);

	// 道路設置テンプレートの初期値（LocalRoad, 2車線）
	m_drawTemplate.roadType   = RoadType::LocalRoad;
	m_drawTemplate.speedLimit = 60.0f;
	m_drawTemplate.lanes      = RoadNetwork::buildDefaultLanes(2, RoadType::LocalRoad);
	RoadNetwork::buildDefaultParts(m_drawTemplate);

	m_roadPresets.load();

	if (getData().isNewGame)
		initNewGame();
	else
		initLoadGame();
}

void GameScene::initLoadGame()
{
	startLoadingPhase(LoadingTask::LoadGame, U"ロード中...", U"セーブデータを読み込み中...", [this]()
	{
		m_loadGameResult = loadGame();
	});
}

void GameScene::initNewGame()
{
	// 新規ゲーム開始時は地形から建物配置までの生成パイプラインを非同期ロード段階へ載せる。
	const auto initResult = MapGenerator::initWorld(getData().seed, m_world);
	m_placeNames = std::move(initResult.placeNames);

	m_world.reserveChunks();

	const float worldCenter = WORLD_SIZE * 0.5f;
	m_camera.setFocus(Vec3{ worldCenter, 0.0, worldCenter });

	m_totalInitChunks = WORLD_CHUNKS * WORLD_CHUNKS;

	startLoadingPhase(LoadingTask::NewGame, U"マップ生成中...", U"地形生成中", [this]()
	{
		generateAllTerrain();
		placeAllSettlements();
		generateAllRoads();
		generateDistrictRoads();
		postProcessRoads();
		applyZonesGlobal();
		placeInitialBuildings();
	});
	Logger << U"[Loading] {} チャンク生成開始"_fmt(m_totalInitChunks);
}

// =============================================================================
void GameScene::setLoadingTitleAndStatus(StringView title, StringView status)
{
	std::lock_guard lock(m_loadingTextMutex);
	m_loadingTitle  = title;
	m_loadingStatus = status;
}

void GameScene::setLoadingStatus(StringView status)
{
	std::lock_guard lock(m_loadingTextMutex);
	m_loadingStatus = status;
}

String GameScene::loadingTitleSnapshot() const
{
	std::lock_guard lock(m_loadingTextMutex);
	return m_loadingTitle;
}

String GameScene::loadingStatusSnapshot() const
{
	std::lock_guard lock(m_loadingTextMutex);
	return m_loadingStatus;
}

void GameScene::startLoadingPhase(LoadingTask task, StringView title, StringView status,
                                  std::function<void()> pipeline)
{
	// ローディング状態を初期化し、進捗表示用の文言と非同期処理を新しいフェーズへ切り替える。
	m_loadingTimer.restart();
	m_loadingTask = task;
	m_loadingFailed = false;
	m_loadingError.clear();
	m_loadGameResult = false;
	setLoadingTitleAndStatus(title, status);
	m_genProgress.store(0.0f);
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
	setLoadingStatus(U"地形生成中");
	const Stopwatch sw{ StartImmediately::Yes };

	// 全チャンクの高さマップを並列生成し、完成後にワールドへ一括で反映する。
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
	setLoadingStatus(U"地区配置中");

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
	m_castleTownCenters.clear();
	for (const auto& s : m_districts)
	{
		if (s.kind == MapGenerator::SettlementKind::CastleTown)
			m_castleTownCenters << s.center;
	}

	m_genProgress.store(kProgressRoads);
}

void GameScene::generateAllRoads()
{
	setLoadingStatus(U"道路生成中");

	MapGenerator::generateGlobalRoads(
		getData().seed, m_districts, m_world, m_network,
		[this](float fraction) {
			m_genProgress.store(kProgressRoads +
				(kProgressDistrict - kProgressRoads) * fraction);
		});
}

void GameScene::generateDistrictRoads()
{
	setLoadingStatus(U"地区内道路生成中");
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
	setLoadingStatus(U"道路ポスト処理中");
	m_genProgress.store(kProgressPostProcess);

	// 道路生成後の形状補正、交差点整理、標識登録までを段階的に実行して走行可能なネットワークへ整える。
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
	step.restart();

	DistrictRoads::straightenCastleTownRoads(m_districts, m_world, m_network);
	Logger << U"[PostProcess] straightenCastleTownRoads: {:.0f}ms"_fmt(step.msF());
	step.restart();

	registerGuideDestinations();
	Logger << U"[PostProcess] registerGuideDestinations: {:.0f}ms"_fmt(step.msF());
	step.restart();

	// 初期生成された道路はすべて建設済み（供用中）とする
	for (const auto& edge : m_network.edges())
	{
		if (edge.id < 0) continue;
		if (RoadEdge* e = m_network.getEdge(edge.id)) e->edgeState = EdgeState::Open;
	}

	m_genProgress.store(kProgressDone);
	Logger << U"[Phase4] PostProcess 完了 ({:.0f}ms)"_fmt(total.msF());
}

// =============================================================================
// 案内標識: NamedDestination 登録 + 自動生成 (plan/21_guide_sign_spec.md)
// =============================================================================

void GameScene::registerGuideDestinations()
{
	// 地区データを道路ノード上の目的地辞書へ写し替え、自動案内標識の再生成まで一気に行う。
	m_network.clearNamedDestinations();

	const auto tierOf = [](MapGenerator::SettlementKind t) -> uint8 {
		switch (t)
		{
		case MapGenerator::SettlementKind::CastleTown:   return 0;
		case MapGenerator::SettlementKind::PostTown: return 1;
		case MapGenerator::SettlementKind::Village:   return 2;
		}
		return 2;
	};

	int registered = 0;
	for (const auto& s : m_districts)
	{
		if (s.name.isEmpty()) continue;

		// 地区中心から最寄り RoadNode を線形検索
		const Vec3 target{ s.center.x, 0.0, s.center.y };
		int   bestNode = -1;
		float bestDistSq = std::numeric_limits<float>::max();
		for (const auto& n : m_network.nodes())
		{
			if (n.id < 0) continue;
			const float dx = static_cast<float>(n.position.x - target.x);
			const float dz = static_cast<float>(n.position.z - target.z);
			const float d2 = dx * dx + dz * dz;
			if (d2 < bestDistSq)
			{
				bestDistSq = d2;
				bestNode = n.id;
			}
		}
		if (bestNode < 0) continue;

		m_network.addNamedDestination(bestNode, s.name, s.reading, tierOf(s.kind));
		++registered;
	}

	m_network.recomputeAllAutoGuideSigns();
	Logger << U"[GuideSign] {} 地区を登録, 案内標識自動生成完了"_fmt(registered);
}

// =============================================================================
// Loading フェーズ更新
// =============================================================================

void GameScene::updateLoading()
{
	// バックグラウンド生成の完了待ちと、成功時の最終初期化・失敗時の画面維持をここで分岐する。
	if (m_generationFuture.valid())
	{
		if (m_generationFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			try
			{
				m_generationFuture.get();
			}
			catch (const std::exception& e)
			{
				m_loadingFailed = true;
				m_loadingError = Unicode::Widen(e.what());
				setLoadingStatus(U"ロード/生成中に例外が発生しました");
				Logger << U"[Loading] Exception: " << m_loadingError;
			}
			catch (...)
			{
				m_loadingFailed = true;
				m_loadingError = U"unknown exception";
				setLoadingStatus(U"ロード/生成中に例外が発生しました");
				Logger << U"[Loading] Unknown exception";
			}

			if (m_loadingFailed)
			{
				drawLoadingScreen(Clamp(m_genProgress.load(), 0.0f, 1.0f));
				return;
			}

			if (m_loadingTask == LoadingTask::LoadGame)
			{
				if (!m_loadGameResult)
				{
					m_loadingFailed = true;
					m_loadingError = U"loadGame() returned false";
					setLoadingStatus(U"ロードに失敗しました。セーブデータの整合性を確認してください");
					Logger << U"[Load] Failed ({:.1f}秒)"_fmt(m_loadingTimer.sF());
					drawLoadingScreen(Clamp(m_genProgress.load(), 0.0f, 1.0f));
					return;
				}

				Logger << U"[Load] 完了 ({:.1f}秒)"_fmt(m_loadingTimer.sF());
				m_minimapRenderer.buildTerrainTexture(m_world);
				m_minimapRenderer.updateRoadOverlay(m_network, m_world);
				m_phase = GamePhase::Playing;
				return;
			}

			Logger << U"[Loading] 全パイプライン完了 ({:.1f}秒)"_fmt(m_loadingTimer.sF());
			setLoadingStatus(U"初期化中...");

			m_roadRenderer.invalidateAllCaches();
			MapGenerator::setupTrain(m_trainNetwork, m_world, m_districts);

			const float worldCenter = WORLD_SIZE * 0.5f;
			Vec3 cameraFocus{ worldCenter, 0.0, worldCenter };
			for (const auto& s : m_districts)
			{
				if (s.kind == MapGenerator::SettlementKind::CastleTown)
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
	const String loadingTitle = loadingTitleSnapshot();
	const String loadingStatus = loadingStatusSnapshot();

	const int elapsedSec = static_cast<int>(m_loadingTimer.sF());
	const int min = elapsedSec / 60;
	const int sec = elapsedSec % 60;
	uiFont(U"{}:{:0>2}"_fmt(min, sec)).drawAt(
		center.movedBy(0, -110), ColorF{ 0.7 });

	titleFont(loadingTitle).drawAt(center.movedBy(0, -60), ColorF{ 0.9 });

	const RectF barBg{ center.x - 200, center.y, 400, 24 };
	barBg.draw(ColorF{ 0.2 });
	RectF{ barBg.pos, barBg.w * progress, barBg.h }.draw(ColorF{ 0.3, 0.7, 1.0 });

	uiFont(U"{}%"_fmt(static_cast<int>(progress * 100))).drawAt(barBg.center(), ColorF{ 1.0 });

	const ColorF statusColor = m_loadingFailed ? ColorF{ 1.0, 0.45, 0.45 } : ColorF{ 0.5 };
	smallFont(loadingStatus).drawAt(center.movedBy(0, 70), statusColor);

	if (m_loadingFailed && !m_loadingError.isEmpty())
	{
		const String clipped = m_loadingError.substr(0, Min<size_t>(80, m_loadingError.size()));
		smallFont(U"Error: {}"_fmt(clipped)).drawAt(center.movedBy(0, 94), ColorF{ 0.85, 0.35, 0.35 });
	}
}

// =============================================================================
// セーブ / ロード
// =============================================================================

void GameScene::saveGame()
{
	if (getData().saveName.isEmpty())
		getData().saveName = U"default";

	const String saveRoot = U"saves/{}"_fmt(getData().saveName);
	FileSystem::CreateDirectories(U"{}/global"_fmt(saveRoot));

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
	meta.save(U"{}/meta.json"_fmt(saveRoot));

	// economy.json
	JSON eco;
	eco[U"funds"]      = m_economy.funds;
	eco[U"population"] = m_economy.population;
	eco[U"happiness"]  = m_economy.happiness;
	eco.save(U"{}/global/economy.json"_fmt(saveRoot));

	// roads.bin
	RoadBinary::writeGlobal(U"{}/global/roads.bin"_fmt(saveRoot), m_network);

	// 案内標識（独立 JSON、テクスチャはランタイム再生成）
	GuideSignStorage::writeJson(U"{}/global/guide_signs.json"_fmt(saveRoot), m_network);

	// districts.json
	JSON dist;
	dist[U"count"] = static_cast<int>(m_districts.size());
	for (int i = 0; i < static_cast<int>(m_districts.size()); ++i)
	{
		const auto& s = m_districts[i];
		dist[U"type_{}"_fmt(i)]    = static_cast<int>(s.kind);
		dist[U"cx_{}"_fmt(i)]      = s.center.x;
		dist[U"cy_{}"_fmt(i)]      = s.center.y;
		dist[U"radius_{}"_fmt(i)]  = s.radius;
		dist[U"score_{}"_fmt(i)]   = s.score;
		dist[U"name_{}"_fmt(i)]    = s.name;
		dist[U"reading_{}"_fmt(i)] = s.reading;
	}
	dist.save(U"{}/global/districts.json"_fmt(saveRoot));

	// 地形データ（チャンクごと）
	const String chunksDir = U"{}/chunks"_fmt(saveRoot);
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

	Console << U"[Save] Saved to " << saveRoot;
}

// =============================================================================
// 地形チャンク並列ロード（loadGame のサブルーチン）
// =============================================================================

void GameScene::loadTerrainChunks(const String& saveRoot, Stopwatch& step)
{
	// 地形チャンクは並列に読み戻し、欠損しているものだけ再生成してロード失敗を吸収する。
	// 地形データ（チャンクごとに並列読み込み + バルク I/O）
	const int totalChunks = WORLD_CHUNKS * WORLD_CHUNKS;
	setLoadingStatus(U"地形データ読み込み中...");
	{
		Array<HeightMapResult> buffer(totalChunks);
		std::atomic<int> counter{ 0 };
		std::atomic<int> loadedAtomic{ 0 };
		const int nThreads = Max(1, static_cast<int>(std::thread::hardware_concurrency()));
		Array<std::thread> threads;
		threads.reserve(nThreads);

		for (int t = 0; t < nThreads; ++t)
		{
			threads.emplace_back([this, &buffer, &counter, &loadedAtomic, totalChunks, &saveRoot]()
			{
				while (true)
				{
					const int idx = counter.fetch_add(1);
					if (idx >= totalChunks) break;
					const int cx = idx % WORLD_CHUNKS;
					const int cy = idx / WORLD_CHUNKS;

					const String terrainPath = U"{}/chunks/{}_{}/terrain.bin"_fmt(saveRoot, cx, cy);
					bool loaded = false;

					if (FileSystem::Exists(terrainPath))
					{
						BinaryReader r{ terrainPath };
						if (r)
						{
							int32 gridSize = 0;
							if (r.read(gridSize) && gridSize == HEIGHT_CELLS + 1)
							{
								HeightMapResult hmr;
								hmr.heightMap = Grid<float>(gridSize, gridSize);
								// バルク読み込み: gridSize² 個の float を1 回で取得
								const size_t cellCount = static_cast<size_t>(gridSize) * gridSize;
								const int64 expectedBytes = static_cast<int64>(cellCount * sizeof(float));
								if (r.read(hmr.heightMap.data(), static_cast<size_t>(expectedBytes)) == expectedBytes)
								{
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
					}

					if (!loaded)
					{
						buffer[idx] = m_world.buildHeightMap(Point{ cx, cy });
					}
					const int done = loadedAtomic.fetch_add(1) + 1;
					m_genProgress.store(static_cast<float>(done) / static_cast<float>(totalChunks) * 0.7f);
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
}

// =============================================================================
// ゲームデータ保存・ロード
// =============================================================================

bool GameScene::loadGame()
{
	const Stopwatch loadTotal{ StartImmediately::Yes };
	Stopwatch step{ StartImmediately::Yes };

	// セーブ一式を読み戻し、道路・地区・建物・時計・カメラまでプレイ直前の状態に復元する。
	const String saveRoot = U"saves/{}"_fmt(getData().saveName);

	// meta.json
	const JSON meta = JSON::Load(U"{}/meta.json"_fmt(saveRoot));
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
		WORLD_SIZE,
		WORLD_SIZE);
	m_world.reserveChunks();
	Console << U"[Load] meta+init: {:.0f}ms"_fmt(step.msF());
	step.restart();

	loadTerrainChunks(saveRoot, step);

	// 経済
	if (const JSON eco = JSON::Load(U"{}/global/economy.json"_fmt(saveRoot)))
	{
		m_economy.funds      = eco[U"funds"].get<double>();
		m_economy.population = eco[U"population"].get<int>();
		m_economy.happiness  = eco[U"happiness"].get<double>();
	}

	// 道路ネットワーク
	{
		const String roadPath = U"{}/global/roads.bin"_fmt(saveRoot);
		const bool roadOk = RoadBinary::readGlobal(roadPath, m_network);
		// 案内標識（独立 JSON。テクスチャは render 時に prepareGuideSignTextures で合成）
		m_network.clearGuideSigns();
		GuideSignStorage::readJson(U"{}/global/guide_signs.json"_fmt(saveRoot), m_network);
		Console << U"[Load] roads: " << (roadOk ? U"OK" : U"FAILED (format mismatch? re-save needed)")
		        << U" nodes=" << m_network.nodes().size()
		        << U" edges=" << m_network.edges().size();
	}
	m_network.setNextIds(nextNodeId, nextEdgeId);

	m_genProgress.store(0.8f);
	setLoadingStatus(U"道路・経済データ復元完了");
	Console << U"[Load] economy+roads: {:.0f}ms"_fmt(step.msF());
	step.restart();

	// 集落
	m_districts.clear();
	m_castleTownCenters.clear();
	if (const JSON dist = JSON::Load(U"{}/global/districts.json"_fmt(saveRoot)))
	{
		const int count = dist[U"count"].get<int>();
		Array<MapGenerator::Settlement> settlements;
		for (int i = 0; i < count; ++i)
		{
			MapGenerator::Settlement s;
			s.kind   = static_cast<MapGenerator::SettlementKind>(dist[U"type_{}"_fmt(i)].get<int>());
			s.center = Vec2{ dist[U"cx_{}"_fmt(i)].get<double>(), dist[U"cy_{}"_fmt(i)].get<double>() };
			s.name   = dist[U"name_{}"_fmt(i)].get<String>();
			const String radiusKey = U"radius_{}"_fmt(i);
			if (dist.hasElement(radiusKey))
				s.radius = dist[radiusKey].get<float>();
			else
			{
				// 旧セーブとの互換: 種別からデフォルト半径を復元
				s.radius = (s.kind == MapGenerator::SettlementKind::CastleTown)   ? 700.0f
				         : (s.kind == MapGenerator::SettlementKind::PostTown) ? 300.0f
				                                                             : 150.0f;
			}
			const String scoreKey = U"score_{}"_fmt(i);
			if (dist.hasElement(scoreKey))
				s.score = dist[scoreKey].get<float>();
			const String readingKey = U"reading_{}"_fmt(i);
			if (dist.hasElement(readingKey))
				s.reading = dist[readingKey].get<String>();
			settlements << s;
		}
		addDistricts(settlements);
	}

	m_genProgress.store(0.9f);
	setLoadingStatus(U"ゾーン・建物を復元中...");

	applyZonesGlobal();
	placeInitialBuildings();
	registerGuideDestinations();

	m_roadRenderer.invalidateAllCaches();

	m_clock.now   = gameNow;
	m_clock.speed = static_cast<TimeSpeed>(timeScale);
	m_clock.syncCalendar();

	m_camera.setState(Vec3{ focusX, focusY, focusZ }, camDist, camYaw, camPitch);

	MapGenerator::setupTrain(m_trainNetwork, m_world, m_districts);

	m_world.update(m_camera.focusPoint());
	startSimThread();

	Console << U"[Load] finish: {:.0f}ms"_fmt(step.msF());
	m_genProgress.store(1.0f);
	Console << U"[Load] TOTAL: {:.0f}ms from {}"_fmt(loadTotal.msF(), saveRoot);
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
		if (s.kind == MapGenerator::SettlementKind::CastleTown)
			m_castleTownCenters << s.center;
	}
}

// =============================================================================
// ゾーン一括割り当て
// =============================================================================

void GameScene::applyZonesGlobal()
{
	const Stopwatch sw{ StartImmediately::Yes };
	constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;

	// 各地区の性格と中心距離に応じて、周辺チャンクへ住宅・商業系ゾーンを面で割り当てる。
	for (const auto& s : m_districts)
	{
		const float innerDist = (s.kind == MapGenerator::SettlementKind::CastleTown)    ? 400.0f
		                      : (s.kind == MapGenerator::SettlementKind::PostTown)  ? 200.0f : 100.0f;
		const float midDist   = (s.kind == MapGenerator::SettlementKind::CastleTown)    ? 800.0f
		                      : (s.kind == MapGenerator::SettlementKind::PostTown)  ? 600.0f : 300.0f;
		const float outerDist = (s.kind == MapGenerator::SettlementKind::CastleTown)    ? 1200.0f
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
							zt = (s.kind == MapGenerator::SettlementKind::CastleTown)
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
	constexpr float kDefaultBuildingSetbackM = 2.0f;

	String buildingTomlPathFromStem(const String& stem)
	{
		if (stem.starts_with(U"residential_"))
			return U"assets/buildings/residential/{}.toml"_fmt(stem);
		return U"assets/buildings/commercial/{}.toml"_fmt(stem);
	}

	float loadSetbackFromToml(const String& stem)
	{
		const String tomlPath = buildingTomlPathFromStem(stem);
		const TOMLReader toml{ tomlPath };
		if (!toml)
		{
			Console << U"[BuildingSetback] TOML not found/invalid: " << tomlPath
			        << U" (fallback=" << kDefaultBuildingSetbackM << U"m)";
			return kDefaultBuildingSetbackM;
		}

		const double setback = toml[U"setback_from_road_m"].getOr<double>(
			toml[U"setback_m"].getOr<double>(kDefaultBuildingSetbackM));
		return static_cast<float>(Max(0.0, setback));
	}

	float setbackFromRoadByModel(BuildingType type, int gx, int gz)
	{
		String stem;
		if (!tryGetBuildingModelStem(type, gx, gz, stem))
			return kDefaultBuildingSetbackM;

		static HashTable<String, float> s_cache;
		if (const auto it = s_cache.find(stem); it != s_cache.end())
			return it->second;

		const float value = loadSetbackFromToml(stem);
		s_cache[stem] = value;
		return value;
	}

	float buildingAngleFromAttachedEdge(
		const RoadNetwork& network, int edgeId, float edgeT, float fallbackAngle)
	{
		if (edgeId < 0) return fallbackAngle;
		const auto bez = network.getBezier(edgeId);
		if (!bez || bez->totalLength <= 1e-3f) return fallbackAngle;
		const float t = Clamp(edgeT, 0.0f, 1.0f);
		const Vec3 tan = bez->tangentAt(bez->totalLength * t);
		Vec2 dir{ static_cast<float>(tan.x), static_cast<float>(tan.z) };
		if (dir.lengthSq() <= 1e-8f) return fallbackAngle;
		dir.normalize();

		return static_cast<float>(std::atan2(dir.y, dir.x));
	}

	struct EdgeProjection
	{
		float edgeT = 0.0f;
		Vec2  position{ 0.0f, 0.0f };
		Vec2  tangent{ 1.0f, 0.0f };
		Vec2  right{ 0.0f, -1.0f };
		float angle = 0.0f;
		float distance = 0.0f;
	};

	bool projectPointToEdgeXZ(const RoadNetwork& network, int edgeId, const Vec2& point, EdgeProjection& out)
	{
		const auto bez = network.getBezier(edgeId);
		if (!bez || bez->totalLength <= 1e-3f) return false;

		const int sampleCount = Max(32, static_cast<int>(Ceil(bez->totalLength / 8.0f)));
		float bestArc = 0.0f;
		double bestDistSq = Math::Inf;
		int bestIndex = 0;

		for (int i = 0; i <= sampleCount; ++i)
		{
			const float arc = bez->totalLength * (static_cast<float>(i) / sampleCount);
			const Vec3 pos = bez->positionAt(arc);
			const double dx = pos.x - point.x;
			const double dz = pos.z - point.y;
			const double distSq = dx * dx + dz * dz;
			if (distSq < bestDistSq)
			{
				bestDistSq = distSq;
				bestArc = arc;
				bestIndex = i;
			}
		}

		float lo = bez->totalLength * (Max(bestIndex - 1, 0) / static_cast<float>(sampleCount));
		float hi = bez->totalLength * (Min(bestIndex + 1, sampleCount) / static_cast<float>(sampleCount));
		for (int iter = 0; iter < 24; ++iter)
		{
			const float m1 = lo + (hi - lo) / 3.0f;
			const float m2 = hi - (hi - lo) / 3.0f;
			const Vec3 p1 = bez->positionAt(m1);
			const Vec3 p2 = bez->positionAt(m2);
			const double d1 = (p1.x - point.x) * (p1.x - point.x) + (p1.z - point.y) * (p1.z - point.y);
			const double d2 = (p2.x - point.x) * (p2.x - point.x) + (p2.z - point.y) * (p2.z - point.y);
			if (d1 < d2) hi = m2;
			else lo = m1;
		}

		bestArc = (lo + hi) * 0.5f;
		const Vec3 pos = bez->positionAt(bestArc);
		const Vec3 tan = bez->tangentAt(bestArc);
		Vec2 tangent{ static_cast<float>(tan.x), static_cast<float>(tan.z) };
		if (tangent.lengthSq() <= 1e-8f) return false;
		tangent.normalize();

		out.edgeT = Clamp(bestArc / bez->totalLength, 0.0f, 1.0f);
		out.position = Vec2{ static_cast<float>(pos.x), static_cast<float>(pos.z) };
		out.tangent = tangent;
		out.right = Vec2{ tangent.y, -tangent.x };
		out.angle = static_cast<float>(std::atan2(tangent.y, tangent.x));
		out.distance = static_cast<float>(Math::Sqrt(
			(out.position.x - point.x) * (out.position.x - point.x)
			+ (out.position.y - point.y) * (out.position.y - point.y)));
		return true;
	}

	struct EdgeFacingSlot
	{
		Point chunkCoord;
		int   col = 0;
		int   row = 0;
		int   edgeId = -1;
		float edgeT = 0.0f;
		float angle = 0.0f;
		float halfWidth = 0.0f;
		float roadDist = 0.0f;
		float centerDistSq = 0.0f;
	};

	int64 zoneCellKey(Point cc, int col, int row)
	{
		return (chunkCoordToKey(cc) << 16) ^ (static_cast<int64>(row) << 8) ^ static_cast<uint32>(col);
	}

	bool worldToZoneCell(float wx, float wz, Point& outChunk, int& outCol, int& outRow)
	{
		const int chunkX = static_cast<int>(Math::Floor(wx / CHUNK_SIZE));
		const int chunkZ = static_cast<int>(Math::Floor(wz / CHUNK_SIZE));
		const float lx = wx - static_cast<float>(chunkX * CHUNK_SIZE);
		const float lz = wz - static_cast<float>(chunkZ * CHUNK_SIZE);
		outChunk = Point{ chunkX, chunkZ };
		outCol = Clamp(static_cast<int>(lx / (static_cast<float>(CHUNK_SIZE) / ZONE_CELLS)), 0, ZONE_CELLS - 1);
		outRow = Clamp(static_cast<int>(lz / (static_cast<float>(CHUNK_SIZE) / ZONE_CELLS)), 0, ZONE_CELLS - 1);
		return true;
	}

	Vec2 cellCenterXZ(Point cc, int col, int row)
	{
		constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
		return Vec2{
			static_cast<float>(cc.x * CHUNK_SIZE) + (col + 0.5f) * cellSize,
			static_cast<float>(cc.y * CHUNK_SIZE) + (row + 0.5f) * cellSize
		};
	}

	bool overlapsExistingBuilding(
		const World& world,
		Point cc,
		int gx,
		int gz,
		float wx,
		float wz,
		float halfBuilding)
	{
		constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
		const int checkRange = static_cast<int>(Ceil((halfBuilding * 2.0f) / cellSize)) + 1;
		for (int oy = -checkRange; oy <= checkRange; ++oy)
		{
			for (int ox = -checkRange; ox <= checkRange; ++ox)
			{
				int nx = gx + ox;
				int nz = gz + oy;
				Point ncc = cc;
				if (nx < 0) { nx += ZONE_CELLS; --ncc.x; }
				else if (nx >= ZONE_CELLS) { nx -= ZONE_CELLS; ++ncc.x; }
				if (nz < 0) { nz += ZONE_CELLS; --ncc.y; }
				else if (nz >= ZONE_CELLS) { nz -= ZONE_CELLS; ++ncc.y; }

				const Chunk* nchunk = world.getChunk(ncc);
				if (!nchunk) continue;
				const Building& nb = nchunk->buildingGrid[{ nx, nz }];
				if (nb.type == BuildingType::None) continue;

				const Vec2 ncenter = cellCenterXZ(ncc, nx, nz);
				const float nHalf = buildingFootprintXZ() * 0.5f;
				if (Math::Abs(wx - ncenter.x) < (halfBuilding + nHalf)
				 && Math::Abs(wz - ncenter.y) < (halfBuilding + nHalf))
				{
					return true;
				}
			}
		}
		return false;
	}

	Array<EdgeFacingSlot> collectEdgeFacingSlots(
		const MapGenerator::Settlement& settlement,
		const World& world,
		const RoadNetwork& network)
	{
		const float halfBuilding = buildingFootprintXZ() * 0.5f;
		const float offsetFromEdge = halfBuilding + kDefaultBuildingSetbackM;
		const Vec2 center{ settlement.center.x, settlement.center.y };
		const float searchRadius = static_cast<float>(settlement.radius * 2.0);
		const float radiusSq = searchRadius * searchRadius;

		HashTable<int64, EdgeFacingSlot> bestByCell;

		for (const auto& edge : network.edges())
		{
			if (edge.id < 0 || !edge.isRoadbedBuilt()) continue;
			if (edge.roadType != RoadType::LocalRoad) continue;

			const auto bez = network.getBezier(edge.id);
			if (!bez || bez->totalLength <= 1.0f) continue;
			const float edgeHalfWidth = edge.totalWidth() * 0.5f;
			const int sampleCount = Max(4, static_cast<int>(Ceil(bez->totalLength / 24.0f)));

			for (int i = 0; i <= sampleCount; ++i)
			{
				const float t = static_cast<float>(i) / sampleCount;
				const float arc = bez->totalLength * t;
				const Vec3 pos = bez->positionAt(arc);
				const Vec3 tan = bez->tangentAt(arc);
				Vec2 tangent{ static_cast<float>(tan.x), static_cast<float>(tan.z) };
				if (tangent.lengthSq() <= 1e-8f) continue;
				tangent.normalize();
				const Vec2 right{ tangent.y, -tangent.x };

				const float dcx = static_cast<float>(pos.x) - static_cast<float>(center.x);
				const float dcz = static_cast<float>(pos.z) - static_cast<float>(center.y);

				if (settlement.kind == MapGenerator::SettlementKind::CastleTown
					&& settlement.gridAxisX.lengthSq() > 1e-6f)
				{
					// 城下町はグリッドフレーム（矩形）内のエッジのみ使用し、隣接城下町の混入を防ぐ
					const Vec2 d{ dcx, dcz };
					const float halfExtent = Max(1000.0f, Min(4000.0f, static_cast<float>(settlement.radius) * 0.6f));
					const float frameSize = halfExtent + 80.0f;
					if (Math::Abs(d.dot(settlement.gridAxisX)) > frameSize) continue;
					if (Math::Abs(d.dot(settlement.gridAxisZ)) > frameSize) continue;
				}
				else
				{
					if (dcx * dcx + dcz * dcz > radiusSq) continue;
				}

				for (const float side : { -1.0f, 1.0f })
				{
					const Vec2 slotPos = Vec2{ static_cast<float>(pos.x), static_cast<float>(pos.z) }
						+ right * side * (edgeHalfWidth + offsetFromEdge);

					Point cc;
					int gx = 0, gz = 0;
					worldToZoneCell(static_cast<float>(slotPos.x), static_cast<float>(slotPos.y), cc, gx, gz);

					const Chunk* chunk = world.getChunk(cc);
					if (!chunk) continue;
					const ZoneType zone = chunk->zoneMap[{ gx, gz }];
					if (zone == ZoneType::Unzoned) continue;

					const Vec2 cellCenter = cellCenterXZ(cc, gx, gz);
					EdgeProjection projection;
					if (!projectPointToEdgeXZ(network, edge.id, cellCenter, projection)) continue;

					const Vec2 toCell = cellCenter - projection.position;
					if (toCell.lengthSq() <= 1e-6f) continue;
					if ((toCell.dot(projection.right) * side) <= 1e-4f) continue;

					const float centerDx = static_cast<float>(cellCenter.x) - static_cast<float>(center.x);
					const float centerDz = static_cast<float>(cellCenter.y) - static_cast<float>(center.y);

					EdgeFacingSlot slot;
					slot.chunkCoord = cc;
					slot.col = gx;
					slot.row = gz;
					slot.edgeId = edge.id;
					slot.edgeT = projection.edgeT;
					slot.angle = projection.angle;
					slot.halfWidth = edgeHalfWidth;
					slot.roadDist = projection.distance;
					slot.centerDistSq = centerDx * centerDx + centerDz * centerDz;

					const int64 key = zoneCellKey(cc, gx, gz);
					const auto it = bestByCell.find(key);
					if (it == bestByCell.end()
					 || slot.roadDist < it->second.roadDist
					 || (Math::Abs(slot.roadDist - it->second.roadDist) < 1e-4f && slot.centerDistSq < it->second.centerDistSq))
					{
						bestByCell[key] = slot;
					}
				}
			}
		}

		Array<EdgeFacingSlot> slots;
		slots.reserve(bestByCell.size());
		for (const auto& [_, slot] : bestByCell) slots << slot;
		slots.sort_by([](const EdgeFacingSlot& a, const EdgeFacingSlot& b)
		{
			if (a.centerDistSq != b.centerDistSq) return a.centerDistSq < b.centerDistSq;
			if (a.chunkCoord.x != b.chunkCoord.x) return a.chunkCoord.x < b.chunkCoord.x;
			if (a.chunkCoord.y != b.chunkCoord.y) return a.chunkCoord.y < b.chunkCoord.y;
			if (a.row != b.row) return a.row < b.row;
			return a.col < b.col;
		});
		return slots;
	}
}

void GameScene::placeInitialBuildings()
{
	const Stopwatch sw{ StartImmediately::Yes };
	constexpr float kNearDist      = 32.0f;
	constexpr float kFarDist       = 64.0f;
	const float buildingSize       = buildingFootprintXZ();
	const float halfBuilding       = buildingSize * 0.5f;
	int placed = 0;

	// 道路沿いスロットを地区ごとに収集し、ゾーン・道路距離・重なり判定を満たす場所へ初期建物を置く。
	for (int si = 0; si < static_cast<int>(m_districts.size()); ++si)
	{
		const auto& s = m_districts[si];

		// デバッグ: 城下町の gridAxisX を確認
		if (s.kind == MapGenerator::SettlementKind::CastleTown)
		{
			Console << U"[Debug CastleTown si={}] gridAxisX=({:.3f},{:.3f})"_fmt(
				si, s.gridAxisX.x, s.gridAxisX.y);
		}

		const float radiusSq = s.radius * s.radius * 4.0f;
		const Array<EdgeFacingSlot> slots = collectEdgeFacingSlots(s, m_world, m_network);
		for (const auto& slot : slots)
		{
			Chunk* chunk = m_world.getChunk(slot.chunkCoord);
			if (!chunk) continue;
			if (chunk->buildingGrid[{ slot.col, slot.row }].type != BuildingType::None) continue;

			const ZoneType zone = chunk->zoneMap[{ slot.col, slot.row }];
			if (zone == ZoneType::Unzoned) continue;

			const Vec2 centerPos = cellCenterXZ(slot.chunkCoord, slot.col, slot.row);
			const float dx = static_cast<float>(centerPos.x - s.center.x);
			const float dz = static_cast<float>(centerPos.y - s.center.y);
			const float distFromCenterSq = dx * dx + dz * dz;
			if (distFromCenterSq > radiusSq) continue;

				const float h = sampleHeightMap(
					chunk->heightMap, slot.chunkCoord,
					static_cast<float>(centerPos.x), static_cast<float>(centerPos.y));
				if (h < 0.0f) continue;
				if (overlapsExistingBuilding(
					m_world, slot.chunkCoord, slot.col, slot.row,
					static_cast<float>(centerPos.x), static_cast<float>(centerPos.y), halfBuilding)) continue;

			float roadScore;
			if      (slot.roadDist < kNearDist) roadScore = 1.0f;
			else if (slot.roadDist < kFarDist)  roadScore = 1.0f - (slot.roadDist - kNearDist) / (kFarDist - kNearDist);
			else                                roadScore = 0.0f;

			const float distFromCenter = Math::Sqrt(distFromCenterSq);
			const float densityFactor = Clamp(0.25f * (1.0f - distFromCenter / (s.radius * 2.0f)), 0.0f, 0.25f);
			const float score = roadScore * densityFactor;
			if (score < 0.02f) continue;

			const uint32 cellHash = static_cast<uint32>(
				(slot.col * 73856093) ^ (slot.row * 19349663) ^ (slot.chunkCoord.x * 83492791) ^ (slot.chunkCoord.y * 41729581));
			const float roll = (cellHash % 1000) / 1000.0f;
			if (roll > score) continue;

			Building b = m_zoneManager.spawnBuilding(zone, 0.0);
			if (b.type == BuildingType::None) continue;

			const int globalGX = slot.chunkCoord.x * ZONE_CELLS + slot.col;
			const int globalGZ = slot.chunkCoord.y * ZONE_CELLS + slot.row;
			const float setbackM = setbackFromRoadByModel(b.type, globalGX, globalGZ);
			if (slot.roadDist < (slot.halfWidth + halfBuilding + setbackM)) continue;

			b.angle = buildingAngleFromAttachedEdge(m_network, slot.edgeId, slot.edgeT, slot.angle);
			b.edgeId = slot.edgeId;
			b.edgeT = slot.edgeT;

			// デバッグ: 城下町の最初の5棟だけ接線を確認
			if (s.kind == MapGenerator::SettlementKind::CastleTown && placed < 5)
			{
				const auto bez = m_network.getBezier(slot.edgeId);
				if (bez)
				{
					const Vec3 tan = bez->tangentAt(bez->totalLength * 0.5f);
					Console << U"[Debug building #{}] edgeId={} tan=({:.3f},{:.3f}) angle={:.2f}deg gridAxisX=({:.3f},{:.3f})"_fmt(
						placed,
						slot.edgeId,
						tan.x, tan.z,
						Math::ToDegrees(b.angle),
						s.gridAxisX.x, s.gridAxisX.y);
				}
			}

			chunk->buildingGrid[{ slot.col, slot.row }] = b;
			chunk->meshDirty = true;
			++placed;
		}
	}

	Logger << U"[placeInitialBuildings] {} 棟配置 ({:.0f}ms)"_fmt(placed, sw.msF());
	refreshBuildingAnglesFromEdges();
}

void GameScene::refreshBuildingAnglesFromEdges()
{
	int updated = 0;
	for (Chunk* chunk : m_world.getActiveChunks())
	{
		if (!chunk) continue;
		bool chunkChanged = false;
		for (int row = 0; row < ZONE_CELLS; ++row)
		{
			for (int col = 0; col < ZONE_CELLS; ++col)
			{
				Building& b = chunk->buildingGrid[{ col, row }];
				if (b.type == BuildingType::None) continue;
				if (b.edgeId < 0) continue;

				const float newAngle = buildingAngleFromAttachedEdge(m_network, b.edgeId, b.edgeT, b.angle);
				if (Math::Abs(newAngle - b.angle) <= 1e-4f) continue;
				b.angle = newAngle;
				chunkChanged = true;
				++updated;
			}
		}
		if (chunkChanged) chunk->meshDirty = true;
	}

	if (updated > 0)
	{
		Logger << U"[refreshBuildingAnglesFromEdges] {} buildings"_fmt(updated);
	}
}

// =============================================================================
// 施工中エッジの Open 遷移
// =============================================================================

void GameScene::tickConstruction()
{
	// 工事中の道路計画を監視し、工期満了時に所属エッジを同時開通する。
	Array<int> completedPlanIds;
	for (const auto& plan : m_network.plans())
	{
		if (plan.id < 0) continue;
		if (plan.state != PlanState::UnderConstruction) continue;
		if (!plan.completionDate || m_clock.now < *plan.completionDate) continue;
		completedPlanIds << plan.id;
	}
	Array<int> dirtyNodes;
	for (const int planId : completedPlanIds)
	{
		const RoadPlan* plan = m_network.getPlan(planId);
		if (!plan) continue;
		for (const int eid : plan->edgeIds)
		{
			if (const RoadEdge* edge = m_network.getEdge(eid))
				dirtyNodes << edge->nodeA << edge->nodeB;
		}
		m_network.completePlanConstruction(planId);
	}
	if (!dirtyNodes.isEmpty())
	{
		for (const int nid : dirtyNodes)
			m_roadRenderer.invalidateCachesAroundNode(nid, m_network);
		notifyNetworkChanged(dirtyNodes);
	}
}

// =============================================================================
// 毎フレーム更新
// =============================================================================

void GameScene::update()
{
	// シミュレーション応答、時間進行、入力、カメラ、描画準備を毎フレームここで順に同期させる。
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
		tickConstruction();
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
