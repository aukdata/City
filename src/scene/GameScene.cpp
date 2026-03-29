#include "GameScene.hpp"
#include "../gen/RoadPathfinder.hpp"
#include "../save/RoadBinary.hpp"
#include "../sim/SimGraph.hpp"
#include <Siv3D/ViewFrustum.hpp>
#include <thread>

// ─────────────────────────────────────────────────────────────────────────────
// 初期化
// ─────────────────────────────────────────────────────────────────────────────

GameScene::GameScene(const InitData& init)
	: IScene{ init }
{
	m_renderTexture = RenderTexture{ Scene::Size(), TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes };
	m_trainManager.init(&m_trainNetwork);

	m_roadRenderer.loadStyle(U"assets/roads");

	m_sandboxActive = getData().sandboxMode;

	initScene();
}

GameScene::~GameScene()
{
	m_simThread.stop();

	// バックグラウンド生成タスクの完了を待機
	if (m_generationFuture.valid())
		m_generationFuture.wait();
}

void GameScene::initScene()
{
	if (!getData().saveName.isEmpty() && loadGame())
	{
		m_phase = GamePhase::Playing;
		return;
	}
	initNewGame();
}

void GameScene::initNewGame()
{
	// ワールドパラメータ設定 + 地名生成（軽量・即座に完了）
	const auto initResult = MapGenerator::initWorld(getData().seed, m_world);
	m_placeNames = std::move(initResult.placeNames);

	// World の配列を事前確保
	m_world.reserveChunks();

	// カメラをマップ中央に設定
	const float worldCenter = static_cast<float>(WORLD_CHUNKS) * CHUNK_SIZE * 0.5f;
	m_camera.setFocus(Vec3{ worldCenter, 0.0, worldCenter });

	// 初期チャンク数
	m_totalInitChunks = WORLD_CHUNKS * WORLD_CHUNKS;
	m_genProgress = 0.0f;

	// 全パイプラインをバックグラウンドで実行
	m_generationFuture = std::async(std::launch::async, [this]()
	{
		generateAllTerrain();   // Phase 1
		placeAllSettlements();  // Phase 2
		generateAllRoads();     // Phase 3a
		generateDistrictRoads(); // Phase 3b
		postProcessRoads();     // Phase 4
	});

	m_loadingTimer.restart();
	m_loadingStatus = U"地形生成中";
	Logger << U"[Loading] {} チャンク生成開始"_fmt(m_totalInitChunks);
	m_phase = GamePhase::Loading;
}

// ─────────────────────────────────────────────────────────────────────────────
// バックグラウンド生成パイプライン
// ─────────────────────────────────────────────────────────────────────────────

// 進捗の重み配分（実測ベース）
// 地形=3%, 地区配置=22%, グローバル道路=49%, 地区内道路=5%, ポスト処理=12%, Setup=9%
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

	// 各地区のバイオームを取得し、バイオームに応じた地名を生成
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

// ─────────────────────────────────────────────────────────────────────────────
// Loading フェーズ更新
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::updateLoading()
{
	if (m_generationFuture.valid())
	{
		if (m_generationFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			m_generationFuture.get();

			Logger << U"[Loading] 全パイプライン完了 ({:.1f}秒)"_fmt(m_loadingTimer.sF());
			m_loadingStatus = U"初期化中...";

			// メインスレッド側のセットアップ
			const Stopwatch setupTimer{ StartImmediately::Yes };
			Stopwatch step{ StartImmediately::Yes };

			applyZonesGlobal();
			placeInitialBuildings();
			m_roadRenderer.invalidateAllCaches();
			MapGenerator::setupTrain(m_trainNetwork, m_world, m_districts);

			// カメラ初期位置（最初の Urban 地区）
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

			Logger << U"[Phase] 全体 {:.1f}秒 → Playing へ遷移"_fmt(m_loadingTimer.sF());
			m_phase = GamePhase::Playing;
			return;
		}
	}

	// 進捗描画
	drawLoadingScreen(Clamp(m_genProgress.load(), 0.0f, 1.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
// SimThread 起動
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::startSimThread()
{
	auto simGraph = std::make_shared<const SimGraph>(SimGraph::build(m_network));

	TrafficManager traffic;
	traffic.setSimGraph(simGraph);
	traffic.markNetworkDirty();
	// TrafficGraph をメインスレッドで事前構築（SimThread 内でロック保持中に rebuild するのを回避）
	traffic.update(0.0, 0.0);

	m_simThread.start(std::move(traffic), TrainManager{},
	                  EventSystem{}, m_clock, simGraph);
}

// ─────────────────────────────────────────────────────────────────────────────
// ローディング画面描画
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::drawLoadingScreen(float progress)
{
	Scene::Rect().draw(ColorF{ 0.07, 0.11, 0.16 });

	static const Font titleFont{ FontMethod::MSDF, 48, Typeface::Bold };
	static const Font uiFont{ FontMethod::MSDF, 20 };
	static const Font smallFont{ FontMethod::MSDF, 16 };

	const Vec2 center = Scene::Center();

	// 経過時間
	const int elapsedSec = static_cast<int>(m_loadingTimer.sF());
	const int min = elapsedSec / 60;
	const int sec = elapsedSec % 60;
	uiFont(U"{}:{:0>2}"_fmt(min, sec)).drawAt(
		center.movedBy(0, -110), ColorF{ 0.7 });

	titleFont(U"マップ生成中...").drawAt(center.movedBy(0, -60), ColorF{ 0.9 });

	// プログレスバー
	const RectF barBg{ center.x - 200, center.y, 400, 24 };
	barBg.draw(ColorF{ 0.2 });
	RectF{ barBg.pos, barBg.w * progress, barBg.h }.draw(ColorF{ 0.3, 0.7, 1.0 });

	// パーセント
	uiFont(U"{}%"_fmt(static_cast<int>(progress * 100))).drawAt(barBg.center(), ColorF{ 1.0 });

	// 実行中の処理内容
	smallFont(m_loadingStatus).drawAt(
		center.movedBy(0, 70), ColorF{ 0.5 });
}

// ─────────────────────────────────────────────────────────────────────────────
// セーブ
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::saveGame()
{
	if (getData().saveName.isEmpty())
		getData().saveName = U"default";

	const String kRoot = U"saves/{}"_fmt(getData().saveName);
	FileSystem::CreateDirectories(U"{}/global"_fmt(kRoot));

	// ---- meta.json ----
	JSON meta;
	meta[U"version"]      = 3;
	meta[U"seed"]         = getData().seed;
	meta[U"worldChunks"]  = WORLD_CHUNKS;
	meta[U"gameNow"]      = m_clock.now;
	meta[U"timeScale"]    = static_cast<int>(m_clock.speed);
	meta[U"nextNodeId"]   = m_network.nextNodeId();
	meta[U"nextEdgeId"]   = m_network.nextEdgeId();
	meta[U"cameraFocusX"] = m_camera.focusPoint().x;
	meta[U"cameraFocusY"] = m_camera.focusPoint().y;
	meta[U"cameraFocusZ"] = m_camera.focusPoint().z;
	meta.save(U"{}/meta.json"_fmt(kRoot));

	// ---- global/economy.json ----
	JSON eco;
	eco[U"funds"]      = m_economy.funds;
	eco[U"population"] = m_economy.population;
	eco[U"happiness"]  = m_economy.happiness;
	eco.save(U"{}/global/economy.json"_fmt(kRoot));

	// ---- global/roads.bin ----
	RoadBinary::writeGlobal(U"{}/global/roads.bin"_fmt(kRoot), m_network);

	// ---- global/districts.json ----
	JSON dist;
	dist[U"count"] = static_cast<int>(m_districts.size());
	for (int i = 0; i < static_cast<int>(m_districts.size()); ++i)
	{
		const auto& s = m_districts[i];
		dist[U"type_{}"_fmt(i)] = static_cast<int>(s.type);
		dist[U"cx_{}"_fmt(i)]   = s.center.x;
		dist[U"cy_{}"_fmt(i)]   = s.center.y;
		dist[U"name_{}"_fmt(i)] = s.name;
	}
	dist.save(U"{}/global/districts.json"_fmt(kRoot));
}

// ─────────────────────────────────────────────────────────────────────────────
// ロード
// ─────────────────────────────────────────────────────────────────────────────

bool GameScene::loadGame()
{
	const String kRoot = U"saves/{}"_fmt(getData().saveName);

	// ---- meta.json ----
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

	// ---- 地形パラメータ・地名を初期化 ----
	{
		const auto initResult = MapGenerator::initWorld(
			getData().seed, m_world);
		m_placeNames = std::move(initResult.placeNames);
	}

	// World の配列を事前確保 + 地形再生成
	m_world.reserveChunks();
	for (int cy = 0; cy < WORLD_CHUNKS; ++cy)
		for (int cx = 0; cx < WORLD_CHUNKS; ++cx)
			m_world.installChunkDirect(Point{ cx, cy },
				m_world.buildHeightMap(Point{ cx, cy }));

	m_districts.clear();
	m_urbanCenters.clear();

	// ---- economy.json ----
	const JSON eco = JSON::Load(U"{}/global/economy.json"_fmt(kRoot));
	if (eco)
	{
		m_economy.funds      = eco[U"funds"].get<double>();
		m_economy.population = eco[U"population"].get<int>();
		m_economy.happiness  = eco[U"happiness"].get<double>();
	}

	// ---- global/roads.bin ----
	if (!RoadBinary::readGlobal(U"{}/global/roads.bin"_fmt(kRoot), m_network))
	{
		// フォールバック: 旧形式（per-chunk）からの読み込みを試行
		const FilePath chunksDir = U"{}/chunks/"_fmt(kRoot);
		if (!FileSystem::Exists(chunksDir)) return false;

		for (const auto& entry : FileSystem::DirectoryContents(chunksDir, Recursive::No))
		{
			if (!FileSystem::IsDirectory(entry)) continue;
			Array<RoadNode> loadedNodes;
			Array<RoadEdge> loadedEdges;
			if (!RoadBinary::read(U"{}/roads.bin"_fmt(entry), loadedNodes, loadedEdges))
				continue;
			for (const auto& n : loadedNodes) m_network.addNodeRaw(n);
			for (const auto& e : loadedEdges) m_network.addEdgeRaw(e);
		}
	}

	// ---- districts.json ----
	const JSON dist = JSON::Load(U"{}/global/districts.json"_fmt(kRoot));
	if (dist)
	{
		const int count = dist[U"count"].get<int>();
		Array<MapGenerator::Settlement> settlements;
		for (int i = 0; i < count; ++i)
		{
			MapGenerator::Settlement s;
			s.type   = static_cast<MapGenerator::SettlementType>(
			               dist[U"type_{}"_fmt(i)].get<int>());
			s.center = Vec2{ dist[U"cx_{}"_fmt(i)].get<double>(),
			                 dist[U"cy_{}"_fmt(i)].get<double>() };
			s.name   = dist[U"name_{}"_fmt(i)].get<String>();
			settlements << s;
		}
		addDistricts(settlements);
	}
	else
	{
		// フォールバック: 旧形式 per-chunk districts
		const FilePath chunksDir = U"{}/chunks/"_fmt(kRoot);
		if (FileSystem::Exists(chunksDir))
		{
			for (const auto& entry : FileSystem::DirectoryContents(chunksDir, Recursive::No))
			{
				if (!FileSystem::IsDirectory(entry)) continue;
				const JSON cdist = JSON::Load(U"{}/districts.json"_fmt(entry));
				if (!cdist) continue;
				const int count = cdist[U"count"].get<int>();
				Array<MapGenerator::Settlement> settlements;
				for (int i = 0; i < count; ++i)
				{
					MapGenerator::Settlement s;
					s.type   = static_cast<MapGenerator::SettlementType>(
					               cdist[U"type_{}"_fmt(i)].get<int>());
					s.center = Vec2{ cdist[U"cx_{}"_fmt(i)].get<double>(),
					                 cdist[U"cy_{}"_fmt(i)].get<double>() };
					s.name   = cdist[U"name_{}"_fmt(i)].get<String>();
					settlements << s;
				}
				addDistricts(settlements);
			}
		}
	}

	// ---- nextNodeId / nextEdgeId を復元 ----
	m_network.setNextIds(nextNodeId, nextEdgeId);

	// ---- レンダラに通知 ----
	m_roadRenderer.invalidateAllCaches();

	// ---- ゲーム時刻を復元 ----
	m_clock.now   = gameNow;
	m_clock.speed = static_cast<TimeSpeed>(timeScale);
	m_clock.syncCalendar();

	// ---- カメラを復元 ----
	m_camera.setFocus(Vec3{ focusX, focusY, focusZ });

	// ---- 鉄道初期設定 ----
	MapGenerator::setupTrain(m_trainNetwork, m_world, m_districts);

	// ---- ワールド更新 ----
	m_world.update(m_camera.focusPoint());

	// ---- SimThread 起動 ----
	startSimThread();

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 地区リストを一括更新
// ─────────────────────────────────────────────────────────────────────────────

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

// ─────────────────────────────────────────────────────────────────────────────
// ゾーン一括割り当て
// ─────────────────────────────────────────────────────────────────────────────

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

		// 影響範囲のチャンクのみ走査（+1 不要: ceil で十分）
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

				// セル範囲を outerDist で絞り込む
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

						// heightMap から直接高さ取得（sampleHeight のチャンク検索を回避）
						const float h = sampleHeightMap(chunk->heightMap, cc, wx, wz);
						if (h < 0.0f) continue;

						// zoneMap に直接書き込み（paintZone のチャンク検索を回避）
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

// ─────────────────────────────────────────────────────────────────────────────
// 初期建物配置
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	struct RoadSample { float x; float z; float angle; float halfWidth; };

	/// @brief 集落周辺の道路サンプル点を収集する
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

			// エッジのバウンディングボックスで粗いフィルタ
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

				// タンジェント
				const float tanx = t1*t1*(p1x-p0x) + 2.0f*t1*t*(p2x-p1x) + t*t*(p3x-p2x);
				const float tanz = t1*t1*(p1z-p0z) + 2.0f*t1*t*(p2z-p1z) + t*t*(p3z-p2z);
				samples.push_back({ sx, sz, std::atan2(tanz, tanx), edgeHalfWidth });
			}
		}
		return samples;
	}

	/// @brief サンプル点リストから最寄り道路情報を返す (距離, 角度, 道路半幅)
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

		// 集落周辺の道路サンプル点を事前収集
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

						// 既に建物がある場合はスキップ
						if (chunk->buildingGrid[{ gx, gz }].type != BuildingType::None) continue;

						const float wx = chunkOriginX + (gx + 0.5f) * cellSize;
						const float wz = chunkOriginZ + (gz + 0.5f) * cellSize;

						// 集落中心からの距離
						const float dx = wx - scx;
						const float dz = wz - scz;
						const float distFromCenterSq = dx*dx + dz*dz;
						if (distFromCenterSq > radiusSq) continue;

						// 水面チェック
						const float h = sampleHeightMap(chunk->heightMap, cc, wx, wz);
						if (h < 0.0f) continue;

						// 道路距離チェック: 道路中心から totalWidth 分だけ離す
						const auto [roadDist, angle, roadHW] = nearestFromSamples(wx, wz, roadSamples);
						if (roadDist < roadHW * 2.0f) continue;  // 道路幅分のクリアランス

						// 道路近接スコア (道路に近いほど高い)
						float roadScore;
						if      (roadDist < kNearDist) roadScore = 1.0f;
						else if (roadDist < kFarDist)  roadScore = 1.0f - (roadDist - kNearDist) / (kFarDist - kNearDist);
						else                           roadScore = 0.0f;

						// 中心からの距離による密度減衰（線形: 中心=0.25, 2*radius=0）
						const float distFromCenter = Math::Sqrt(distFromCenterSq);
						const float densityFactor = Clamp(0.25f * (1.0f - distFromCenter / (s.radius * 2.0f)), 0.0f, 0.25f);

						// 最終スコア
						const float score = roadScore * densityFactor;
						if (score < 0.02f) continue;

						// 確率的間引き: スコアに比例して配置確率を決定
						// 決定論的ハッシュ（セル座標ベース）
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

// ─────────────────────────────────────────────────────────────────────────────
// 毎フレーム更新（ロジック + 描画）
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::update()
{
	if (m_phase == GamePhase::Loading)
	{
		updateLoading();
		return;
	}

	const double dt = Scene::DeltaTime();

	// ---- SimThread から時刻を同期 ----
	{
		auto lock = m_simThread.lockForRead();
		m_clock = m_simThread.clock();
	}

	// ---- 通知を取得 ----
	for (const auto& n : m_simThread.popNotifications())
	{
		m_notifications << n;
		if (static_cast<int>(m_notifications.size()) > 5)
			m_notifications.erase(m_notifications.begin());
	}

	// ---- メインスレッドのロジック ----
	const Stopwatch swLogic{ StartImmediately::Yes };
	m_world.update(m_camera.focusPoint());
	m_camera.update(dt, m_world);
	m_logicMs = swLogic.msF();

	// フォローカメラ
	if (m_camera.mode() == CameraMode::Follow)
	{
		auto lock = m_simThread.lockForRead();
		const auto& vehicles = m_simThread.vehicles();
		if (!vehicles.isEmpty())
		{
			m_followVehicleIdx = m_followVehicleIdx % static_cast<int>(vehicles.size());
			const Vehicle& v = vehicles[m_followVehicleIdx];
			m_camera.setFollowTarget(v.position, v.heading);
		}
	}

	handleInput();
	updateCursor();
	m_debugRenderer.handleInput();

	// ---- ゲーム速度を SimThread に同期 ----
	m_simThread.setSpeed(m_clock.speed);

	// ---- 描画 ----
	renderWorld();
}

// ─────────────────────────────────────────────────────────────────────────────
// 描画
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::renderWorld()
{
	// ---- 描画時間計測 ----
	static double s_sky = 0, s_terrain = 0, s_road = 0, s_zone = 0;
	static double s_vehicle = 0, s_train = 0, s_debug = 0, s_ui = 0, s_total = 0;
	Stopwatch swStep{ StartImmediately::Yes };
	const Stopwatch swTotal{ StartImmediately::Yes };

	auto lap = [&](double& out) { out = swStep.msF(); swStep.restart(); };

	// ---- 太陽・空のパラメータ計算 ----
	const float hour = m_clock.hour;
	const float t    = (hour - 6.0f) * static_cast<float>(Math::Pi / 12.0);
	const float sinT  = static_cast<float>(Math::Sin(t));
	const float dayF  = Clamp(sinT, 0.0f, 1.0f);
	const float dawnF = Clamp(1.0f - Abs(sinT) * 2.5f, 0.0f, 1.0f);
	const double exposure = 0.15 + 0.85 * dayF + 0.30 * dawnF;

	// ---- 3D シーンを深度バッファ付きレンダーテクスチャに描画 ----
	{
		const ScopedRenderTarget3D target{ m_renderTexture.clear(ColorF{ 0.2, 0.3, 0.4 }.removeSRGBCurve()) };
		const ScopedRenderStates3D depthState{ DepthStencilState::DepthTestWrite };

		Graphics3D::SetCameraTransform(m_camera.camera3D());

		const Vec3 sunDir = Vec3{ Math::Cos(t), sinT, 0.3 }.normalized();
		Graphics3D::SetSunDirection(sunDir);
		Graphics3D::SetGlobalAmbientColor(ColorF{ 0.55 + 0.30 * dayF + 0.10 * dawnF });

		const ColorF dayZenith  { 0.10, 0.35, 0.80 };
		const ColorF dawnZenith { 0.22, 0.18, 0.38 };
		const ColorF nightZenith{ 0.01, 0.02, 0.07 };
		m_sky.zenithColor = nightZenith.lerp(dawnZenith, dawnF).lerp(dayZenith, dayF);

		const ColorF dayHorizon  { 0.60, 0.78, 0.95 };
		const ColorF dawnHorizon { 0.85, 0.42, 0.15 };
		const ColorF nightHorizon{ 0.02, 0.03, 0.10 };
		m_sky.horizonColor = nightHorizon.lerp(dawnHorizon, dawnF).lerp(dayHorizon, dayF);

		m_sky.starBrightness = Clamp(1.0 - dayF * 3.0 - dawnF * 2.0, 0.0, 1.0);
		m_sky.cloudTime = Scene::Time() * 0.015;
		m_sky.draw(exposure);
		lap(s_sky);

		const ViewFrustum frustum{ m_camera.camera3D(), 24000.0 };
		m_worldRenderer.render(m_world, m_camera.camera3D());
		lap(s_terrain);

		m_roadRenderer.render(m_network, m_world, frustum,
		                     m_camera.camera3D().getEyePosition());

		// 選択中の Edge / Node をハイライト描画
		if (m_selectedEdgeId)
		{
			if (const auto bez = m_network.getBezier(*m_selectedEdgeId))
			{
				constexpr int kDiv = 30;
				const float len = bez->totalLength;
				for (int i = 0; i < kDiv; ++i)
				{
					const Vec3 a = bez->positionAt(len * (i / static_cast<float>(kDiv)));
					const Vec3 b = bez->positionAt(len * ((i + 1) / static_cast<float>(kDiv)));
					const float ha = m_world.computeHeight(static_cast<float>(a.x), static_cast<float>(a.z));
					const float hb = m_world.computeHeight(static_cast<float>(b.x), static_cast<float>(b.z));
					const Vec3 pa{ a.x, ha + 3.0, a.z };
					const Vec3 pb{ b.x, hb + 3.0, b.z };
					Cylinder{ pa, pb, 1.0 }.draw(ColorF{ 0.2, 0.8, 1.0, 0.6 });
				}
			}
		}
		if (m_selectedNodeId)
		{
			if (const auto* node = m_network.getNode(*m_selectedNodeId))
			{
				const float nh = m_world.computeHeight(
					static_cast<float>(node->position.x), static_cast<float>(node->position.z));
				Sphere{ Vec3{ node->position.x, nh + 5.0, node->position.z }, 12.0 }
					.draw(ColorF{ 1.0, 0.5, 0.0, 0.6 });
			}
		}

		lap(s_road);

		m_zoneManager.renderOverlay(m_world);
		lap(s_zone);

		// 車両の (edgeId, arcPos) → ワールド座標に変換して描画する
		{
			{
				auto lock = m_simThread.lockForRead();
				m_renderVehicles = m_simThread.vehicles();
			}

			for (auto& v : m_renderVehicles)
			{
				if (v.currentEdge < 0) continue;
				if (const auto bezier = m_network.getBezier(v.currentEdge))
				{
					const float clampedArc = Clamp(v.arcPos, 0.0f, bezier->totalLength);
					v.position = bezier->positionAt(clampedArc);
					v.position.y = m_world.sampleHeight(
						static_cast<float>(v.position.x),
						static_cast<float>(v.position.z)) + 2.0f;
					const Vec3 tangent = bezier->tangentAt(clampedArc);
					const RoadEdge* edge = m_network.getEdge(v.currentEdge);
					const bool fwd = (!edge || v.currentLane >= static_cast<int>(edge->lanes.size()))
						? true : (edge->lanes[v.currentLane].dir == LaneDir::Forward);
					const float sign = fwd ? 1.0f : -1.0f;
					v.heading = static_cast<float>(Math::Atan2(sign * tangent.x, sign * tangent.z));
				}
			}
			m_vehicleRenderer.render(m_renderVehicles, m_camera.camera3D().getEyePosition());
		}
		lap(s_vehicle);

		m_trainRenderer.renderTracks(m_trainNetwork);
		m_trainRenderer.renderTrains(m_trainManager.trains());
		lap(s_train);

		if (m_mode == EditMode::TrainDraw && m_cursorGroundPos)
		{
			Sphere{ *m_cursorGroundPos, 6.0f }.draw(ColorF{ 0.9, 0.85, 0.2, 0.8 }.removeSRGBCurve());
		}

		if (m_mode == EditMode::BusRouteDraw && m_cursorGroundPos)
		{
			Sphere{ *m_cursorGroundPos, 4.0f }.draw(ColorF{ 0.2, 0.5, 0.9, 0.8 }.removeSRGBCurve());
		}

		if (m_mode == EditMode::RoadDraw && m_cursorGroundPos)
		{
			Sphere{ *m_cursorGroundPos, 5.0f }.draw(ColorF{ 1, 1, 0, 0.8 }.removeSRGBCurve());
		}

		if (m_mode == EditMode::ZonePaint && m_rectStart && m_cursorGroundPos)
		{
			const double minX = Min(m_rectStart->x, m_cursorGroundPos->x);
			const double maxX = Max(m_rectStart->x, m_cursorGroundPos->x);
			const double minZ = Min(m_rectStart->z, m_cursorGroundPos->z);
			const double maxZ = Max(m_rectStart->z, m_cursorGroundPos->z);
			const double cx = (minX + maxX) * 0.5;
			const double cz = (minZ + maxZ) * 0.5;
			const double sx = Max(maxX - minX, 1.0);
			const double sz = Max(maxZ - minZ, 1.0);
			const ColorF zc = zoneColor(m_paintZone);
			Box{ cx, 0.5, cz, sx, 1.0, sz }.draw(ColorF{ zc.r, zc.g, zc.b, 0.3 }.removeSRGBCurve());
		}

		if (m_mode == EditMode::TerrainEdit && m_cursorGroundPos)
		{
			Cylinder{ *m_cursorGroundPos + Vec3{0, -1, 0},
			          *m_cursorGroundPos + Vec3{0, 2, 0},
			          static_cast<double>(m_terrainBrushRadius) }
				.draw(ColorF{ 0.9, 0.6, 0.2, 0.25 }.removeSRGBCurve());
		}

		if (m_mode == EditMode::SandboxEdit)
		{
			const Vec2 cur2D = m_cursorGroundPos
				? Vec2{ m_cursorGroundPos->x, m_cursorGroundPos->z } : Vec2{ 0, 0 };

			for (const auto& edge : m_network.edges())
			{
				if (edge.id < 0) continue;
				const RoadNode* nA = m_network.getNode(edge.nodeA);
				const RoadNode* nB = m_network.getNode(edge.nodeB);
				if (!nA || !nB) continue;

				const auto isHovCtrl = [&](Vec3 cp) {
					return m_cursorGroundPos &&
					       Vec2{ cp.x, cp.z }.distanceFrom(cur2D) < 18.0f;
				};
				const bool dragA = m_sandboxDragCtrl &&
				    m_sandboxDragCtrl->edgeId == edge.id && m_sandboxDragCtrl->isControlPointA;
				const bool dragB = m_sandboxDragCtrl &&
				    m_sandboxDragCtrl->edgeId == edge.id && !m_sandboxDragCtrl->isControlPointA;

				const ColorF colA = dragA
				    ? ColorF{ 1.0, 0.5, 0.0, 1.0 }
				    : (isHovCtrl(edge.ctrlA) ? ColorF{ 1.0, 1.0, 0.3, 0.9 }
				                             : ColorF{ 0.2, 0.9, 0.4, 0.7 });
				Line3D{ nA->position + Vec3{0,2,0}, edge.ctrlA + Vec3{0,2,0} }
					.draw(ColorF{ 0.5, 0.5, 0.5, 0.5 }.removeSRGBCurve());
				Sphere{ edge.ctrlA + Vec3{0,2,0}, dragA ? 6.0 : 4.0 }
					.draw(colA.removeSRGBCurve());

				const ColorF colB = dragB
				    ? ColorF{ 1.0, 0.5, 0.0, 1.0 }
				    : (isHovCtrl(edge.ctrlB) ? ColorF{ 1.0, 1.0, 0.3, 0.9 }
				                             : ColorF{ 0.2, 0.9, 0.4, 0.7 });
				Line3D{ nB->position + Vec3{0,2,0}, edge.ctrlB + Vec3{0,2,0} }
					.draw(ColorF{ 0.5, 0.5, 0.5, 0.5 }.removeSRGBCurve());
				Sphere{ edge.ctrlB + Vec3{0,2,0}, dragB ? 6.0 : 4.0 }
					.draw(colB.removeSRGBCurve());
			}

			for (const auto& node : m_network.nodes())
			{
				if (node.id < 0) continue;
				const bool dragging = m_sandboxDragNode && (*m_sandboxDragNode == node.id);
				const bool hovered  = m_cursorGroundPos &&
				    Vec2{ node.position.x, node.position.z }.distanceFrom(cur2D) < 20.0f;
				const ColorF col = dragging
				    ? ColorF{ 1.0, 0.4, 0.1, 0.95 }
				    : (hovered ? ColorF{ 1.0, 1.0, 0.2, 0.9 }
				               : ColorF{ 0.3, 0.8, 1.0, 0.7 });
				Sphere{ node.position + Vec3{0, 2, 0}, dragging ? 7.0 : 5.0 }
					.draw(col.removeSRGBCurve());
			}
		}

		m_debugRenderer.render(m_network, m_renderVehicles, m_world, m_camera);
		lap(s_debug);
	}

	Graphics3D::Flush();
	Shader::LinearToScreen(m_renderTexture);

	// ---- UI（2D）----
	m_placeNameRenderer.render(m_districts, m_camera, m_world);
	m_uiRenderer.render(m_clock, static_cast<int>(m_renderVehicles.size()), modeString(), m_economy);

	// ---- 地名リストパネル ----
	if (m_showNameList)
	{
		static const Font listFont{ FontMethod::MSDF, 14 };
		static const Font headerFont{ FontMethod::MSDF, 16, Typeface::Bold };

		constexpr int kPanelW = 250;
		constexpr int kLineH  = 22;
		constexpr int kPad    = 8;
		const int panelH = Scene::Height() - 20;
		const int px = Scene::Width() - kPanelW - 10;
		const int py = 10;

		// 背景
		RectF{ static_cast<double>(px), static_cast<double>(py),
		       static_cast<double>(kPanelW), static_cast<double>(panelH) }
			.draw(ColorF{ 0, 0, 0, 0.7 });

		// ヘッダ
		headerFont(U"地名リスト (N)").draw(Vec2{ px + kPad, py + kPad }, Palette::Yellow);

		// スクロール（ホイール）
		if (RectF{ static_cast<double>(px), static_cast<double>(py),
		           static_cast<double>(kPanelW), static_cast<double>(panelH) }.mouseOver())
		{
			m_nameListScroll -= Mouse::Wheel() * kLineH * 3;
		}

		const int visibleLines = (panelH - kPad * 2 - kLineH) / kLineH;
		const int maxScroll = Max(0, static_cast<int>(m_districts.size()) - visibleLines) * kLineH;
		m_nameListScroll = Clamp(m_nameListScroll, 0.0, static_cast<double>(maxScroll));

		const int startIdx = static_cast<int>(m_nameListScroll / kLineH);

		for (int i = 0; i < visibleLines && (startIdx + i) < static_cast<int>(m_districts.size()); ++i)
		{
			const int di = startIdx + i;
			const auto& s = m_districts[di];

			const int ly = py + kPad + kLineH + i * kLineH;

			// 種別ラベル
			StringView typeStr;
			ColorF typeColor;
			switch (s.type)
			{
			case MapGenerator::SettlementType::Urban:
				typeStr = U"[U]"; typeColor = ColorF{ 1.0, 0.4, 0.4 }; break;
			case MapGenerator::SettlementType::Suburbs:
				typeStr = U"[S]"; typeColor = ColorF{ 0.4, 0.8, 1.0 }; break;
			default:
				typeStr = U"[R]"; typeColor = ColorF{ 0.6, 0.8, 0.5 }; break;
			}

			// クリック判定
			const RectF itemRect{ static_cast<double>(px + kPad), static_cast<double>(ly),
			                      static_cast<double>(kPanelW - kPad * 2), static_cast<double>(kLineH) };
			const bool hovered = itemRect.mouseOver();

			if (hovered)
				itemRect.draw(ColorF{ 1, 1, 1, 0.1 });

			listFont(typeStr).draw(Vec2{ px + kPad, ly + 2 }, typeColor);
			listFont(s.name).draw(Vec2{ px + kPad + 30, ly + 2 },
				hovered ? Palette::Yellow : Palette::White);

			if (hovered && MouseL.down())
			{
				const float h = m_world.computeHeight(
					static_cast<float>(s.center.x), static_cast<float>(s.center.y));
				m_camera.setFocus(Vec3{ s.center.x, h, s.center.y });
				if (m_camera.mode() != CameraMode::Overview)
					m_camera.cycleMode();  // 俯瞰に戻す
			}
		}
	}

	// ---- 道路情報パネル ----
	if (m_selectedEdgeId)
	{
		const RoadEdge* edge = m_network.getEdge(*m_selectedEdgeId);
		if (edge)
		{
			static const Font pFont{ FontMethod::MSDF, 12 };
			static const Font pHeader{ FontMethod::MSDF, 14, Typeface::Bold };

			constexpr int kPW = 320;
			constexpr int kPad = 8;
			constexpr int kLH = 18;
			const int pH = Scene::Height() - 20;
			const int pX = Scene::Width() - kPW - 10;
			const int pY = 10;

			RectF{ static_cast<double>(pX), static_cast<double>(pY),
			       static_cast<double>(kPW), static_cast<double>(pH) }
				.draw(ColorF{ 0, 0, 0, 0.8 });

			int y = pY + kPad;
			auto line = [&](const String& text, ColorF color = Palette::White)
			{
				pFont(text).draw(Vec2{ pX + kPad, y }, color);
				y += kLH;
			};
			auto header = [&](const String& text)
			{
				pHeader(text).draw(Vec2{ pX + kPad, y }, Palette::Yellow);
				y += kLH + 4;
			};

			static constexpr StringView rtNames[] = { U"LocalRoad", U"Arterial", U"Expressway", U"Highway" };
			static constexpr StringView ntNames[] = { U"Endpoint", U"Joint", U"Intersection", U"Diverge" };
			static constexpr StringView bsNames[] = { U"NotBuilt", U"UnderConstr", U"Built", U"StubEnd" };
			static constexpr StringView osNames[] = { U"Open", U"Provisional", U"Closed", U"Reserved" };
			static constexpr StringView dirNames[] = { U"Forward", U"Backward" };
			static constexpr StringView ltNames[] = { U"None", U"SolidW", U"DashW", U"SolidY", U"DblY" };
			static constexpr StringView ptNames[] = { U"Roadbed", U"Shoulder", U"Median", U"Sidewalk", U"Gutter", U"Guardrail", U"Wall", U"Curb", U"Slope", U"BikeLane" };

			// -- Edge 基本情報 --
			header(U"RoadEdge #{}"_fmt(edge->id));
			line(U"nodeA: {}  nodeB: {}"_fmt(edge->nodeA, edge->nodeB));
			line(U"type: {}  speed: {} km/h"_fmt(rtNames[static_cast<int>(edge->roadType)], edge->speedLimit));
			line(U"length: {:.1f} m  cutoff: {:.1f}/{:.1f}"_fmt(edge->length, edge->cutoffA, edge->cutoffB));
			line(U"state: {}  congestion: {:.2f}"_fmt(static_cast<int>(edge->edgeState), edge->congestion));
			line(U"totalWidth: {:.1f} m"_fmt(edge->totalWidth()));
			y += 4;

			// -- Parts --
			header(U"Parts ({})"_fmt(edge->parts.size()));
			for (size_t i = 0; i < edge->parts.size(); ++i)
			{
				const auto& p = edge->parts[i];
				line(U"[{}] {} w={:.1f} off={:.1f} {}"_fmt(
					i, ptNames[static_cast<int>(p.type)], p.width, p.offset,
					bsNames[static_cast<int>(p.build)]));
			}
			y += 4;

			// -- Lanes --
			header(U"Lanes ({})"_fmt(edge->lanes.size()));
			for (size_t i = 0; i < edge->lanes.size(); ++i)
			{
				const auto& L = edge->lanes[i];
				line(U"[{}] {} {} nw={:.1f}"_fmt(
					i, dirNames[static_cast<int>(L.dir)],
					osNames[static_cast<int>(L.op)], L.nominalWidth));
				line(U"    A: {:.1f}~{:.1f}  B: {:.1f}~{:.1f}"_fmt(
					L.offsetA_L, L.offsetA_R, L.offsetB_L, L.offsetB_R));
				line(U"    line: {}/{} chg: {}/{}"_fmt(
					ltNames[static_cast<int>(L.lineLeft)],
					ltNames[static_cast<int>(L.lineRight)],
					L.canChangeLaneLeft ? U"Y" : U"N",
					L.canChangeLaneRight ? U"Y" : U"N"));
			}
			y += 4;

			// -- Nodes --
			const RoadNode* nA = m_network.getNode(edge->nodeA);
			const RoadNode* nB = m_network.getNode(edge->nodeB);
			header(U"Nodes");
			if (nA) line(U"A #{}: {} {} att={}"_fmt(nA->id, ntNames[static_cast<int>(nA->type)],
				nA->transition == NodeTransition::Blend ? U"Blend" : U"Abrupt",
				nA->attachments.size()));
			if (nB) line(U"B #{}: {} {} att={}"_fmt(nB->id, ntNames[static_cast<int>(nB->type)],
				nB->transition == NodeTransition::Blend ? U"Blend" : U"Abrupt",
				nB->attachments.size()));
		}
		else
		{
			m_selectedEdgeId = none;
		}
	}

	// ---- ノード情報パネル ----
	if (m_selectedNodeId)
	{
		const RoadNode* node = m_network.getNode(*m_selectedNodeId);
		if (node)
		{
			static const Font pFont{ FontMethod::MSDF, 12 };
			static const Font pHeader{ FontMethod::MSDF, 14, Typeface::Bold };

			constexpr int kPW = 320;
			constexpr int kPad = 8;
			constexpr int kLH = 18;
			const int pH = Scene::Height() - 20;
			const int pX = Scene::Width() - kPW - 10;
			const int pY = 10;

			RectF{ static_cast<double>(pX), static_cast<double>(pY),
			       static_cast<double>(kPW), static_cast<double>(pH) }
				.draw(ColorF{ 0, 0, 0, 0.8 });

			int y = pY + kPad;
			auto line = [&](const String& text, ColorF color = Palette::White)
			{
				pFont(text).draw(Vec2{ pX + kPad, y }, color);
				y += kLH;
			};
			auto header = [&](const String& text)
			{
				pHeader(text).draw(Vec2{ pX + kPad, y }, Palette::Yellow);
				y += kLH + 4;
			};

			static constexpr StringView ntNames[] = { U"Endpoint", U"Joint", U"Intersection", U"Diverge" };
			static constexpr StringView rtNames[] = { U"LocalRoad", U"Arterial", U"Expressway", U"Highway" };

			// -- Node 基本情報 --
			header(U"RoadNode #{}"_fmt(node->id));
			line(U"type: {}"_fmt(ntNames[static_cast<int>(node->type)]));
			line(U"transition: {}"_fmt(node->transition == NodeTransition::Blend ? U"Blend" : U"Abrupt"));
			line(U"pos: ({:.0f}, {:.1f}, {:.0f})"_fmt(node->position.x, node->position.y, node->position.z));
			line(U"attachments: {}"_fmt(node->attachments.size()));
			y += 4;

			// -- Attachments --
			header(U"EdgeAttachments");
			for (size_t i = 0; i < node->attachments.size(); ++i)
			{
				const auto& att = node->attachments[i];
				const RoadEdge* e = m_network.getEdge(att.edgeId);
				String info = U"[{}] edge #{}"_fmt(i, att.edgeId);
				if (att.lateralOffset != 0.0f)
					info += U" lat={:.1f}"_fmt(att.lateralOffset);
				if (att.isThrough)
					info += U" [THROUGH]";
				line(info);

				if (e)
				{
					line(U"    {} {:.0f}km/h len={:.0f}m lanes={}"_fmt(
						rtNames[static_cast<int>(e->roadType)],
						e->speedLimit, e->length,
						e->lanes.size()), ColorF{ 0.7, 0.7, 0.7 });
				}
			}
		}
		else
		{
			m_selectedNodeId = none;
		}
	}

	lap(s_ui);
	s_total = swTotal.msF();

	m_debugRenderer.renderProfiler(s_total, m_logicMs, s_sky, s_terrain,
	                               s_road, s_zone, s_vehicle, s_train,
	                               s_debug, s_ui, m_network);

}

// ─────────────────────────────────────────────────────────────────────────────
// 入力処理
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::handleInput()
{
	if (KeyControl.pressed() && KeyS.down())
	{
		saveGame();
		return;
	}

	if (KeySpace.down())
	{
		if (m_clock.speed == TimeSpeed::Paused)
			m_clock.speed = m_prevSpeed;
		else
		{
			m_prevSpeed   = m_clock.speed;
			m_clock.speed = TimeSpeed::Paused;
		}
	}

	if (KeyTab.down())
		m_zoneManager.showOverlay = !m_zoneManager.showOverlay;

	if (KeyEscape.down())
	{
		if (m_mode != EditMode::None)
		{
			m_mode               = EditMode::None;
			m_drawStartNode      = none;
			m_rectStart          = none;
			m_trainDrawStartNode = none;
			m_sandboxDragNode    = none;
			m_sandboxDragCtrl    = none;
			m_editingRouteId     = -1;
			m_zoneManager.showOverlay = false;
		}
		m_selectedEdgeId = none;
		m_selectedNodeId = none;
	}

	if (KeyR.down())
	{
		m_mode = (m_mode == EditMode::RoadDraw) ? EditMode::None : EditMode::RoadDraw;
		m_drawStartNode = none;
		m_rectStart     = none;
	}
	if (KeyZ.down())
	{
		m_mode = (m_mode == EditMode::ZonePaint) ? EditMode::None : EditMode::ZonePaint;
		m_drawStartNode = none;
		m_rectStart     = none;
		m_zoneManager.showOverlay = (m_mode == EditMode::ZonePaint);
	}

	if (m_mode == EditMode::ZonePaint)
	{
		if (Key1.down()) m_paintZone = ZoneType::UrbanControl;
		if (Key2.down()) m_paintZone = ZoneType::LowResidential;
		if (Key3.down()) m_paintZone = ZoneType::Residential;
		if (Key4.down()) m_paintZone = ZoneType::Commercial;
		if (Key5.down()) m_paintZone = ZoneType::Industrial;
		if (Key6.down()) m_paintZone = ZoneType::Agriculture;
		if (Key0.down()) m_paintZone = ZoneType::Unzoned;
	}
	else
	{
		if (Key1.down()) { m_prevSpeed = TimeSpeed::x1; m_clock.speed = TimeSpeed::x1; }
		if (Key2.down()) { m_prevSpeed = TimeSpeed::x2; m_clock.speed = TimeSpeed::x2; }
		if (Key3.down()) { m_prevSpeed = TimeSpeed::x4; m_clock.speed = TimeSpeed::x4; }
		if (Key0.down()) { m_prevSpeed = m_clock.speed != TimeSpeed::Paused ? m_clock.speed : m_prevSpeed;
		                   m_clock.speed = TimeSpeed::Paused; }
	}

	if (KeyT.down())
		m_simThread.spawnVehicle();

	if (KeyF.down())
		m_camera.cycleMode();

	if (KeyG.down())
	{
		m_mode = (m_mode == EditMode::TerrainEdit) ? EditMode::None : EditMode::TerrainEdit;
		m_drawStartNode = none;
		m_rectStart     = none;
	}

	if (m_mode == EditMode::TerrainEdit && KeyControl.pressed())
	{
		const double wheel = Mouse::Wheel();
		m_terrainBrushRadius = Clamp(
			m_terrainBrushRadius + static_cast<float>(wheel * -20.0),
			20.0f, 400.0f);
	}

	if (KeyX.down())
	{
		m_mode = (m_mode == EditMode::TrainDraw) ? EditMode::None : EditMode::TrainDraw;
		m_trainDrawStartNode = none;
	}

	if (KeyB.down())
	{
		if (m_mode == EditMode::BusRouteDraw)
		{
			m_mode           = EditMode::None;
			m_editingRouteId = -1;
		}
		else
		{
			m_mode = EditMode::BusRouteDraw;
			BusRoute newRoute;
			newRoute.headwaySec  = 120.0f;
			m_editingRouteId = -1;
		}
	}

	if (m_sandboxActive && KeyV.down())
	{
		m_mode = (m_mode == EditMode::SandboxEdit) ? EditMode::None : EditMode::SandboxEdit;
		m_sandboxDragNode = none;
		m_drawStartNode   = none;
		m_rectStart       = none;
	}

	if (KeyN.down())
		m_showNameList = !m_showNameList;

	if      (m_mode == EditMode::RoadDraw)     handleRoadDraw();
	else if (m_mode == EditMode::ZonePaint)    handleZonePaint();
	else if (m_mode == EditMode::BusRouteDraw) handleBusRouteDraw();
	else if (m_mode == EditMode::TerrainEdit)  handleTerrainEdit();
	else if (m_mode == EditMode::TrainDraw)    handleTrainDraw();
	else if (m_mode == EditMode::SandboxEdit)  handleSandboxEdit();
	else if (m_mode == EditMode::None)
	{
		// 道路/ノード選択（左クリック、排他）
		if (MouseL.down() && m_cursorGroundPos)
		{
			const auto hitNode = m_network.findNodeNear(*m_cursorGroundPos, 20.0f);
			if (hitNode)
			{
				m_selectedNodeId = hitNode;
				m_selectedEdgeId = none;
			}
			else
			{
				const auto hitEdge = m_network.findEdgeNear(*m_cursorGroundPos, 15.0f);
				m_selectedEdgeId = hitEdge;
				m_selectedNodeId = none;
			}
		}
	}
}

void GameScene::handleRoadDraw()
{
	if (MouseL.down() && m_cursorGroundPos)
	{
		auto nearNode = m_network.findNodeNear(*m_cursorGroundPos, 20.0f);
		int nodeId;
		if (!nearNode)
			nodeId = m_network.addNode(*m_cursorGroundPos);
		else
			nodeId = *nearNode;

		if (!m_drawStartNode)
		{
			m_drawStartNode = nodeId;
		}
		else
		{
			int from = *m_drawStartNode;
			if (from != nodeId)
			{
				Vec3 pA  = m_network.getNode(from)->position;
				Vec3 pB  = m_network.getNode(nodeId)->position;
				Vec3 mid = (pA + pB) / 2.0;
				m_network.addEdgeWithIntersection(from, nodeId, mid, mid, RoadType::LocalRoad, 2);
				notifyNetworkChanged();
				m_roadRenderer.invalidateCachesAroundNode(from, m_network);
				m_roadRenderer.invalidateCachesAroundNode(nodeId, m_network);
			}
			m_drawStartNode = nodeId;
		}
	}
}

void GameScene::handleZonePaint()
{
	if (!m_cursorGroundPos) return;

	if (KeyShift.pressed())
	{
		if (MouseL.down())
			m_rectStart = m_cursorGroundPos;
		if (MouseL.up() && m_rectStart)
		{
			m_zoneManager.paintZoneRect(m_world, *m_rectStart, *m_cursorGroundPos, m_paintZone);
			m_rectStart = none;
		}
	}
	else
	{
		m_rectStart = none;
		if (MouseL.pressed())
			m_zoneManager.paintZone(m_world, *m_cursorGroundPos, m_paintZone, 2);
	}
}

void GameScene::handleBusRouteDraw()
{
	if (!m_cursorGroundPos) return;
	if (m_editingRouteId < 0) return;

	if (MouseL.down())
	{
		BusStop stop;
		stop.position = *m_cursorGroundPos;

		float bestDist = 30.0f;
		for (const auto& edge : m_network.edges())
		{
			if (edge.id < 0) continue;
			if (const auto bez = m_network.getBezier(edge.id))
			{
				const Vec3  mid = bez->positionAt(bez->totalLength * 0.5f);
				const float d   = static_cast<float>(stop.position.distanceFrom(mid));
				if (d < bestDist)
				{
					bestDist    = d;
					stop.edgeId = edge.id;
					stop.arcPos = edge.length * 0.5f;
				}
			}
		}
	}
}

void GameScene::handleTerrainEdit()
{
	if (!m_cursorGroundPos) return;

	const float dt    = static_cast<float>(Scene::DeltaTime());
	const float raise = MouseL.pressed() ? m_terrainBrushStrength * dt : 0.0f;
	const float lower = MouseR.pressed() ? m_terrainBrushStrength * dt : 0.0f;
	const float delta = raise - lower;
	if (delta == 0.0f) return;

	const Vec3 center = *m_cursorGroundPos;

	for (Chunk* chunk : m_world.getActiveChunks())
	{
		if (!chunk) continue;

		constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / HEIGHT_CELLS;
		const Vec3      origin   = chunk->worldOrigin();
		bool            modified = false;

		for (int row = 0; row <= HEIGHT_CELLS; ++row)
		{
			for (int col = 0; col <= HEIGHT_CELLS; ++col)
			{
				const Vec3   vpos = origin + Vec3{ col * cellSize, 0.0, row * cellSize };
				const double dist = Vec2{ vpos.x, vpos.z }.distanceFrom(Vec2{ center.x, center.z });
				if (dist > m_terrainBrushRadius) continue;

				const float t      = static_cast<float>(dist / m_terrainBrushRadius);
				const float weight = static_cast<float>(Math::Cos(t * Math::Pi / 2.0));
				chunk->heightMap[{ col, row }] = Clamp(
					chunk->heightMap[{ col, row }] + delta * weight,
					-200.0f, 350.0f);
				modified = true;
			}
		}

		if (modified)
		{
			chunk->meshDirty = true;
			chunk->updateHeightBounds();
		}
	}
}

void GameScene::handleTrainDraw()
{
	if (!m_cursorGroundPos) return;

	if (MouseL.down())
	{
		Optional<int> nearNode;
		float         bestDist = 20.0f;
		for (const auto& node : m_trainNetwork.nodes())
		{
			const float d = static_cast<float>(node.position.distanceFrom(*m_cursorGroundPos));
			if (d < bestDist)
			{
				bestDist = d;
				nearNode = node.id;
			}
		}

		int nodeId;
		if (!nearNode)
			nodeId = m_trainNetwork.addStation(*m_cursorGroundPos, U"駅");
		else
			nodeId = *nearNode;

		if (!m_trainDrawStartNode)
		{
			m_trainDrawStartNode = nodeId;
		}
		else
		{
			const int  from = *m_trainDrawStartNode;
			if (from != nodeId)
			{
				const Vec3 pa = m_trainNetwork.getNode(from)->position;
				const Vec3 pb = m_trainNetwork.getNode(nodeId)->position;
				m_trainNetwork.addEdge(from, nodeId,
					pa + (pb - pa) * (1.0 / 3),
					pa + (pb - pa) * (2.0 / 3));
			}
			m_trainDrawStartNode = nodeId;
		}
	}

	if (MouseR.down())
		m_trainDrawStartNode = none;
}

void GameScene::handleSandboxEdit()
{
	if (!m_cursorGroundPos) return;

	const Vec2 cur2D{ m_cursorGroundPos->x, m_cursorGroundPos->z };

	if (MouseL.down())
	{
		m_sandboxDragNode = none;
		m_sandboxDragCtrl = none;

		m_sandboxDragNode = m_network.findNodeNear(*m_cursorGroundPos, 20.0f);

		if (!m_sandboxDragNode)
		{
			float bestDist = 18.0f;
			for (const auto& edge : m_network.edges())
			{
				if (edge.id < 0) continue;
				const float dA = static_cast<float>(
					Vec2{ edge.ctrlA.x, edge.ctrlA.z }.distanceFrom(cur2D));
				const float dB = static_cast<float>(
					Vec2{ edge.ctrlB.x, edge.ctrlB.z }.distanceFrom(cur2D));
				if (dA < bestDist) { bestDist = dA; m_sandboxDragCtrl = CtrlDrag{ edge.id, true  }; }
				if (dB < bestDist) { bestDist = dB; m_sandboxDragCtrl = CtrlDrag{ edge.id, false }; }
			}
		}

		m_sandboxPrevCursor = *m_cursorGroundPos;
	}

	if (MouseL.up())
	{
		if (m_sandboxDragNode || m_sandboxDragCtrl)
			notifyNetworkChanged();
		m_sandboxDragNode = none;
		m_sandboxDragCtrl = none;
	}

	if (MouseL.pressed())
	{
		const Vec3 delta = *m_cursorGroundPos - m_sandboxPrevCursor;

		if (m_sandboxDragNode)
		{
			RoadNode* node = m_network.getNode(*m_sandboxDragNode);
			if (node)
			{
				for (const auto& att : node->attachments)
				{
					const int eid = att.edgeId;
					RoadEdge* edge = m_network.getEdge(eid);
					if (!edge) continue;
					if (edge->nodeA == node->id) edge->ctrlA += delta;
					if (edge->nodeB == node->id) edge->ctrlB += delta;
					m_roadRenderer.invalidateEdgeCache(eid, edge->nodeA, edge->nodeB);
				}
				node->position += delta;
			}
		}
		else if (m_sandboxDragCtrl)
		{
			RoadEdge* edge = m_network.getEdge(m_sandboxDragCtrl->edgeId);
			if (edge)
			{
				if (m_sandboxDragCtrl->isControlPointA) edge->ctrlA += delta;
				else                        edge->ctrlB += delta;
				m_roadRenderer.invalidateEdgeCache(edge->id, edge->nodeA, edge->nodeB);
			}
		}

		m_sandboxPrevCursor = *m_cursorGroundPos;
	}

	if (MouseR.down())
	{
		auto nearNode = m_network.findNodeNear(*m_cursorGroundPos, 20.0f);
		if (nearNode)
		{
			Array<int> neighborNodes;
			if (const RoadNode* node = m_network.getNode(*nearNode))
			{
				for (const auto& att : node->attachments)
				{
					const int eid = att.edgeId;
					m_roadRenderer.invalidateEdgeCache(eid);
					if (const RoadEdge* e = m_network.getEdge(eid))
					{
						const int other = (e->nodeA == *nearNode) ? e->nodeB : e->nodeA;
						neighborNodes << other;
					}
				}
			}
			m_network.removeNode(*nearNode);
			notifyNetworkChanged();
			for (const int nid : neighborNodes)
				m_roadRenderer.invalidateCachesAroundNode(nid, m_network);
		}
		else
		{
			int   bestId   = -1;
			float bestDist = 30.0f;
			for (const auto& edge : m_network.edges())
			{
				if (edge.id < 0) continue;
				const auto bez = m_network.getBezier(edge.id);
				if (!bez) continue;
				for (int k = 0; k <= 4; ++k)
				{
					const Vec3  pt   = bez->evaluate(k * 0.25f);
					const float dist = static_cast<float>(
						Vec2{ pt.x, pt.z }.distanceFrom(cur2D));
					if (dist < bestDist) { bestDist = dist; bestId = edge.id; }
				}
			}
			if (bestId >= 0)
			{
				int nA = -1, nB = -1;
				if (const RoadEdge* e = m_network.getEdge(bestId))
				{ nA = e->nodeA; nB = e->nodeB; }
				m_roadRenderer.invalidateEdgeCache(bestId, nA, nB);
				m_network.removeEdge(bestId);
				notifyNetworkChanged();
				if (nA >= 0) m_roadRenderer.invalidateCachesAroundNode(nA, m_network);
				if (nB >= 0) m_roadRenderer.invalidateCachesAroundNode(nB, m_network);
			}
		}
	}
}

void GameScene::updateCursor()
{
	const Ray    ray  = m_camera.screenToRay(Vec2{ Cursor::Pos() });
	const Float3 orig = ray.origin;
	const Float3 dir  = ray.direction;

	if (dir.y >= 0.0f)
	{
		m_cursorGroundPos = none;
		return;
	}

	constexpr float kStep    = 8.0f;
	constexpr float kMaxDist = 8000.0f;

	float tPrev    = 0.0f;
	bool  hitFound = false;

	for (float t = kStep; t < kMaxDist; t += kStep)
	{
		const float px = orig.x + dir.x * t;
		const float pz = orig.z + dir.z * t;
		const float py = orig.y + dir.y * t;
		const float th = m_world.sampleHeight(px, pz);

		if (py <= th)
		{
			float tLo = tPrev, tHi = t;
			for (int i = 0; i < 8; ++i)
			{
				const float tMid = (tLo + tHi) * 0.5f;
				const float mx   = orig.x + dir.x * tMid;
				const float mz   = orig.z + dir.z * tMid;
				const float my   = orig.y + dir.y * tMid;
				if (my <= m_world.sampleHeight(mx, mz))
					tHi = tMid;
				else
					tLo = tMid;
			}
			const float tf = (tLo + tHi) * 0.5f;
			const float fx = orig.x + dir.x * tf;
			const float fz = orig.z + dir.z * tf;
			m_cursorGroundPos = Vec3{ fx, static_cast<double>(m_world.sampleHeight(fx, fz)), fz };
			hitFound = true;
			break;
		}

		tPrev = t;
	}

	if (!hitFound)
		m_cursorGroundPos = m_camera.screenToGround(Vec2{ Cursor::Pos() });
}

String GameScene::modeString() const
{
	switch (m_mode)
	{
	case EditMode::RoadDraw:
		return U"道路描画モード（左クリックで配置）";
	case EditMode::ZonePaint:
		return U"ゾーン塗り [{}] 左:ブラシ Shift+左ドラッグ:矩形  0〜6:種別変更"_fmt(zoneName(m_paintZone));
	case EditMode::BusRouteDraw:
		return U"バス路線描画モード（左クリックでバス停配置・Bキーで確定）";
	case EditMode::TerrainEdit:
		return U"地形編集モード（左:盛土 右:掘削 Ctrl+ホイール:ブラシサイズ）";
	case EditMode::TrainDraw:
		return U"線路描画モード（左クリックで駅配置・連結 右クリックで中断）";
	case EditMode::SandboxEdit:
		return U"サンドボックス編集（左ドラッグ:ノード移動 右クリック:削除）";
	default:
		return U"";
	}
}
