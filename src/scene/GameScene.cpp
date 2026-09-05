#include "GameScene.hpp"
#include "../gen/RoadPathfinder.hpp"
#include "../gen/DistrictRoads.hpp"
#include "../save/RoadBinary.hpp"
#include "../save/GuideSignStorage.hpp"
#include "../save/SaveTransaction.hpp"
#include "../sim/SimGraph.hpp"
#include "../asset/AssetRegistrar.hpp"
#include "../road/RoadGeometry.hpp"
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
	{
		getData().saveName = U"default";
	}
	if (getData().saveName.contains(U'/') || getData().saveName.contains(U'\\')
		|| getData().saveName == U"." || getData().saveName == U"..")
	{
		DebugLog::print(U"[Save] Invalid save name: {}"_fmt(getData().saveName));
		return;
	}

	const FilePath saveRoot = U"saves/{}"_fmt(getData().saveName);
	const SaveResult result = SaveTransaction::commit(saveRoot,
		[this](const FilePath& temporaryDirectory)
		{
			return writeGameSnapshot(temporaryDirectory);
		},
		[this](const FilePath& temporaryDirectory)
		{
			return verifyGameSnapshot(temporaryDirectory);
		});

	if (result)
	{
		DebugLog::print(U"[Save] Saved atomically to {}"_fmt(result.path));
	}
	else
	{
		DebugLog::print(U"[Save] Failed: {} ({})"_fmt(result.message, result.path));
	}
}

SaveResult GameScene::writeGameSnapshot(const FilePath& saveRoot) const
{
	constexpr int kSaveVersion = 3;
	const FilePath globalDirectory = saveRoot + U"/global";
	if (!FileSystem::CreateDirectories(globalDirectory))
	{
		return SaveResult::failed(SaveError::WriteFailed,
			U"global ディレクトリを作成できません", globalDirectory);
	}

	JSON meta;
	meta[U"version"] = kSaveVersion;
	meta[U"seed"] = getData().seed;
	meta[U"worldChunks"] = WORLD_CHUNKS;
	meta[U"gameNow"] = m_clock.now;
	meta[U"timeScale"] = static_cast<int>(m_clock.speed);
	meta[U"nextNodeId"] = m_network.nextNodeId();
	meta[U"nextEdgeId"] = m_network.nextEdgeId();
	meta[U"cameraFocusX"] = m_camera.focusPoint().x;
	meta[U"cameraFocusY"] = m_camera.focusPoint().y;
	meta[U"cameraFocusZ"] = m_camera.focusPoint().z;
	meta[U"cameraDistance"] = m_camera.distance();
	meta[U"cameraYaw"] = m_camera.yaw();
	meta[U"cameraPitch"] = m_camera.pitch();
	if (!meta.save(saveRoot + U"/meta.json"))
	{
		return SaveResult::failed(SaveError::WriteFailed, U"meta.json を保存できません", saveRoot);
	}

	JSON economy;
	economy[U"funds"] = m_economy.funds;
	economy[U"population"] = m_economy.population;
	economy[U"happiness"] = m_economy.happiness;
	if (!economy.save(globalDirectory + U"/economy.json"))
	{
		return SaveResult::failed(SaveError::WriteFailed, U"economy.json を保存できません", saveRoot);
	}
	if (!RoadBinary::writeGlobal(globalDirectory + U"/roads.bin", m_network))
	{
		return SaveResult::failed(SaveError::WriteFailed, U"roads.bin を保存できません", saveRoot);
	}
	if (!GuideSignStorage::writeJson(globalDirectory + U"/guide_signs.json", m_network))
	{
		return SaveResult::failed(SaveError::WriteFailed, U"guide_signs.json を保存できません", saveRoot);
	}

	JSON districts;
	districts[U"count"] = static_cast<int>(m_districts.size());
	for (int index = 0; index < static_cast<int>(m_districts.size()); ++index)
	{
		const auto& settlement = m_districts[index];
		districts[U"type_{}"_fmt(index)] = static_cast<int>(settlement.kind);
		districts[U"cx_{}"_fmt(index)] = settlement.center.x;
		districts[U"cy_{}"_fmt(index)] = settlement.center.y;
		districts[U"radius_{}"_fmt(index)] = settlement.radius;
		districts[U"score_{}"_fmt(index)] = settlement.score;
		districts[U"name_{}"_fmt(index)] = settlement.name;
		districts[U"reading_{}"_fmt(index)] = settlement.reading;
	}
	if (!districts.save(globalDirectory + U"/districts.json"))
	{
		return SaveResult::failed(SaveError::WriteFailed, U"districts.json を保存できません", saveRoot);
	}

	const FilePath chunksDirectory = saveRoot + U"/chunks";
	for (int chunkY = 0; chunkY < WORLD_CHUNKS; ++chunkY)
	{
		for (int chunkX = 0; chunkX < WORLD_CHUNKS; ++chunkX)
		{
			const Chunk* chunk = m_world.getChunk(Point{ chunkX, chunkY });
			if (!chunk || chunk->heightMap.isEmpty())
			{
				continue;
			}

			const FilePath chunkDirectory = U"{}/{}_{}"_fmt(chunksDirectory, chunkX, chunkY);
			if (!FileSystem::CreateDirectories(chunkDirectory))
			{
				return SaveResult::failed(SaveError::WriteFailed,
					U"チャンクディレクトリを作成できません", chunkDirectory);
			}
			BinaryWriter writer{ chunkDirectory + U"/terrain.bin" };
			if (!writer)
			{
				return SaveResult::failed(SaveError::WriteFailed,
					U"terrain.bin を作成できません", chunkDirectory);
			}

			const int gridSize = HEIGHT_CELLS + 1;
			writer.write(static_cast<int32>(gridSize));
			for (int row = 0; row < gridSize; ++row)
			{
				for (int col = 0; col < gridSize; ++col)
				{
					writer.write(chunk->heightMap[{ col, row }]);
				}
			}
			BinaryWriter landWriter{ chunkDirectory + U"/land_patches.bin" };
			if (!landWriter)
			{
				return SaveResult::failed(SaveError::WriteFailed,
					U"land_patches.bin を作成できません", chunkDirectory);
			}
			landWriter.write(static_cast<uint32>(chunk->landPatches.size()));
			for (const LandPatch& patch : chunk->landPatches)
			{
				landWriter.write(static_cast<int32>(patch.id));
				landWriter.write(static_cast<uint8>(patch.type));
				landWriter.write(patch.elevationOffset);
				landWriter.write(patch.materialVariant);
				landWriter.write(static_cast<int64>(patch.sourceParcelKey));
				landWriter.write(static_cast<uint32>(patch.polygon.size()));
				for (const Vec2& point : patch.polygon)
				{
					landWriter.write(static_cast<float>(point.x));
					landWriter.write(static_cast<float>(point.y));
				}
			}
		}
	}
	return SaveResult::succeeded(saveRoot);
}

SaveResult GameScene::verifyGameSnapshot(const FilePath& saveRoot) const
{
	constexpr int kSaveVersion = 3;
	const FilePath metaPath = saveRoot + U"/meta.json";
	const FilePath economyPath = saveRoot + U"/global/economy.json";
	const FilePath roadsPath = saveRoot + U"/global/roads.bin";
	const JSON meta = JSON::Load(metaPath);
	if (!meta)
	{
		return SaveResult::failed(SaveError::MissingData, U"meta.json がありません", metaPath);
	}
	const int version = meta[U"version"].getOr<int>(0);
	if (version <= 0 || version > kSaveVersion)
	{
		return SaveResult::failed(SaveError::UnsupportedVersion,
			U"未対応のセーブバージョンです: {}"_fmt(version), metaPath);
	}
	if (!JSON::Load(economyPath))
	{
		return SaveResult::failed(SaveError::CorruptData,
			U"economy.json が欠損または破損しています", economyPath);
	}
	if (!FileSystem::IsFile(roadsPath) || FileSystem::FileSize(roadsPath) <= 0)
	{
		return SaveResult::failed(SaveError::MissingData,
			U"roads.bin が欠損しています", roadsPath);
	}
	return SaveResult::succeeded(saveRoot);
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
			Chunk* chunk = m_world.getChunk(Point{ cx, cy });
			if (!chunk) continue;
			const String landPath = U"{}/chunks/{}_{}/land_patches.bin"_fmt(saveRoot, cx, cy);
			if (!FileSystem::Exists(landPath)) continue;
			BinaryReader landReader{ landPath };
			if (!landReader) continue;
			uint32 patchCount = 0;
			if (!landReader.read(patchCount) || patchCount > 4096) continue;
			chunk->landPatches.clear();
			for (uint32 p = 0; p < patchCount; ++p)
			{
				LandPatch patch;
				int32 id = -1;
				uint8 type = 0;
				int64 sourceParcelKey = -1;
				uint32 pointCount = 0;
				if (!landReader.read(id) || !landReader.read(type) || !landReader.read(patch.elevationOffset)
					|| !landReader.read(patch.materialVariant) || !landReader.read(sourceParcelKey)
					|| !landReader.read(pointCount) || pointCount > 64)
				{
					chunk->landPatches.clear();
					break;
				}
				patch.id = static_cast<int>(id);
				patch.type = static_cast<LandPatchType>(type);
				patch.sourceParcelKey = sourceParcelKey;
				for (uint32 pointIndex = 0; pointIndex < pointCount; ++pointIndex)
				{
					float px = 0.0f, py = 0.0f;
					if (!landReader.read(px) || !landReader.read(py))
					{
						patch.polygon.clear();
						break;
					}
					patch.polygon << Vec2{ px, py };
				}
				if (patch.polygon.size() >= 3)
				{
					chunk->landPatches << patch;
				}
			}
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
	generateLandPatches(true);
	migrateLegacyBuildingFrontageReferences();
	refreshBuildingAnglesFromEdges();
	m_cityConstraintValidationPassed = validateGeneratedCityConstraints();
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
// 初期土地利用の形状補助
// =============================================================================

namespace
{
	uint32 settlementCellHash(uint64 seed, int settlementIndex, int gx, int gz)
	{
		uint64 value = seed ^ (static_cast<uint64>(settlementIndex) * 0x9E3779B97F4A7C15ULL);
		value ^= static_cast<uint64>(gx) * 0xBF58476D1CE4E5B9ULL;
		value ^= static_cast<uint64>(gz) * 0x94D049BB133111EBULL;
		value ^= value >> 30;
		value *= 0xBF58476D1CE4E5B9ULL;
		value ^= value >> 27;
		value *= 0x94D049BB133111EBULL;
		value ^= value >> 31;
		return static_cast<uint32>(value);
	}

	float hash01(uint64 seed, int settlementIndex, int gx, int gz)
	{
		return static_cast<float>(settlementCellHash(seed, settlementIndex, gx, gz) & 0xFFFFu) / 65535.0f;
	}

	Vec2 landUseAxisX(const MapGenerator::Settlement& settlement, uint64 seed, int settlementIndex)
	{
		if (settlement.gridAxisX.lengthSq() > 1e-6f)
		{
			Vec2 axis = settlement.gridAxisX;
			axis.normalize();
			return axis;
		}
		const float angle = static_cast<float>((seed + settlementIndex * 97) % 6283) * 0.001f;
		return Vec2{ Math::Cos(angle), Math::Sin(angle) };
	}

	float localSlope(const Chunk& chunk, Point chunkCoord, float wx, float wz)
	{
		constexpr float sample = 16.0f;
		const float hx0 = sampleHeightMap(chunk.heightMap, chunkCoord, wx - sample, wz);
		const float hx1 = sampleHeightMap(chunk.heightMap, chunkCoord, wx + sample, wz);
		const float hz0 = sampleHeightMap(chunk.heightMap, chunkCoord, wx, wz - sample);
		const float hz1 = sampleHeightMap(chunk.heightMap, chunkCoord, wx, wz + sample);
		const float dx = (hx1 - hx0) / (sample * 2.0f);
		const float dz = (hz1 - hz0) / (sample * 2.0f);
		return Math::Sqrt(dx * dx + dz * dz);
	}

	ZoneType pickInitialZone(uint64 seed, int settlementIndex,
		const MapGenerator::Settlement& settlement, const Chunk& chunk, Point chunkCoord,
		int globalGX, int globalGZ, float wx, float wz)
	{
		const float h = sampleHeightMap(chunk.heightMap, chunkCoord, wx, wz);
		if (h < 0.0f) return ZoneType::Unzoned;
		const float slope = localSlope(chunk, chunkCoord, wx, wz);
		if (slope > 0.12f) return ZoneType::Unzoned;
		if (h < 7.0f) return ZoneType::Unzoned;

		const Vec2 axisX = landUseAxisX(settlement, seed, settlementIndex);
		const Vec2 axisZ{ -axisX.y, axisX.x };
		const Vec2 delta{ wx - static_cast<float>(settlement.center.x), wz - static_cast<float>(settlement.center.y) };
		const float lx = static_cast<float>(delta.dot(axisX));
		const float lz = static_cast<float>(delta.dot(axisZ));
		const float n = hash01(seed, settlementIndex, globalGX / 2, globalGZ / 2);
		const float fine = hash01(seed ^ 0xA53A9E11ULL, settlementIndex, globalGX, globalGZ);

		if (settlement.kind == MapGenerator::SettlementKind::CastleTown)
		{
			const float halfX = Max(1080.0f, settlement.radius * 1.32f) * (0.96f + n * 0.16f);
			const float halfZ = Max(820.0f, settlement.radius * 1.02f) * (0.92f + n * 0.18f);
			const float nx = Math::Abs(lx) / halfX;
			const float nz = Math::Abs(lz) / halfZ;
			const float roundedShape = Math::Pow(nx, 1.45f) + Math::Pow(nz, 1.45f);
			const float cornerBite = (nx > 0.62f && nz > 0.62f) ? 0.20f + fine * 0.14f : 0.0f;
			const float boundaryNoise = (n - 0.5f) * 0.30f + Math::Sin((lx + lz) * 0.003f) * 0.06f;
			const float corridor = Min(Math::Abs(lz) / 135.0f + Math::Abs(lx) / (halfX * 1.38f),
				Math::Abs(lx) / 125.0f + Math::Abs(lz) / (halfZ * 1.30f));
			const bool urbanCorridor = corridor < 1.0f;
			const bool detachedPocket = roundedShape < (1.18f + boundaryNoise - cornerBite);
			if (!detachedPocket && !urbanCorridor)
			{
				const float fieldShape = Math::Pow(nx, 1.20f) + Math::Pow(nz, 1.20f);
				return (fieldShape < 1.88f && slope < 0.070f && fine < 0.78f) ? ZoneType::Agriculture : ((fieldShape < 1.72f && fine < 0.30f) ? ZoneType::LowResidential : ZoneType::Unzoned);
			}
			if (roundedShape > 0.96f && fine < 0.16f) return ZoneType::Industrial;
			if (urbanCorridor || (Math::Abs(lx) < 330.0f && Math::Abs(lz) < 260.0f && fine < 0.72f)) return ZoneType::Commercial;
			if (roundedShape < 0.78f || fine < 0.66f) return ZoneType::Residential;
			return ZoneType::LowResidential;
		}

		if (settlement.kind == MapGenerator::SettlementKind::PostTown)
		{
			const float length = Max(520.0f, settlement.radius * 2.05f) * (0.92f + n * 0.20f);
			const float width = Max(210.0f, settlement.radius * 0.92f) * (0.90f + n * 0.20f);
			const float ribbon = Math::Abs(lx) / length + Math::Abs(lz) / width;
			if (ribbon > 1.70f) return (slope < 0.060f && fine < 0.70f) ? ZoneType::Agriculture : ZoneType::Unzoned;
			if (Math::Abs(lz) < 62.0f && Math::Abs(lx) < length * 0.82f) return ZoneType::Commercial;
			return (ribbon < 1.18f || fine < 0.58f) ? ZoneType::Residential : ZoneType::LowResidential;
		}

		const float hamlet = Math::Abs(lx) / Max(170.0f, settlement.radius * 1.25f)
			+ Math::Abs(lz) / Max(72.0f, settlement.radius * 0.58f);
		if (hamlet < 0.92f) return ZoneType::LowResidential;
		if (hamlet < 1.22f && fine < 0.52f) return ZoneType::Residential;
		const float fieldRadius = Max(460.0f, settlement.radius * 3.35f) * (0.92f + n * 0.20f);
		if (delta.length() < fieldRadius && slope < 0.060f) return ZoneType::Agriculture;
		return ZoneType::Unzoned;
	}

	int zonePriority(ZoneType zone)
	{
		switch (zone)
		{
		case ZoneType::Commercial: return 5;
		case ZoneType::Residential: return 4;
		case ZoneType::LowResidential: return 3;
		case ZoneType::Industrial: return 2;
		case ZoneType::Agriculture: return 1;
		case ZoneType::UrbanControl: return 0;
		default: return -1;
		}
	}
	int nearestSettlementIndex(const Array<MapGenerator::Settlement>& settlements, float wx, float wz)
	{
		int bestIndex = -1;
		float bestScore = 1e30f;
		for (int i = 0; i < static_cast<int>(settlements.size()); ++i)
		{
			const auto& s = settlements[i];
			const float dx = wx - static_cast<float>(s.center.x);
			const float dz = wz - static_cast<float>(s.center.y);
			const float radius = Max(1.0f, s.radius);
			const float score = (dx * dx + dz * dz) / (radius * radius);
			if (score < bestScore)
			{
				bestScore = score;
				bestIndex = i;
			}
		}
		return bestIndex;
	}

	Array<Vec2> makeCellPatchPolygon(Point chunk, int minX, int minY, int width, int height, uint64 salt)
	{
		constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
		const float x0 = static_cast<float>(chunk.x * ZONE_CELLS + minX) * cellSize;
		const float z0 = static_cast<float>(chunk.y * ZONE_CELLS + minY) * cellSize;
		const float x1 = static_cast<float>(chunk.x * ZONE_CELLS + minX + width) * cellSize;
		const float z1 = static_cast<float>(chunk.y * ZONE_CELLS + minY + height) * cellSize;
		const float w = x1 - x0;
		const float h = z1 - z0;
		const float cornerInset = Min(w, h) * 0.11f;
		const float edgeInset = Min(w, h) * 0.05f;
		auto noise = [&](int shift, float scale)
		{
			return (static_cast<float>((salt >> shift) & 255u) / 255.0f - 0.5f) * scale;
		};
		return Array<Vec2>{
			Vec2{ x0 + cornerInset + noise(0, edgeInset), z0 + noise(8, edgeInset) },
			Vec2{ x0 + w * (0.48f + noise(16, 0.05f)), z0 + edgeInset + noise(24, edgeInset) },
			Vec2{ x1 - cornerInset + noise(32, edgeInset), z0 + noise(40, edgeInset) },
			Vec2{ x1 - edgeInset + noise(6, edgeInset), z0 + h * (0.48f + noise(14, 0.05f)) },
			Vec2{ x1 - cornerInset + noise(22, edgeInset), z1 + noise(30, edgeInset) },
			Vec2{ x0 + w * (0.52f + noise(38, 0.05f)), z1 - edgeInset + noise(46, edgeInset) },
			Vec2{ x0 + cornerInset + noise(4, edgeInset), z1 + noise(12, edgeInset) },
			Vec2{ x0 + edgeInset + noise(20, edgeInset), z0 + h * (0.52f + noise(28, 0.05f)) }
		};
	}
	Array<Vec2> makeOrientedPatchPolygon(Vec2 center, float width, float depth, float angle, uint64 salt)
	{
		const float hx = width * 0.5f;
		const float hz = depth * 0.5f;
		const float cosA = Math::Cos(angle);
		const float sinA = Math::Sin(angle);
		auto noise = [&](int shift, float scale)
		{
			return (static_cast<float>((salt >> shift) & 255u) / 255.0f - 0.5f) * scale;
		};
		auto transform = [&](float x, float z)
		{
			return Vec2{ center.x + x * cosA - z * sinA, center.y + x * sinA + z * cosA };
		};
		return Array<Vec2>{
			transform(-hx * 0.86f + noise(0, hx * 0.10f), -hz * 0.98f + noise(8, hz * 0.08f)),
			transform(noise(16, hx * 0.12f), -hz * 0.90f + noise(24, hz * 0.06f)),
			transform(hx * 0.92f + noise(32, hx * 0.08f), -hz * 0.72f + noise(40, hz * 0.10f)),
			transform(hx * 0.98f + noise(6, hx * 0.06f), noise(14, hz * 0.12f)),
			transform(hx * 0.76f + noise(22, hx * 0.12f), hz * 0.94f + noise(30, hz * 0.08f)),
			transform(noise(38, hx * 0.14f), hz * 0.86f + noise(46, hz * 0.08f)),
			transform(-hx * 0.94f + noise(4, hx * 0.08f), hz * 0.68f + noise(12, hz * 0.12f)),
			transform(-hx * 0.98f + noise(20, hx * 0.06f), noise(28, hz * 0.10f))
		};
	}
	bool hasWaterNeighbor(const Chunk& chunk, int x, int y)
	{
		for (int dy = -1; dy <= 1; ++dy)
		{
			for (int dx = -1; dx <= 1; ++dx)
			{
				if (dx == 0 && dy == 0) continue;
				const int nx = x + dx;
				const int ny = y + dy;
				if (nx < 0 || ny < 0 || nx >= ZONE_CELLS || ny >= ZONE_CELLS) continue;
				if (chunk.heightMap[ny][nx] < -0.35f) return true;
			}
		}
		return false;
	}
	float infillDensity(MapGenerator::SettlementKind kind, ZoneType zone)
	{
		if (kind == MapGenerator::SettlementKind::CastleTown)
		{
			if (zone == ZoneType::Commercial) return 0.72f;
			if (zone == ZoneType::Residential) return 0.54f;
			if (zone == ZoneType::LowResidential) return 0.42f;
			if (zone == ZoneType::Industrial) return 0.34f;
		}
		if (kind == MapGenerator::SettlementKind::PostTown)
		{
			if (zone == ZoneType::Commercial) return 0.58f;
			if (zone == ZoneType::Residential) return 0.46f;
			if (zone == ZoneType::LowResidential) return 0.36f;
			if (zone == ZoneType::Industrial) return 0.30f;
		}
		if (zone == ZoneType::Residential) return 0.36f;
		if (zone == ZoneType::LowResidential) return 0.32f;
		return 0.0f;
	}
}
// =============================================================================
// ゾーン一括割り当て
// =============================================================================

void GameScene::applyZonesGlobal()
{
	const Stopwatch sw{ StartImmediately::Yes };
	constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;

	for (int si = 0; si < static_cast<int>(m_districts.size()); ++si)
	{
		const auto& settlement = m_districts[si];
		const float outerDist = (settlement.kind == MapGenerator::SettlementKind::CastleTown) ? Max(1650.0f, settlement.radius * 1.62f)
			: (settlement.kind == MapGenerator::SettlementKind::PostTown) ? Max(1050.0f, settlement.radius * 2.75f)
			: Max(680.0f, settlement.radius * 3.85f);

		const float scx = static_cast<float>(settlement.center.x);
		const float scz = static_cast<float>(settlement.center.y);
		const int chunkRadius = static_cast<int>(Ceil(outerDist / CHUNK_SIZE)) + 1;
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
						const int globalGX = cc.x * ZONE_CELLS + gx;
						const int globalGZ = cc.y * ZONE_CELLS + gz;
						const ZoneType candidate = pickInitialZone(getData().seed, si, settlement,
							*chunk, cc, globalGX, globalGZ, wx, wz);
						if (candidate == ZoneType::Unzoned) continue;

						ZoneType& current = chunk->zoneMap[{ gx, gz }];
						if (zonePriority(candidate) >= zonePriority(current))
						{
							current = candidate;
						}
					}
				}
			}
		}
	}

	Logger << U"[applyZonesGlobal] realistic land-use {:.0f}ms"_fmt(sw.msF());
}
// =============================================================================
// 初期建物配置
// =============================================================================

namespace
{
	namespace InitialBuilding
	{
		float density(MapGenerator::SettlementKind kind, ZoneType zone)
		{
			if (kind == MapGenerator::SettlementKind::CastleTown)
			{
				if (zone == ZoneType::Commercial) return 0.82f;
				if (zone == ZoneType::Residential) return 0.62f;
				if (zone == ZoneType::LowResidential) return 0.46f;
				if (zone == ZoneType::Industrial) return 0.50f;
				if (zone == ZoneType::Agriculture) return 0.20f;
			}
			else if (kind == MapGenerator::SettlementKind::PostTown)
			{
				if (zone == ZoneType::Commercial) return 0.70f;
				if (zone == ZoneType::Residential) return 0.56f;
				if (zone == ZoneType::LowResidential) return 0.42f;
				if (zone == ZoneType::Industrial) return 0.50f;
				if (zone == ZoneType::Agriculture) return 0.32f;
			}
			else
			{
				if (zone == ZoneType::Residential) return 0.38f;
				if (zone == ZoneType::LowResidential) return 0.34f;
				if (zone == ZoneType::Agriculture) return 0.62f;
			}
			return 0.0f;
		}

		Building spawn(ZoneType zone, MapGenerator::SettlementKind kind, GameTime gameNow, uint32 hash)
		{
			Building building;
			building.builtAt = gameNow;
			const uint32 roll = hash % 100u;
			switch (zone)
			{
			case ZoneType::LowResidential:
				if (roll < 5u) building.type = BuildingType::ParkBuilding;
				else if (roll < 9u) building.type = BuildingType::Parking;
				else building.type = (roll < (kind == MapGenerator::SettlementKind::Village ? 94u : 82u))
					? BuildingType::Detached : BuildingType::LowApartment;
				break;
			case ZoneType::Residential:
				if (roll < 5u) building.type = BuildingType::ParkBuilding;
				else if (roll < 10u) building.type = BuildingType::PublicFacility;
				else if (roll < 11u) building.type = BuildingType::MidApartment;
				else building.type = (roll < 84u) ? BuildingType::Detached : BuildingType::LowApartment;
				break;
			case ZoneType::Commercial:
				if (roll < 8u) building.type = BuildingType::Parking;
				else if (roll < 13u) building.type = BuildingType::PublicFacility;
				else building.type = (roll < 86u) ? BuildingType::Shop : BuildingType::Office;
				break;
			case ZoneType::Industrial:
				if (roll < 12u) building.type = BuildingType::Parking;
				else if (roll < 18u) building.type = BuildingType::PublicFacility;
				else building.type = BuildingType::Factory;
				break;
			case ZoneType::Agriculture:
				building.type = BuildingType::Farmland;
				break;
			default:
				building.type = BuildingType::None;
				break;
			}
			return building;
		}
		void applySettlementContext(Building& building, ZoneType zone, MapGenerator::SettlementKind kind,
		                            float distFromCenter, float settlementRadius, float roadDist, uint32 hash)
		{
			const float coreRadius = settlementRadius * (kind == MapGenerator::SettlementKind::CastleTown ? 0.26f : 0.18f);
			const bool urbanCore = (kind == MapGenerator::SettlementKind::CastleTown && distFromCenter < coreRadius);
			const bool mainStreetCore = (kind == MapGenerator::SettlementKind::PostTown && distFromCenter < coreRadius && roadDist < 46.0f);
			const bool roadside = (roadDist < 30.0f);
			const bool districtEdge = (distFromCenter > settlementRadius * 0.92f);
			const uint32 roll = (hash / 17u) % 100u;

			if ((zone == ZoneType::Residential || zone == ZoneType::LowResidential) && roadside && !districtEdge)
			{
				const uint32 roadsideRoll = (hash / 11u) % 100u;
				if (roadsideRoll < (urbanCore ? 18u : 8u))
				{
					building.type = BuildingType::Shop;
				}
				else if (roadsideRoll < (urbanCore ? 24u : 11u))
				{
					building.type = BuildingType::Parking;
				}
			}

			if (building.type == BuildingType::MidApartment && !(urbanCore || mainStreetCore))
			{
				building.type = (roll < 78u) ? BuildingType::Detached : BuildingType::LowApartment;
			}

			if (building.type == BuildingType::HighApartment && !urbanCore)
			{
				building.type = BuildingType::MidApartment;
			}

			if (building.type == BuildingType::Office)
			{
				const uint32 officeRoll = (hash / 29u) % 100u;
				const bool officeAllowed = urbanCore && zone == ZoneType::Commercial && roadDist < 42.0f;
				
				if (!officeAllowed)
				{
					building.type = (officeRoll < 80u) ? BuildingType::Shop : BuildingType::Parking;
				}
			}

			if (building.type == BuildingType::PublicFacility)
			{
				if (kind == MapGenerator::SettlementKind::Village && roll < 55u)
				{
					building.type = BuildingType::Detached;
				}
				else if (!roadside && roll < 35u)
				{
					building.type = BuildingType::ParkBuilding;
				}
			}

			if (districtEdge && building.type == BuildingType::LowApartment && roll < 72u)
			{
				building.type = BuildingType::Detached;
			}
		}
		BuildingType ruralFringeBuildingType(MapGenerator::SettlementKind kind, float distFromCenter, float settlementRadius, uint32 hash)
		{
			const float fringeStart = settlementRadius * (kind == MapGenerator::SettlementKind::Village ? 0.92f : 1.08f);
			const float fringeEnd = settlementRadius * (kind == MapGenerator::SettlementKind::Village ? 2.80f : 2.15f);
			if (distFromCenter < fringeStart || distFromCenter > fringeEnd)
			{
				return BuildingType::Farmland;
			}

			const uint32 roll = (hash / 37u) % 100u;
			if (roll < 3u) return BuildingType::Detached;
			if (roll < 4u) return BuildingType::Factory;
			if (roll < 5u) return BuildingType::Parking;
			if (roll < 6u) return BuildingType::ParkBuilding;
			return BuildingType::Farmland;
		}
	}
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

	
	Vec2 interiorBuildingOffset(uint32 hash, float maxOffset)
	{
		const float ox = (static_cast<float>(hash & 0xFFu) / 255.0f - 0.5f) * maxOffset * 2.0f;
		const float oz = (static_cast<float>((hash >> 8) & 0xFFu) / 255.0f - 0.5f) * maxOffset * 2.0f;
		return Vec2{ ox, oz };
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
		Vec2  roadToCellDir{ 1.0f, 0.0f };
		float centerDistSq = 0.0f;
	};
	Vec2 roadsideBuildingOffset(const EdgeFacingSlot& slot, BuildingType type, int gx, int gz)
	{
		if (type == BuildingType::Farmland || slot.roadDist <= 0.001f)
		{
			return Vec2{ 0.0f, 0.0f };
		}

		const float buildingHalf = buildingFootprintXZ() * 0.5f;
		const float targetDist = slot.halfWidth + buildingHalf + setbackFromRoadByModel(type, gx, gz) + 1.5f;
		const float moveTowardRoad = Clamp(slot.roadDist - targetDist, 0.0f, 5.5f);
		return -slot.roadToCellDir * moveTowardRoad;
	}



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
			if (edge.roadType != RoadType::LocalRoad && edge.roadType != RoadType::Arterial) continue;

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
					const Vec2 roadToCellDir = toCell.normalized();

					const float centerDx = static_cast<float>(cellCenter.x) - static_cast<float>(center.x);
					const float centerDz = static_cast<float>(cellCenter.y) - static_cast<float>(center.y);

					EdgeFacingSlot slot;
					slot.chunkCoord = cc;
					slot.col = gx;
					slot.row = gz;
					slot.edgeId = edge.id;
					slot.edgeT = projection.edgeT;
					slot.angle = static_cast<float>(std::atan2(-roadToCellDir.x, roadToCellDir.y));
					slot.halfWidth = edgeHalfWidth;
					slot.roadDist = projection.distance;
					slot.roadToCellDir = roadToCellDir;
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
	HashTable<int64, EdgeFacingSlot> edgeFacingSlotsByCell;

	// 道路沿いスロットを地区ごとに収集し、ゾーン・道路距離・重なり判定を満たす場所へ初期建物を置く。
	for (int si = 0; si < static_cast<int>(m_districts.size()); ++si)
	{
		const auto& s = m_districts[si];


		const float radiusSq = s.radius * s.radius * 4.0f;
		const Array<EdgeFacingSlot> slots = collectEdgeFacingSlots(s, m_world, m_network);
		for (const auto& slot : slots)
		{
			Chunk* chunk = m_world.getChunk(slot.chunkCoord);
			if (!chunk) continue;
			if (chunk->buildingGrid[{ slot.col, slot.row }].type != BuildingType::None) continue;

			const ZoneType zone = chunk->zoneMap[{ slot.col, slot.row }];
			if (zone == ZoneType::Unzoned) continue;

			const int64 slotKey = zoneCellKey(slot.chunkCoord, slot.col, slot.row);
			const auto slotIt = edgeFacingSlotsByCell.find(slotKey);
			if (slotIt == edgeFacingSlotsByCell.end() || slot.roadDist < slotIt->second.roadDist)
			{
				edgeFacingSlotsByCell[slotKey] = slot;
			}

			const Vec2 centerPos = cellCenterXZ(slot.chunkCoord, slot.col, slot.row);
			const float dx = static_cast<float>(centerPos.x - s.center.x);
			const float dz = static_cast<float>(centerPos.y - s.center.y);
			const float distFromCenterSq = dx * dx + dz * dz;
			if (distFromCenterSq > radiusSq) continue;

			const float h = sampleHeightMap(
				chunk->heightMap, slot.chunkCoord,
				static_cast<float>(centerPos.x), static_cast<float>(centerPos.y));
			if (h < 2.6f) continue;

			const int globalGX = slot.chunkCoord.x * ZONE_CELLS + slot.col;
			const int globalGZ = slot.chunkCoord.y * ZONE_CELLS + slot.row;
			const uint32 cellHash = settlementCellHash(getData().seed, si, globalGX, globalGZ);
			Building b = InitialBuilding::spawn(zone, s.kind, 0.0, cellHash);
			InitialBuilding::applySettlementContext(b, zone, s.kind, Math::Sqrt(distFromCenterSq), s.radius, slot.roadDist, cellHash);
			if (b.type == BuildingType::None) continue;

			if (b.type != BuildingType::Farmland
				&& overlapsExistingBuilding(
					m_world, slot.chunkCoord, slot.col, slot.row,
					static_cast<float>(centerPos.x), static_cast<float>(centerPos.y), halfBuilding)) continue;

			float roadScore;
			if      (slot.roadDist < kNearDist) roadScore = 1.0f;
			else if (slot.roadDist < kFarDist)  roadScore = 1.0f - (slot.roadDist - kNearDist) / (kFarDist - kNearDist);
			else                                roadScore = 0.0f;

			const float distFromCenter = Math::Sqrt(distFromCenterSq);
			const float centerFalloff = Clamp(1.0f - distFromCenter / Max(1.0f, s.radius * 2.2f), 0.20f, 1.0f);
			const float densityFactor = InitialBuilding::density(s.kind, zone) * (0.78f + centerFalloff * 0.22f);
			const float score = roadScore * densityFactor;
			if (score < 0.03f) continue;

			const float roll = (cellHash % 1000) / 1000.0f;
			if (roll > score) continue;

			const float setbackM = setbackFromRoadByModel(b.type, globalGX, globalGZ);
			if (b.type != BuildingType::Farmland
				&& slot.roadDist < (slot.halfWidth + halfBuilding + setbackM)) continue;
			const Vec2 roadOffset = roadsideBuildingOffset(slot, b.type, globalGX, globalGZ);
			b.offsetX = static_cast<float>(roadOffset.x);
			b.offsetZ = static_cast<float>(roadOffset.y);
			b.angle = slot.angle;
			b.edgeId = slot.edgeId;
			b.edgeT = slot.edgeT;


			chunk->buildingGrid[{ slot.col, slot.row }] = b;
			chunk->meshDirty = true;
			++placed;
		}
	}

	int infillPlaced = 0;
	for (int chunkY = 0; chunkY < WORLD_CHUNKS; ++chunkY)
	{
		for (int chunkX = 0; chunkX < WORLD_CHUNKS; ++chunkX)
		{
			Chunk* chunk = m_world.getChunk(Point{ chunkX, chunkY });
			if (!chunk) continue;
			bool changed = false;
			for (int row = 0; row < ZONE_CELLS; ++row)
			{
				for (int col = 0; col < ZONE_CELLS; ++col)
				{
					Building& building = chunk->buildingGrid[{ col, row }];
					if (building.type != BuildingType::None) continue;

					const ZoneType zone = chunk->zoneMap[{ col, row }];
					if (zone != ZoneType::Commercial && zone != ZoneType::Residential
						&& zone != ZoneType::LowResidential && zone != ZoneType::Industrial) continue;

					const Vec2 centerPos = cellCenterXZ(Point{ chunkX, chunkY }, col, row);
					const int si = nearestSettlementIndex(m_districts,
						static_cast<float>(centerPos.x), static_cast<float>(centerPos.y));
					if (si < 0) continue;

					const auto& settlement = m_districts[si];
					const int globalGX = chunkX * ZONE_CELLS + col;
					const int globalGZ = chunkY * ZONE_CELLS + row;
					const uint32 cellHash = settlementCellHash(getData().seed ^ 0xE35A11B5ULL, si, globalGX, globalGZ);
					const float density = infillDensity(settlement.kind, zone);
					if ((cellHash % 1000u) >= static_cast<uint32>(density * 1000.0f)) continue;

					const float centerHeight = sampleHeightMap(chunk->heightMap, Point{ chunkX, chunkY }, static_cast<float>(centerPos.x), static_cast<float>(centerPos.y));
					if (centerHeight < 2.6f) continue;
					Building b = InitialBuilding::spawn(zone, settlement.kind, 0.0, cellHash);
					const float distFromCenter = static_cast<float>((centerPos - settlement.center).length());
					InitialBuilding::applySettlementContext(b, zone, settlement.kind, distFromCenter, settlement.radius, 48.0f, cellHash);
					if (b.type == BuildingType::None || b.type == BuildingType::Farmland) continue;

					const int64 roadSlotKey = zoneCellKey(Point{ chunkX, chunkY }, col, row);
					if (const auto roadSlot = edgeFacingSlotsByCell.find(roadSlotKey); roadSlot != edgeFacingSlotsByCell.end())
					{
						const EdgeFacingSlot& slot = roadSlot->second;
						const float setbackM = setbackFromRoadByModel(b.type, globalGX, globalGZ);
						if (slot.roadDist < (slot.halfWidth + halfBuilding + setbackM)) continue;
						const Vec2 roadOffset = roadsideBuildingOffset(slot, b.type, globalGX, globalGZ);
						b.offsetX = static_cast<float>(roadOffset.x);
						b.offsetZ = static_cast<float>(roadOffset.y);
						b.angle = slot.angle;
						b.edgeId = slot.edgeId;
						b.edgeT = slot.edgeT;
					}
					else
					{
						continue;
					}					building = b;
					++infillPlaced;
					changed = true;
				}
			}
			if (changed) chunk->meshDirty = true;
		}
	}

	int fieldCells = 0;
	for (int chunkY = 0; chunkY < WORLD_CHUNKS; ++chunkY)
	{
		for (int chunkX = 0; chunkX < WORLD_CHUNKS; ++chunkX)
		{
			Chunk* chunk = m_world.getChunk(Point{ chunkX, chunkY });
			if (!chunk) continue;
			bool changed = false;
			for (int row = 0; row < ZONE_CELLS; ++row)
			{
				for (int col = 0; col < ZONE_CELLS; ++col)
				{
					if (chunk->zoneMap[{ col, row }] != ZoneType::Agriculture) continue;
					Building& building = chunk->buildingGrid[{ col, row }];
					if (building.type != BuildingType::None) continue;
					const int globalGX = chunkX * ZONE_CELLS + col;
					const int globalGZ = chunkY * ZONE_CELLS + row;
					const uint32 hash = settlementCellHash(getData().seed, 0, globalGX / 2, globalGZ / 2);
					if ((hash % 100u) >= 92u) continue;
					++fieldCells;
				}
			}
			if (changed) chunk->meshDirty = true;
		}
	}
	generateLandPatches();
	Logger << U"[placeInitialBuildings] {} 棟配置, インフィル{}棟, 農地{}セル ({:.0f}ms)"_fmt(placed, infillPlaced, fieldCells, sw.msF());
	refreshBuildingAnglesFromEdges();
	m_cityConstraintValidationPassed = validateGeneratedCityConstraints();
}

void GameScene::generateLandPatches(bool preserveExisting)
{
	HashTable<int64, EdgeFacingSlot> edgeFacingSlotsByCell;
	for (const auto& settlement : m_districts)
	{
		const Array<EdgeFacingSlot> slots = collectEdgeFacingSlots(settlement, m_world, m_network);
		for (const EdgeFacingSlot& slot : slots)
		{
			const int64 key = zoneCellKey(slot.chunkCoord, slot.col, slot.row);
			const auto it = edgeFacingSlotsByCell.find(key);
			if (it == edgeFacingSlotsByCell.end() || slot.roadDist < it->second.roadDist)
			{
				edgeFacingSlotsByCell[key] = slot;
			}
		}
	}

	auto isUrbanLandZone = [](ZoneType zone)
	{
		return zone == ZoneType::LowResidential || zone == ZoneType::Residential
			|| zone == ZoneType::Commercial || zone == ZoneType::Industrial;
	};

	auto parcelTypeFor = [](ZoneType zone, BuildingType buildingType, uint32 salt)
	{
		if (buildingType == BuildingType::Parking || zone == ZoneType::Commercial || zone == ZoneType::Industrial)
		{
			return ((salt >> 5) & 1u) ? LandPatchType::ParcelAsphalt : LandPatchType::ParcelGravel;
		}
		return ((salt >> 4) & 1u) ? LandPatchType::GardenSoil : LandPatchType::ParcelGravel;
	};

	for (int chunkY = 0; chunkY < WORLD_CHUNKS; ++chunkY)
	{
		for (int chunkX = 0; chunkX < WORLD_CHUNKS; ++chunkX)
		{
			Chunk* chunkPtr = m_world.getChunk(Point{ chunkX, chunkY });
			if (!chunkPtr) continue;
			Chunk& chunk = *chunkPtr;
			if (preserveExisting && !chunk.landPatches.isEmpty()) continue;
			chunk.landPatches.clear();
			const Point coord{ chunkX, chunkY };
			uint32 patchIndex = 0;
			for (int row = 0; row < ZONE_CELLS; ++row)
			{
				for (int col = 0; col < ZONE_CELLS; ++col)
				{
					const ZoneType zone = chunk.zoneMap[{ col, row }];
					if (!isUrbanLandZone(zone)) continue;

					const Building& building = chunk.buildingGrid[{ col, row }];
					if (building.type == BuildingType::Farmland) continue;

					const int64 cellKey = zoneCellKey(coord, col, row);
					const int globalGX = chunkX * ZONE_CELLS + col;
					const int globalGZ = chunkY * ZONE_CELLS + row;
					const uint32 salt = settlementCellHash(getData().seed ^ 0xA24BAED5u, 211, globalGX, globalGZ);

					int edgeId = -1;
					Vec2 sampleCenter = cellCenterXZ(coord, col, row);
					if (building.type != BuildingType::None && building.edgeId >= 0)
					{
						edgeId = building.edgeId;
						sampleCenter += Vec2{ building.offsetX, building.offsetZ };
					}
					else if (const auto slotIt = edgeFacingSlotsByCell.find(cellKey); slotIt != edgeFacingSlotsByCell.end())
					{
						edgeId = slotIt->second.edgeId;
					}
					else
					{
						continue;
					}

					EdgeProjection projection;
					if (!projectPointToEdgeXZ(m_network, edgeId, sampleCenter, projection)) continue;
					const RoadEdge* edge = m_network.getEdge(edgeId);
					if (!edge || !edge->isRoadbedBuilt()) continue;

					const RoadGeometry::LateralRange range = RoadGeometry::structuralRangeAt(*edge, projection.edgeT);
					if (!range.valid) continue;

					const Vec2 toCell = sampleCenter - projection.position;
					const float signedLateral = static_cast<float>(toCell.dot(projection.right));
					const bool rightSide = (signedLateral >= 0.0f);
					const Vec2 frontageDir = rightSide ? projection.right : -projection.right;
					const float structuralOuter = rightSide ? Max(0.0f, range.right) : Max(0.0f, -range.left);
					const float cellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
					const float buildingHalf = (building.type == BuildingType::None) ? cellSize * 0.30f : buildingFootprintXZ() * 0.5f;
					const float frontageDepth = (building.type == BuildingType::None)
						? Clamp(projection.distance - structuralOuter + cellSize * 0.60f, cellSize * 0.64f, cellSize * 1.25f)
						: Max(cellSize * 0.70f, projection.distance + buildingHalf + 3.0f - structuralOuter);
					const float frontOffset = structuralOuter + 0.03f;
					const float backOffset = structuralOuter + frontageDepth;
					const float halfAlong = Min(cellSize * 0.62f,
						buildingHalf + 2.8f + static_cast<float>(((globalGX * 13 + globalGZ * 7) & 3)) * 0.45f);

					LandPatch patch;
					patch.id = static_cast<int>(patchIndex++);
					patch.sourceParcelKey = cellKey;
					patch.type = parcelTypeFor(zone, building.type, salt);
					patch.elevationOffset = 0.055f;
					patch.materialVariant = salt;
					const Vec2 along = projection.tangent;
					const float frontShift = (static_cast<float>((salt >> 11) & 15u) / 15.0f - 0.5f) * halfAlong * 0.16f;
					const float backShift = (static_cast<float>((salt >> 17) & 15u) / 15.0f - 0.5f) * halfAlong * 0.20f;
					const float sideBias = (static_cast<float>((salt >> 23) & 15u) / 15.0f - 0.5f) * 0.36f;
					patch.polygon = Array<Vec2>{
						projection.position - along * halfAlong + along * frontShift + frontageDir * frontOffset,
						projection.position + along * (halfAlong * 0.18f + frontShift * 0.35f) + frontageDir * (frontOffset + (backOffset - frontOffset) * 0.08f),
						projection.position + along * halfAlong * (0.88f + sideBias * 0.10f) + frontageDir * (frontOffset + (backOffset - frontOffset) * 0.18f),
						projection.position + along * halfAlong * (0.96f - sideBias * 0.12f) + frontageDir * backOffset,
						projection.position + along * backShift + frontageDir * (backOffset + (backOffset - frontOffset) * 0.04f),
						projection.position - along * halfAlong * (0.92f + sideBias * 0.10f) + frontageDir * backOffset
					};
					chunk.landPatches << patch;
				}
			}

			for (int y = 0; y < ZONE_CELLS; ++y)
			{
				int x = 0;
				while (x < ZONE_CELLS)
				{
					float averageHeight = 0.0f;
					int runLength = 0;
					while (x + runLength < ZONE_CELLS && hasWaterNeighbor(chunk, x + runLength, y))
					{
						averageHeight += chunk.heightMap[y][x + runLength];
						++runLength;
					}
					if (runLength <= 0)
					{
						++x;
						continue;
					}
					averageHeight /= static_cast<float>(runLength);
					const int stripWidth = Min(4, Max(1, runLength));
					const uint32 salt = settlementCellHash(getData().seed, 97, coord.x * ZONE_CELLS + x, coord.y * ZONE_CELLS + y);
					if (averageHeight >= -0.18f && averageHeight < 3.20f)
					{
						LandPatch patch;
						patch.id = static_cast<int>(patchIndex++);
						patch.type = LandPatchType::Beach;
						patch.polygon = makeCellPatchPolygon(coord, x, y, stripWidth, 1, salt);
						patch.elevationOffset = Max(0.015f, 0.055f - averageHeight * 0.35f);
						patch.materialVariant = salt;
						chunk.landPatches << patch;
					}
					else if (averageHeight >= 3.20f && averageHeight < 7.4f)
					{
						LandPatch patch;
						patch.id = static_cast<int>(patchIndex++);
						patch.type = LandPatchType::Seawall;
						patch.polygon = makeCellPatchPolygon(coord, x, y, stripWidth, 1, salt ^ 0x9E3779B9u);
						patch.elevationOffset = 0.08f;
						patch.materialVariant = salt;
						chunk.landPatches << patch;
					}
					x += stripWidth;
				}
			}

			for (int y = 0; y < ZONE_CELLS - 3; y += 4)
			{
				for (int x = 0; x < ZONE_CELLS - 3; x += 4)
				{
					int agricultureCount = 0;
					int urbanCount = 0;
					float averageHeight = 0.0f;
					for (int dy = 0; dy < 4; ++dy)
					{
						for (int dx = 0; dx < 4; ++dx)
						{
							const ZoneType cellZone = chunk.zoneMap[{ x + dx, y + dy }];
							if (cellZone == ZoneType::Agriculture) ++agricultureCount;
							else if (isUrbanLandZone(cellZone)) ++urbanCount;
							averageHeight += chunk.heightMap[y + dy][x + dx];
						}
					}
					if (agricultureCount < 8 || urbanCount > 2) continue;
					averageHeight *= 1.0f / 16.0f;
					if (averageHeight < 7.0f) continue;

					const uint32 salt = settlementCellHash(getData().seed ^ 0xD1B54A32u, 131, coord.x * ZONE_CELLS + x, coord.y * ZONE_CELLS + y);
					LandPatch patch;
					patch.id = static_cast<int>(patchIndex++);
					patch.type = ((salt >> 3) & 1u) ? LandPatchType::PaddyField : LandPatchType::FarmField;
					const Vec2 farmCenter = cellCenterXZ(coord, x + 2, y + 2);
					const float farmAngle = static_cast<float>(((salt >> 9) % 360u) * Math::Pi / 180.0);
					const float farmWidth = (static_cast<float>(CHUNK_SIZE) / ZONE_CELLS) * (2.8f + static_cast<float>((salt >> 17) % 9u) * 0.12f);
					const float farmDepth = (static_cast<float>(CHUNK_SIZE) / ZONE_CELLS) * (2.0f + static_cast<float>((salt >> 25) % 11u) * 0.12f);
					patch.polygon = makeOrientedPatchPolygon(farmCenter, farmWidth, farmDepth, farmAngle, salt);
					patch.elevationOffset = 0.035f;
					patch.materialVariant = salt;
					chunk.landPatches << patch;
				}
			}
		}
	}
}
void GameScene::migrateLegacyBuildingFrontageReferences()
{
	HashTable<int64, EdgeFacingSlot> edgeFacingSlotsByCell;
	for (const auto& settlement : m_districts)
	{
		const Array<EdgeFacingSlot> slots = collectEdgeFacingSlots(settlement, m_world, m_network);
		for (const EdgeFacingSlot& slot : slots)
		{
			const int64 key = zoneCellKey(slot.chunkCoord, slot.col, slot.row);
			const auto it = edgeFacingSlotsByCell.find(key);
			if (it == edgeFacingSlotsByCell.end() || slot.roadDist < it->second.roadDist)
			{
				edgeFacingSlotsByCell[key] = slot;
			}
		}
	}

	int assigned = 0;
	int unresolved = 0;
	for (int chunkY = 0; chunkY < WORLD_CHUNKS; ++chunkY)
	{
		for (int chunkX = 0; chunkX < WORLD_CHUNKS; ++chunkX)
		{
			Chunk* chunk = m_world.getChunk(Point{ chunkX, chunkY });
			if (!chunk) continue;
			bool changed = false;
			for (int row = 0; row < ZONE_CELLS; ++row)
			{
				for (int col = 0; col < ZONE_CELLS; ++col)
				{
					Building& building = chunk->buildingGrid[{ col, row }];
					if (building.type == BuildingType::None || building.type == BuildingType::Farmland) continue;
					if (building.edgeId >= 0) continue;

					const int64 key = zoneCellKey(Point{ chunkX, chunkY }, col, row);
					const auto slotIt = edgeFacingSlotsByCell.find(key);
					if (slotIt == edgeFacingSlotsByCell.end())
					{
						++unresolved;
						continue;
					}

					const EdgeFacingSlot& slot = slotIt->second;
					const int globalGX = chunkX * ZONE_CELLS + col;
					const int globalGZ = chunkY * ZONE_CELLS + row;
					const Vec2 roadOffset = roadsideBuildingOffset(slot, building.type, globalGX, globalGZ);
					building.offsetX = static_cast<float>(roadOffset.x);
					building.offsetZ = static_cast<float>(roadOffset.y);
					building.angle = slot.angle;
					building.edgeId = slot.edgeId;
					building.edgeT = slot.edgeT;
					changed = true;
					++assigned;
				}
			}
			if (changed) chunk->meshDirty = true;
		}
	}

	if (assigned > 0 || unresolved > 0)
	{
		Logger << U"[migrateLegacyBuildingFrontageReferences] assigned={} unresolved={}"_fmt(assigned, unresolved);
	}
}
bool GameScene::validateGeneratedCityConstraints()
{
	HashTable<int64, EdgeFacingSlot> edgeFacingSlotsByCell;
	for (const auto& settlement : m_districts)
	{
		const Array<EdgeFacingSlot> slots = collectEdgeFacingSlots(settlement, m_world, m_network);
		for (const EdgeFacingSlot& slot : slots)
		{
			const int64 key = zoneCellKey(slot.chunkCoord, slot.col, slot.row);
			const auto it = edgeFacingSlotsByCell.find(key);
			if (it == edgeFacingSlotsByCell.end() || slot.roadDist < it->second.roadDist)
			{
				edgeFacingSlotsByCell[key] = slot;
			}
		}
	}

	HashSet<int64> generatedParcelKeys;
	for (int chunkY = 0; chunkY < WORLD_CHUNKS; ++chunkY)
	{
		for (int chunkX = 0; chunkX < WORLD_CHUNKS; ++chunkX)
		{
			const Chunk* chunk = m_world.getChunk(Point{ chunkX, chunkY });
			if (!chunk) continue;
			for (const LandPatch& patch : chunk->landPatches)
			{
				if ((patch.type == LandPatchType::ParcelAsphalt || patch.type == LandPatchType::ParcelGravel || patch.type == LandPatchType::GardenSoil)
					&& patch.sourceParcelKey >= 0)
				{
					generatedParcelKeys.insert(patch.sourceParcelKey);
				}
			}
		}
	}

	int buildingCount = 0;
	int noFrontageCount = 0;
	int missingEdgeCount = 0;
	int roadOverlapCount = 0;
	int frontageDistanceCount = 0;
	int coastalBuildingCount = 0;
	int missingParcelCount = 0;
	for (int chunkY = 0; chunkY < WORLD_CHUNKS; ++chunkY)
	{
		for (int chunkX = 0; chunkX < WORLD_CHUNKS; ++chunkX)
		{
			const Point chunkCoord{ chunkX, chunkY };
			const Chunk* chunk = m_world.getChunk(chunkCoord);
			if (!chunk) continue;
			for (int row = 0; row < ZONE_CELLS; ++row)
			{
				for (int col = 0; col < ZONE_CELLS; ++col)
				{
					const Building& building = chunk->buildingGrid[{ col, row }];
					if (building.type == BuildingType::None || building.type == BuildingType::Farmland) continue;
					++buildingCount;

					const int64 cellKey = zoneCellKey(chunkCoord, col, row);
					if (!generatedParcelKeys.contains(cellKey))
					{
						++missingParcelCount;
					}
					if (!edgeFacingSlotsByCell.contains(cellKey) || building.edgeId < 0)
					{
						++noFrontageCount;
					}

					const Vec2 center = cellCenterXZ(chunkCoord, col, row) + Vec2{ building.offsetX, building.offsetZ };
					const float h = sampleHeightMap(chunk->heightMap, chunkCoord, static_cast<float>(center.x), static_cast<float>(center.y));
					if (h < 1.55f)
					{
						++coastalBuildingCount;
					}

					const RoadEdge* edge = m_network.getEdge(building.edgeId);
					if (!edge || !edge->isRoadbedBuilt())
					{
						++missingEdgeCount;
						continue;
					}

					EdgeProjection projection;
					if (!projectPointToEdgeXZ(m_network, building.edgeId, center, projection))
					{
						++missingEdgeCount;
						continue;
					}

					const int globalGX = chunkX * ZONE_CELLS + col;
					const int globalGZ = chunkY * ZONE_CELLS + row;
					const float edgeHalfWidth = edge->totalWidth() * 0.5f;
					const float buildingHalf = buildingFootprintXZ() * 0.5f;
					const float minimumClearance = edgeHalfWidth + buildingHalf * 0.45f;
					const float maximumFrontageDistance = edgeHalfWidth + buildingHalf + setbackFromRoadByModel(building.type, globalGX, globalGZ) + 8.0f;
					if (projection.distance < minimumClearance)
					{
						++roadOverlapCount;
					}
					const float cosA = Math::Cos(building.angle);
					const float sinA = Math::Sin(building.angle);
					int footprintInsideRoadCorners = 0;
					for (const Vec2 localCorner : { Vec2{ -buildingHalf, -buildingHalf }, Vec2{ buildingHalf, -buildingHalf }, Vec2{ buildingHalf, buildingHalf }, Vec2{ -buildingHalf, buildingHalf } })
					{
						const Vec2 corner{ center.x + localCorner.x * cosA - localCorner.y * sinA, center.y + localCorner.x * sinA + localCorner.y * cosA };
						EdgeProjection cornerProjection;
						if (projectPointToEdgeXZ(m_network, building.edgeId, corner, cornerProjection)
							&& cornerProjection.distance < edgeHalfWidth + 0.20f)
						{
							++footprintInsideRoadCorners;
						}
					}
					if (footprintInsideRoadCorners >= 2)
					{
						++roadOverlapCount;
					}
					if (projection.distance > maximumFrontageDistance)
					{
						++frontageDistanceCount;
					}
				}
			}
		}
	}
	const bool passed = (noFrontageCount == 0 && missingEdgeCount == 0 && missingParcelCount == 0 && roadOverlapCount == 0
		&& frontageDistanceCount == 0 && coastalBuildingCount == 0);
	m_cityConstraintValidationSummary = U"buildings={} noFrontage={} missingEdge={} missingParcel={} roadOverlap={} frontageDistance={} coastal={} passed={}"_fmt(
		buildingCount, noFrontageCount, missingEdgeCount, missingParcelCount, roadOverlapCount, frontageDistanceCount, coastalBuildingCount, passed);
	Logger << U"[CityConstraintValidation] " + m_cityConstraintValidationSummary;
	return passed;
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
// 提出用スクリーンショット
// =============================================================================

Vec3 GameScene::captureFocusPoint() const
{
	for (const auto& s : m_districts)
	{
		if (s.kind == MapGenerator::SettlementKind::CastleTown)
		{
			const float y = m_world.computeHeight(
				static_cast<float>(s.center.x), static_cast<float>(s.center.y));
			return Vec3{ s.center.x, y, s.center.y };
		}
	}

	const float worldCenter = WORLD_SIZE * 0.5f;
	const float y = m_world.computeHeight(worldCenter, worldCenter);
	return Vec3{ worldCenter, y, worldCenter };
}


Vec3 GameScene::captureRuralFringePoint(Vec3 fallback) const
{
	Vec3 best = fallback;
	double bestScore = Math::Inf;
	for (int chunkY = 0; chunkY < WORLD_CHUNKS; ++chunkY)
	{
		for (int chunkX = 0; chunkX < WORLD_CHUNKS; ++chunkX)
		{
			const Chunk* chunk = m_world.getChunk(Point{ chunkX, chunkY });
			if (!chunk) continue;
			for (int row = 1; row < ZONE_CELLS - 1; ++row)
			{
				for (int col = 1; col < ZONE_CELLS - 1; ++col)
				{
					if (chunk->zoneMap[{ col, row }] != ZoneType::Agriculture) continue;

					bool touchesUrban = false;
					for (int dz = -1; dz <= 1 && !touchesUrban; ++dz)
					{
						for (int dx = -1; dx <= 1; ++dx)
						{
							const ZoneType neighbor = chunk->zoneMap[{ col + dx, row + dz }];
							if (neighbor == ZoneType::Commercial || neighbor == ZoneType::Residential
								|| neighbor == ZoneType::LowResidential || neighbor == ZoneType::Industrial)
							{
								touchesUrban = true;
								break;
							}
						}
					}
					if (!touchesUrban) continue;

					const Vec2 center = cellCenterXZ(Point{ chunkX, chunkY }, col, row);
					const double dx = center.x - fallback.x;
					const double dz = center.y - fallback.z;
					const double score = dx * dx + dz * dz;
					if (score < bestScore)
					{
						bestScore = score;
						best = Vec3{ center.x, m_world.computeHeight(static_cast<float>(center.x), static_cast<float>(center.y)), center.y };
					}
				}
			}
		}
	}
	return best;
}
Vec3 GameScene::captureStreetCornerPoint(Vec3 fallback) const
{
	constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
	const float radius = 120.0f;
	const float radiusSq = radius * radius;
	const int cellRadius = static_cast<int>(Ceil(radius / cellSize));
	int bestNode = -1;
	double bestScore = -Math::Inf;
	for (const auto& node : m_network.nodes())
	{
		if (node.id < 0 || node.attachments.size() < 2) continue;
		const double fallbackDx = node.position.x - fallback.x;
		const double fallbackDz = node.position.z - fallback.z;
		const double fallbackDistSq = fallbackDx * fallbackDx + fallbackDz * fallbackDz;
		if (fallbackDistSq > 2400.0 * 2400.0) continue;

		int buildingCount = 0;
		int commercialCount = 0;
		int parkingCount = 0;
		int arterialCount = 0;
		for (const EdgeAttachment& attachment : node.attachments)
		{
			const RoadEdge* edge = m_network.getEdge(attachment.edgeId);
			if (edge && edge->roadType == RoadType::Arterial) ++arterialCount;
		}
		for (int dz = -cellRadius; dz <= cellRadius; ++dz)
		{
			for (int dx = -cellRadius; dx <= cellRadius; ++dx)
			{
				const float wx = static_cast<float>(node.position.x) + dx * cellSize;
				const float wz = static_cast<float>(node.position.z) + dz * cellSize;
				Point cc;
				int col = 0, row = 0;
				worldToZoneCell(wx, wz, cc, col, row);
				const Chunk* chunk = m_world.getChunk(cc);
				if (!chunk) continue;
				const Building& building = chunk->buildingGrid[{ col, row }];
				if (building.type == BuildingType::None || building.type == BuildingType::Farmland) continue;
				const Vec2 center = cellCenterXZ(cc, col, row) + Vec2{ building.offsetX, building.offsetZ };
				const float bx = static_cast<float>(center.x - node.position.x);
				const float bz = static_cast<float>(center.y - node.position.z);
				if (bx * bx + bz * bz > radiusSq) continue;
				++buildingCount;
				if (building.type == BuildingType::Shop || building.type == BuildingType::Office
					|| building.type == BuildingType::PublicFacility) ++commercialCount;
				if (building.type == BuildingType::Parking) ++parkingCount;
			}
		}

		const double degreeBonus = Min<double>(static_cast<double>(node.attachments.size()), 4.0) * 9000.0;
		const double score = buildingCount * 8500.0 + commercialCount * 6500.0 + parkingCount * 3200.0
			+ arterialCount * 32000.0 + degreeBonus - fallbackDistSq * 0.035;
		if (score > bestScore)
		{
			bestScore = score;
			bestNode = node.id;
		}
	}

	if (const RoadNode* node = m_network.getNode(bestNode))
	{
		const float y = m_world.computeHeight(
			static_cast<float>(node->position.x), static_cast<float>(node->position.z));
		return Vec3{ node->position.x, y, node->position.z };
	}
	return fallback;
}
String GameScene::captureFileName(int index) const
{
	switch (index)
	{
	case 0: return U"city_render_01_overall.png";
	case 1: return U"city_render_02_city.png";
	case 2: return U"city_render_03_close.png";
	case 3: return U"city_render_04_rural_fringe.png";
	case 4: return U"city_render_05_intersection.png";
	case 5: return U"city_render_06_street_corner.png";
	default: return U"city_render_done.png";
	}
}

void GameScene::updateCaptureCityRenders()
{
	constexpr int kCaptureCount = 6;
	constexpr int kWarmupFrames = 24;

	m_clock.speed = TimeSpeed::Paused;
	m_clock.hour = 13.0f;
	m_camera.setBlockInput(true);

	if (m_captureCameraDirty)
	{
		const Vec3 focus = captureFocusPoint();
		const Vec3 streetFocus = captureStreetCornerPoint(focus);
		const Vec3 fringeFocus = captureRuralFringePoint(focus);
		switch (m_captureIndex)
		{
		case 0:
			m_camera.setCaptureState(focus, 2800.0f, static_cast<float>(-35.0_deg), static_cast<float>(63.0_deg));
			break;
		case 1:
			m_camera.setCaptureState(focus + Vec3{ 180.0, 0.0, -80.0 }, 1450.0f, static_cast<float>(-42.0_deg), static_cast<float>(50.0_deg));
			break;
		case 2:
			m_camera.setCaptureState(streetFocus + Vec3{ 54.0, 0.0, -42.0 }, 540.0f, static_cast<float>(-48.0_deg), static_cast<float>(38.0_deg));
			break;
		case 3:
			m_camera.setCaptureState(fringeFocus + Vec3{ 90.0, 0.0, -75.0 }, 960.0f, static_cast<float>(-45.0_deg), static_cast<float>(54.0_deg));
			break;
		case 4:
			m_camera.setCaptureState(streetFocus, 260.0f, static_cast<float>(-50.0_deg), static_cast<float>(55.0_deg));
			break;
		case 5:
			m_camera.setCaptureState(streetFocus + Vec3{ 8.0, 0.0, -6.0 }, 150.0f, static_cast<float>(-58.0_deg), static_cast<float>(34.0_deg));
			break;
		}
		m_captureCameraDirty = false;
		m_captureFrame = 0;
	}

	m_world.update(m_camera.focusPoint());
	renderWorld();

	if (++m_captureFrame == kWarmupFrames)
	{
		FileSystem::CreateDirectories(U"Screenshot/city_generation");
		const String fileName = U"city_generation/" + captureFileName(m_captureIndex);
		ScreenCapture::SaveCurrentFrame(fileName);
		DebugLog::print(U"[CaptureCity] saved Screenshot/{}"_fmt(fileName));
	}
	else if (m_captureFrame > kWarmupFrames + 8)
	{
		++m_captureIndex;
		if (m_captureIndex >= kCaptureCount)
		{
			if (!m_cityConstraintValidationPassed)
			{
				TextWriter writer{ U"Screenshot/city_generation/constraint_validation_failed.txt" };
				if (writer) writer << U"City constraint validation failed: " + m_cityConstraintValidationSummary;
			}
			System::Exit();
			return;
		}
		m_captureCameraDirty = true;
		m_captureFrame = 0;
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

	if (getData().captureCityRenders)
	{
		updateCaptureCityRenders();
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
		const int64 monthIndexBefore = m_clock.monthIndex();
		const double gameDt = dt * m_clock.speedMultiplier() * 60.0; // 物理用: 実時間1秒=ゲーム内60秒相当の移動
		const double vehicleDt = gameDt / 60.0;
		m_clock.advance(dt);
		const int64 monthIndexAfter = m_clock.monthIndex();

		if (monthIndexAfter > monthIndexBefore)
		{
			for (int64 monthIndex = monthIndexBefore + 1; monthIndex <= monthIndexAfter; ++monthIndex)
			{
				const GameTime monthStartTime = GameClock::TimeFromMonthIndex(monthIndex);
				const uint8 month = GameClock::MonthFromMonthIndex(monthIndex);
				m_eventSystem.rollMonthly(monthStartTime, month, monthIndex, m_network);
				m_citySnapshot = collectCitySnapshot(m_world, m_network,
					m_vehicleManager.vehicles(), m_vehicleManager.completedTripMinutes());
				m_lastMonthlyEconomy = m_economy.applyMonthly(m_network, m_citySnapshot,
					m_busSystem.activeRouteCount());
			}
		}

		if (m_simGraph)
		{
			m_vehicleManager.update(vehicleDt, m_clock.now, *m_simGraph,
			                        m_network, m_roadRenderer.visibleEdges());
		}

		m_trainManager.update(gameDt, m_clock.now);
		m_eventSystem.update(m_clock.now);
		for (auto& event : m_eventSystem.popNewNotifications())
		{
			m_notifications << std::move(event);
		}
		while (m_notifications.size() > 5)
		{
			m_notifications.erase(m_notifications.begin());
		}
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