#include "GameScene.hpp"
#include "../gen/WaterCrossings.hpp"
#include "../gen/RoadTerrainFit.hpp"
#include "../gen/RoadVerticalAlignment.hpp"
#include "../gen/StreetBlocks.hpp"
#include "../ui/PanelWidget.hpp"
#include "../road/JunctionGeometry.hpp"
#include "../gen/ParcelGeometry.hpp"
#include "../gen/UrbanParcel.hpp"
#include "../gen/ParcelRoadIndex.hpp"
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
	m_worldRenderer.setAsyncTerrain(!getData().syncTerrain);
	m_camera.setWalkSurface([this](Vec3 point)
	{
		const double ground=m_world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z));
		if (const auto hit=m_network.findEdgeNearDetailed(point,12))
		{
			const auto* edge=m_network.getEdge(hit->first); const auto curve=m_network.getBezier(hit->first);
			if (edge && edge->tunnel && curve)
			{
				const Vec3 pavement=curve->positionAt(hit->second);
				if (Abs(point.y-pavement.y)<4 && Vec2{point.x-pavement.x,point.z-pavement.z}.length()<edge->totalWidth()*.5) { return pavement.y+.035; }
			}
		}
		return ground;
	});
	m_renderTexture = MSRenderTexture{ Scene::Size(), TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes };
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
	m_panelManager.registerPanel(U"land_info",Vec2{320,190},true,true);
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
	m_drawTemplate = RoadPlanDraft::makeRoadTemplate(0);

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
		m_world.generateRivers();
		generateAllTerrain();
		placeAllSettlements();
		generateAllRoads();
		generateDistrictRoads();
		postProcessRoads();
		MapGenerator::setupTrain(m_trainNetwork,m_world,m_districts,&m_network);
		m_districtHierarchy.generate(m_world,m_districts);
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
		if (s.kind == MapGenerator::SettlementKind::RegionalCity)
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
		if (getData().auditRoadIntegrity) RoadBinary::writeGlobal(U"integrity_fixSharpAngles.bin",m_network);
		step.restart();
	}

	m_network.smoothAllCurves();
	Logger << U"[PostProcess] smoothAllCurves: {:.0f}ms"_fmt(step.msF());
	if (getData().auditRoadIntegrity) RoadBinary::writeGlobal(U"integrity_smoothAllCurves.bin",m_network);
	step.restart();

	DistrictRoads::straightenCastleTownRoads(m_districts, m_world, m_network);
	for (const auto& edge : m_network.edges())
	{
		if (edge.id >= 0) m_network.getEdge(edge.id)->edgeState = EdgeState::Open;
	}
	m_network.resolveIntersections();
	m_network.consolidateOverlappingRoads();
	m_network.resolveIntersections();
	m_network.consolidateOverlappingRoads();
	m_network.removeDuplicateEdges(getData().seed);
	Logger << U"[PostProcess] reconcileRoadGeometry: {:.0f}ms"_fmt(step.msF());
	if (getData().auditRoadIntegrity) RoadBinary::writeGlobal(U"integrity_reconciled.bin",m_network);
	if (getData().auditRoadIntegrity)
	{
		for (const auto& district : m_districts)
		{
			if (!district.plan.ready || district.plan.frontageRoads) { continue; }
			const Vec2 interior = district.plan.halfExtent-Vec2{80,80};
			int tested=0, misaligned=0;
			double largestDrift=0;
			for (const auto& edge : m_network.edges())
			{
				if (edge.id<0) { continue; }
				const Vec3 a=m_network.getNode(edge.nodeA)->position,b=m_network.getNode(edge.nodeB)->position;
				const Vec2 localA{a.x-district.center.x,a.z-district.center.y},localB{b.x-district.center.x,b.z-district.center.y};
				if (Abs(localA.dot(district.gridAxisX))>interior.x || Abs(localA.dot(district.gridAxisZ))>interior.y
					|| Abs(localB.dot(district.gridAxisX))>interior.x || Abs(localB.dot(district.gridAxisZ))>interior.y) { continue; }
				++tested;
				const Vec2 direction=localB-localA;
				const double drift=Min(Abs(direction.dot(district.gridAxisX)),Abs(direction.dot(district.gridAxisZ)));
				largestDrift=Max(largestDrift,drift);
				misaligned+=(drift>0.1);
			}
			DBG_LOG(U"[TownGridIntegrity] center=({}, {}) edges={} misaligned={} maxDriftM={}"_fmt(district.center.x,district.center.y,tested,misaligned,largestDrift));
		}
	}
	step.restart();

	RoadTerrainFit::apply(m_network,m_world);
	RoadVerticalAlignment::apply(m_network,m_world);
	const auto water=WaterCrossings::repair(m_network,[&](double x,double z) { return m_world.sampleHeight(static_cast<float>(x),static_cast<float>(z)); },[&](double x,double z) { return m_world.waterSurfaceHeight(x,z); });
	for (const auto& edge : m_network.edges())
	{
		if (edge.id>=0 && edge.useElevation) { m_network.generatePiersForEdge(edge.id,m_world); }
	}
	DBG_LOG(U"[WaterCrossings] wetBefore={} elevatedEdges={}"_fmt(water.wetEdges,water.elevatedEdges));
	if (getData().auditRoadIntegrity)
	{
		int wetRoads=0;
		for (const auto& edge : m_network.edges())
		{
			if (edge.id<0) { continue; } const auto curve=m_network.getBezier(edge.id);bool wet=false;
			for (float arc=0;arc<=curve->totalLength;arc+=4)
			{
				const Vec3 point=curve->positionAt(arc),right=tangentToRight(curve->tangentAt(arc));
				for (const int side : {-1,0,1})
				{
					const Vec3 p=point+right*(edge.totalWidth()*.5*side);const double ground=m_world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z)),surface=m_world.waterSurfaceHeight(p.x,p.z);
					const double pavement=edge.useElevation ? point.y : ground;
					wet|=ground<surface && pavement<surface-.03 && !(edge.tunnel && pavement<ground-6);
				}
			}
			wetRoads+=wet;
		}
		DBG_LOG(U"[RoadWaterAudit] submergedRoads={}"_fmt(wetRoads));
	}

	{
		int elevated=0,low=0,buried=0;
		for (const auto& edge : m_network.edges())
		{
			if (edge.id<0 || !edge.useElevation) { continue; }
			++elevated; double maximum=-1e9,minimum=1e9;
			const auto curve=m_network.getBezier(edge.id);
			for (int i=0;i<=20;++i) { const auto point=curve->evaluate(i/20.0f); const double gap=point.y-m_world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z)); maximum=Max(maximum,gap); minimum=Min(minimum,gap); }
			low+=maximum<2; buried+=minimum<-.05;
		}
		DBG_LOG(U"[RoadGroundAudit] elevated={} belowTwoMeters={} buried={}"_fmt(elevated,low,buried));
	}
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
		case MapGenerator::SettlementKind::RegionalCity:   return 0;
		case MapGenerator::SettlementKind::LocalTown: return 1;
		case MapGenerator::SettlementKind::RuralSettlement:   return 2;
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
			if (n.id < 0 || n.attachments.isEmpty()) { continue; }
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
				DBG_LOG(U"[Loading] Exception: {}"_fmt(m_loadingError));
				if (getData().auditRoadIntegrity) System::Exit();
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
				m_riverRenderer.build(m_world);
			m_tunnelRenderer.build(m_world,m_network,m_trainNetwork);
			m_worldRenderer.setTunnelOpenings(m_tunnelRenderer.openings);
			m_minimapRenderer.setGeography(m_world,m_districtHierarchy);
			m_minimapRenderer.buildTerrainTexture(m_world);
				m_minimapRenderer.updateRoadOverlay(m_network, m_world);
				m_worldRenderer.preloadBuildingModels();
			m_housingCapacity.update(m_world,WORLD_CHUNKS*WORLD_CHUNKS);
			m_phase = GamePhase::Playing;
				return;
			}

			Logger << U"[Loading] 全パイプライン完了 ({:.1f}秒)"_fmt(m_loadingTimer.sF());
			setLoadingStatus(U"初期化中...");

			m_roadRenderer.invalidateAllCaches();

			Vec3 cameraFocus{WORLD_SIZE*.5,0,WORLD_SIZE*.5}; double bestArea=0;
			for (const auto& town : m_districts)
			{
				const double area=town.plan.halfExtent.x*town.plan.halfExtent.y;
				if (!town.plan.frontageRoads && town.plan.scale==0 && area>bestArea)
				{
					bestArea=area; cameraFocus={town.center.x,m_world.sampleHeight(static_cast<float>(town.center.x),static_cast<float>(town.center.y)),town.center.y};
				}
			}
			m_camera.setFocus(cameraFocus);
			m_world.update(m_camera.focusPoint());

			startSimThread();

			m_riverRenderer.build(m_world);
			m_tunnelRenderer.build(m_world,m_network,m_trainNetwork);
			m_worldRenderer.setTunnelOpenings(m_tunnelRenderer.openings);
			m_minimapRenderer.setGeography(m_world,m_districtHierarchy);
			m_minimapRenderer.buildTerrainTexture(m_world);
			m_minimapRenderer.updateRoadOverlay(m_network, m_world);

			Logger << U"[Phase] 全体 {:.1f}秒 → Playing へ遷移"_fmt(m_loadingTimer.sF());
			m_worldRenderer.preloadBuildingModels();
			m_housingCapacity.update(m_world,WORLD_CHUNKS*WORLD_CHUNKS);
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
	meta[U"roadGeometryVersion"] = 1;
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
	if (!m_clearanceLedger.save(globalDirectory + U"/construction_clearance.json"))
	{
		return SaveResult::failed(SaveError::WriteFailed,U"撤去した区画を保存できません",globalDirectory);
	}
	if (!GuideSignStorage::writeJson(globalDirectory + U"/guide_signs.json", m_network))
	{
		return SaveResult::failed(SaveError::WriteFailed, U"guide_signs.json を保存できません", saveRoot);
	}

	JSON districts;
	districts[U"count"] = static_cast<int>(m_districts.size());
	districts[U"morphologyVersion"] = 1;
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
		const auto& plan=settlement.plan;
		districts[U"origin_{}"_fmt(index)]=static_cast<int>(plan.origin);
		districts[U"ruralForm_{}"_fmt(index)]=static_cast<int>(plan.ruralForm);
		districts[U"halfX_{}"_fmt(index)]=plan.halfExtent.x;
		districts[U"halfZ_{}"_fmt(index)]=plan.halfExtent.y;
		districts[U"axisX_{}"_fmt(index)]=settlement.gridAxisX.x;
		districts[U"axisZ_{}"_fmt(index)]=settlement.gridAxisX.y;
		districts[U"railway_{}"_fmt(index)]=plan.station.has_value();
		districts[U"frontage_{}"_fmt(index)]=plan.frontageRoads;
		districts[U"oldX_{}"_fmt(index)]=plan.oldCore.x;
		districts[U"oldZ_{}"_fmt(index)]=plan.oldCore.y;
		if (plan.station)
		{
			districts[U"stationX_{}"_fmt(index)]=plan.station->x;
			districts[U"stationZ_{}"_fmt(index)]=plan.station->y;
		}
		districts[U"civic_{}"_fmt(index)]=plan.civic.has_value();
		const RectF civic=plan.civic.value_or(RectF{0,0,0,0});
		districts[U"civicX_{}"_fmt(index)]=civic.x; districts[U"civicZ_{}"_fmt(index)]=civic.y;
		districts[U"civicW_{}"_fmt(index)]=civic.w; districts[U"civicH_{}"_fmt(index)]=civic.h;
		districts[U"industryX_{}"_fmt(index)]=plan.industry.x; districts[U"industryZ_{}"_fmt(index)]=plan.industry.y;
		districts[U"industryW_{}"_fmt(index)]=plan.industry.w; districts[U"industryH_{}"_fmt(index)]=plan.industry.h;
		districts[U"homes_{}"_fmt(index)]=static_cast<int>(plan.ruralHomes.size());
		for (size_t home=0;home<plan.ruralHomes.size();++home)
		{
			districts[U"homeX_{}_{}"_fmt(index,home)]=plan.ruralHomes[home].x;
			districts[U"homeZ_{}_{}"_fmt(index,home)]=plan.ruralHomes[home].y;
		}
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
	const FilePath clearancePath = saveRoot + U"/global/construction_clearance.json";
	if (FileSystem::IsFile(clearancePath))
	{
		RoadConstruction::ClearanceLedger verification;
		if (!verification.load(clearancePath))
			return SaveResult::failed(SaveError::CorruptData,U"撤去区画の記録が破損しています",clearancePath);
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
	m_world.generateRivers();
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
				s.radius = (s.kind == MapGenerator::SettlementKind::RegionalCity)   ? 700.0f
				         : (s.kind == MapGenerator::SettlementKind::LocalTown) ? 300.0f
				                                                             : 150.0f;
			}
			const String scoreKey = U"score_{}"_fmt(i);
			if (dist.hasElement(scoreKey))
				s.score = dist[scoreKey].get<float>();
			const String readingKey = U"reading_{}"_fmt(i);
			if (dist.hasElement(readingKey))
				s.reading = dist[readingKey].get<String>();
			const auto origin=static_cast<UrbanMorphology::Origin>(dist[U"origin_{}"_fmt(i)].getOr<int>(7));
			UrbanMorphology::Site site;
			const double halfZ=dist[U"halfZ_{}"_fmt(i)].getOr<double>(170);
			site.shoreDistance=halfZ+45;
			s.plan=UrbanMorphology::makePlan(origin,static_cast<uint8>(s.kind),site,
				UrbanMorphology::mix(getData().seed ^ (static_cast<uint64>(i)*0x9e3779b97f4a7c15ULL)),dist[U"railway_{}"_fmt(i)].getOr<bool>(false));
			s.plan.ruralForm=static_cast<UrbanMorphology::RuralForm>(dist[U"ruralForm_{}"_fmt(i)].getOr<int>(0));
			s.plan.halfExtent={dist[U"halfX_{}"_fmt(i)].getOr<double>(230),halfZ};
			s.plan.frontageRoads=dist[U"frontage_{}"_fmt(i)].getOr<bool>(false);
			s.plan.oldCore={dist[U"oldX_{}"_fmt(i)].getOr<double>(0),dist[U"oldZ_{}"_fmt(i)].getOr<double>(0)};
			if (s.plan.station) { s.plan.station=Vec2{dist[U"stationX_{}"_fmt(i)].getOr<double>(0),dist[U"stationZ_{}"_fmt(i)].getOr<double>(0)}; }
			if (dist[U"civic_{}"_fmt(i)].getOr<bool>(false))
			{
				s.plan.civic=RectF{dist[U"civicX_{}"_fmt(i)].get<double>(),dist[U"civicZ_{}"_fmt(i)].get<double>(),dist[U"civicW_{}"_fmt(i)].get<double>(),dist[U"civicH_{}"_fmt(i)].get<double>()};
			}
			s.plan.industry=RectF{dist[U"industryX_{}"_fmt(i)].getOr<double>(0),dist[U"industryZ_{}"_fmt(i)].getOr<double>(0),dist[U"industryW_{}"_fmt(i)].getOr<double>(0),dist[U"industryH_{}"_fmt(i)].getOr<double>(0)};
			for (int home=0;home<dist[U"homes_{}"_fmt(i)].getOr<int>(0);++home)
			{
				s.plan.ruralHomes << Vec2{dist[U"homeX_{}_{}"_fmt(i,home)].get<double>(),dist[U"homeZ_{}_{}"_fmt(i,home)].get<double>()};
			}
			s.gridAxisX={dist[U"axisX_{}"_fmt(i)].getOr<double>(1),dist[U"axisZ_{}"_fmt(i)].getOr<double>(0)};
			s.gridAxisZ={-s.gridAxisX.y,s.gridAxisX.x};
			settlements << s;
		}
		addDistricts(settlements);
	}

	// Upgrade only the loaded in-memory graph; saving remains an explicit user operation.
	if (!meta.hasElement(U"roadGeometryVersion") || meta[U"roadGeometryVersion"].getOr<int>(0) < 1)
	{
		setLoadingStatus(U"重複する道路の接続を修復中...");
		m_network.consolidateOverlappingRoads();
	}
	m_genProgress.store(0.9f);
	setLoadingStatus(U"ゾーン・建物を復元中...");

	MapGenerator::setupTrain(m_trainNetwork,m_world,m_districts,&m_network);
	m_districtHierarchy.generate(m_world,m_districts);
	applyZonesGlobal();
	placeInitialBuildings();
	const FilePath clearancePath = saveRoot + U"/global/construction_clearance.json";
	if (FileSystem::IsFile(clearancePath) && m_clearanceLedger.load(clearancePath))
		m_clearanceLedger.apply(m_world);
	m_restoreConstructionSites = true;
	registerGuideDestinations();

	m_roadRenderer.invalidateAllCaches();

	m_clock.now   = gameNow;
	m_clock.speed = static_cast<TimeSpeed>(timeScale);
	m_clock.syncCalendar();

	m_camera.setState(Vec3{ focusX, focusY, focusZ }, camDist, camYaw, camPitch);


	m_world.update(m_camera.focusPoint());
	startSimThread();

	Console << U"[Load] finish: {:.0f}ms"_fmt(step.msF());
	m_genProgress.store(1.0f);
	generateLandPatches(true);
	m_clearanceLedger.apply(m_world);
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
		if (s.kind == MapGenerator::SettlementKind::RegionalCity)
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

	Vec2 planLocal(const MapGenerator::Settlement& settlement, const Vec2& position)
	{
		const Vec2 delta=position-settlement.center;
		return {delta.dot(settlement.gridAxisX),delta.dot(settlement.gridAxisZ)};
	}

	ZoneType pickInitialZone([[maybe_unused]] uint64 seed, [[maybe_unused]] int settlementIndex,
		const MapGenerator::Settlement& settlement, const Chunk& chunk, Point chunkCoord,
		[[maybe_unused]] int globalGX, [[maybe_unused]] int globalGZ, float wx, float wz)
	{
		const float height=sampleHeightMap(chunk.heightMap,chunkCoord,wx,wz);
		const float slope=localSlope(chunk,chunkCoord,wx,wz);
		if (height<1.0f || slope>0.12f) { return ZoneType::Unzoned; }
		const Vec2 local=planLocal(settlement,{wx,wz});
		const auto use=UrbanMorphology::sample(settlement.plan,local);
		switch (use.district)
		{
		case UrbanMorphology::District::Civic: return ZoneType::UrbanControl;
		case UrbanMorphology::District::Industry: return ZoneType::Industrial;
		case UrbanMorphology::District::OldTown:
		case UrbanMorphology::District::Station: return ZoneType::Commercial;
		case UrbanMorphology::District::PlannedHousing: return ZoneType::Residential;
		case UrbanMorphology::District::Housing:
			return settlement.plan.scale==2 ? ZoneType::LowResidential : ZoneType::Residential;
		default:
			return slope<0.06f && UrbanMorphology::contains(settlement.plan,local,450)
				? ZoneType::Agriculture : ZoneType::Unzoned;
		}
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
			const float radius = static_cast<float>(Max(1.0,s.plan.halfExtent.length()));
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
	Array<Vec2> makeCoastalBandPolygon(Point chunk, int minX, int shoreY, int width, int depth, int landwardSign, uint64 salt)
	{
		const int clampedDepth = Max(1, depth);
		const int minY = (landwardSign >= 0) ? shoreY : Max(0, shoreY - clampedDepth + 1);
		const int maxY = (landwardSign >= 0) ? Min(ZONE_CELLS, shoreY + clampedDepth) : Min(ZONE_CELLS, shoreY + 1);
		const Array<Vec2> base = makeCellPatchPolygon(chunk, minX, minY, width, Max(1, maxY - minY), salt);
		if (base.size() < 8) return base;
		const float cellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
		const float wave = cellSize * (0.20f + static_cast<float>((salt >> 11) & 7u) * 0.018f);
		auto jitter = [&](int shift)
		{
			return (static_cast<float>((salt >> shift) & 255u) / 255.0f - 0.5f) * wave;
		};
		Array<Vec2> result = base;
		const float shoreZ = static_cast<float>(chunk.y * ZONE_CELLS + shoreY + ((landwardSign >= 0) ? 0 : 1)) * cellSize;
		result[0].y = shoreZ + jitter(3);
		result[1].y = shoreZ + jitter(17);
		result[2].y = shoreZ + jitter(29);
		return result;
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
		if (kind == MapGenerator::SettlementKind::RegionalCity)
		{
			if (zone == ZoneType::Commercial) return 0.48f;
			if (zone == ZoneType::Residential) return 0.24f;
			if (zone == ZoneType::LowResidential) return 0.34f;
			if (zone == ZoneType::Industrial) return 0.24f;
		}
		if (kind == MapGenerator::SettlementKind::LocalTown)
		{
			if (zone == ZoneType::Commercial) return 0.38f;
			if (zone == ZoneType::Residential) return 0.30f;
			if (zone == ZoneType::LowResidential) return 0.24f;
			if (zone == ZoneType::Industrial) return 0.20f;
		}
		if (zone == ZoneType::Residential) return 0.24f;
		if (zone == ZoneType::LowResidential) return 0.22f;
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
		const float outerDist=static_cast<float>(settlement.plan.halfExtent.length()+500);

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
			if (kind == MapGenerator::SettlementKind::RegionalCity)
			{
				if (zone == ZoneType::Commercial) return 0.94f;
				if (zone == ZoneType::Residential) return 0.96f;
				if (zone == ZoneType::LowResidential) return 0.88f;
				if (zone == ZoneType::Industrial) return 0.72f;
				if (zone == ZoneType::Agriculture) return 0.20f;
			}
			else if (kind == MapGenerator::SettlementKind::LocalTown)
			{
				if (zone == ZoneType::Commercial) return 0.86f;
				if (zone == ZoneType::Residential) return 0.76f;
				if (zone == ZoneType::LowResidential) return 0.34f;
				if (zone == ZoneType::Industrial) return 0.72f;
				if (zone == ZoneType::Agriculture) return 0.32f;
			}
			else
			{
				if (zone == ZoneType::Residential) return 0.86f;
				if (zone == ZoneType::LowResidential) return 0.56f;
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
				else building.type = (roll < (kind == MapGenerator::SettlementKind::RuralSettlement ? 94u : 82u))
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
		void applySettlementContext(Building& building, const MapGenerator::Settlement& settlement,
			const Vec2& position, uint32 hash)
		{
			const auto use=UrbanMorphology::sample(settlement.plan,planLocal(settlement,position));
			const uint32 roll=(hash/17u)%100u;
			const Vec2 local=planLocal(settlement,position);
			if (settlement.plan.station && Abs(local.x-settlement.plan.station->x)<32 && Abs(local.y-settlement.plan.station->y)<15)
			{
				building.type=BuildingType::None; return;
			}
			switch (use.district)
			{
			case UrbanMorphology::District::Civic: building.type=BuildingType::None; break;
			case UrbanMorphology::District::Industry:
				building.type=roll<92u ? BuildingType::Factory : BuildingType::Parking; break;
			case UrbanMorphology::District::Station:
				building.type=roll<38u ? BuildingType::Office : (roll<66u ? BuildingType::MidApartment
					: (roll<78u && settlement.plan.scale==0 ? BuildingType::HighApartment : BuildingType::Shop)); break;
			case UrbanMorphology::District::OldTown:
				building.type=roll<78u ? BuildingType::Shop : (roll<91u ? BuildingType::MidApartment : BuildingType::LowApartment); break;
			case UrbanMorphology::District::PlannedHousing:
				building.type=roll<62u ? BuildingType::MidApartment : (roll<82u ? BuildingType::LowApartment
					: (roll<96u ? BuildingType::Detached : BuildingType::PublicFacility)); break;
			case UrbanMorphology::District::Housing:
				building.type=roll<(settlement.plan.scale==2 ? 94u : (settlement.plan.scale==1 ? 82u : 58u)) ? BuildingType::Detached
					: (roll<84u ? BuildingType::LowApartment : BuildingType::MidApartment); break;
			default: building.type=BuildingType::None; break;
			}
		}

		BuildingType ruralFringeBuildingType(MapGenerator::SettlementKind kind, float distFromCenter, float settlementRadius, uint32 hash)
		{
			const float fringeStart = settlementRadius * (kind == MapGenerator::SettlementKind::RuralSettlement ? 0.92f : 1.08f);
			const float fringeEnd = settlementRadius * (kind == MapGenerator::SettlementKind::RuralSettlement ? 2.80f : 2.15f);
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
		Vec2 roadPosition{ 0, 0 };
	};
	Vec2 roadsideBuildingOffset(const EdgeFacingSlot& slot, BuildingType type, int gx, int gz)
	{
		constexpr float kCellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
		const Vec2 cellCenter{
			slot.chunkCoord.x * CHUNK_SIZE + (slot.col + 0.5f) * kCellSize,
			slot.chunkCoord.y * CHUNK_SIZE + (slot.row + 0.5f) * kCellSize };
		const float targetDistance = slot.halfWidth + buildingFootprintXZ(type) * 0.5f
			+ Max(1.2f, setbackFromRoadByModel(type, gx, gz));
		return slot.roadPosition + slot.roadToCellDir * targetDistance - cellCenter;
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

	bool hasRoadClearance(
		const RoadNetwork& network,
		int edgeId,
		const Vec2& position,
		float halfBuilding,
		float minimumSetback)
	{
		EdgeProjection projection;
		if (!projectPointToEdgeXZ(network, edgeId, position, projection))
		{
			return false;
		}
		const RoadEdge* edge = network.getEdge(edgeId);
		if (!edge || !edge->isRoadbedBuilt())
		{
			return false;
		}
		const RoadGeometry::LateralRange range = RoadGeometry::structuralRangeAt(*edge, projection.edgeT);
		if (!range.valid)
		{
			return false;
		}
		const Vec2 toPosition = position - projection.position;
		const float signedLateral = static_cast<float>(toPosition.dot(projection.right));
		const float roadOuter = (signedLateral >= 0.0f) ? Max(0.0f, range.right) : Max(0.0f, -range.left);
		return Math::Abs(signedLateral) >= roadOuter + halfBuilding + minimumSetback;
	}
	bool overlapsExistingBuilding(
		const World& world,
		Point cc,
		int gx,
		int gz,
		float wx,
		float wz,
		float halfBuilding,
		float angle)
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

				const Vec2 ncenter = cellCenterXZ(ncc, nx, nz) + Vec2{ nb.offsetX, nb.offsetZ };
				const float nHalf = buildingFootprintXZ(nb.type) * 0.5f + 0.25f;
				if (ParcelGeometry::overlaps(ParcelGeometry::footprint(Vec2{ wx, wz }, halfBuilding + 0.25f, angle),
					ParcelGeometry::footprint(ncenter, nHalf, nb.angle)))
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

		HashTable<int64, EdgeFacingSlot> bestByCell;

		for (const auto& edge : network.edges())
		{
			if (edge.id < 0 || !edge.isRoadbedBuilt()) continue;
			if (edge.roadType != RoadType::LocalRoad && edge.roadType != RoadType::Arterial) continue;

			const auto bez = network.getBezier(edge.id);
			if (!bez || bez->totalLength <= 1.0f) continue;
			const float edgeHalfWidth = edge.totalWidth() * 0.5f;
			const Vec3 middle=bez->positionAt(bez->totalLength*0.5f);
			const float frontagePitch=Min(settlement.plan.scale<2 ? 16.0f : 40.0f,static_cast<float>(UrbanMorphology::sample(settlement.plan,planLocal(settlement,{middle.x,middle.z})).frontage));
			constexpr float kCornerSetback = 6.5f;
			const float startArc = edge.cutoffA + kCornerSetback;
			const float endArc = bez->totalLength - edge.cutoffB - kCornerSetback;
			if (endArc < startArc) { continue; }
			const int sampleCount = Max(1, static_cast<int>(Floor((endArc - startArc) / frontagePitch)) + 1);

			for (int i = 0; i < sampleCount; ++i)
			{
				const float t = sampleCount == 1 ? 0.5f : static_cast<float>(i) / (sampleCount - 1);
				const float arc = Math::Lerp(startArc, endArc, t);
				const Vec3 pos = bez->positionAt(arc);
				const Vec3 tan = bez->tangentAt(arc);
				Vec2 tangent{ static_cast<float>(tan.x), static_cast<float>(tan.z) };
				if (tangent.lengthSq() <= 1e-8f) continue;
				tangent.normalize();
				const Vec2 right{ tangent.y, -tangent.x };


				if (!UrbanMorphology::contains(settlement.plan,planLocal(settlement,{pos.x,pos.z}),45)) { continue; }

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
					if (!projectPointToEdgeXZ(network, edge.id, slotPos, projection)) continue;

					const Vec2 toCell = slotPos - projection.position;
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
					slot.roadPosition = projection.position;

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
	int placed = 0;
	int rejectedRoad = 0;
	int rejectedSlope = 0;
	int rejectedDensity = 0, rejectedClearance = 0, rejectedNeighbor = 0, candidateSlots = 0;
	ParcelRoadIndex roadIndex{ m_network,true };
	roadIndex.addRailway(m_trainNetwork);
	auto isBuildableFootprint = [&](const Building& building, Vec2 center, float maximumRelief=1.2f)
	{
		const auto footprint = ParcelGeometry::footprint(center, buildingFootprintXZ(building.type) * 0.5 + 0.35, building.angle);
		if (roadIndex.overlaps(footprint))
		{
			++rejectedRoad;
			return false;
		}
		float minHeight = Math::Inf, maxHeight = -Math::Inf;
		for (const Vec2& corner : footprint)
		{
			const float height = m_world.sampleHeight(static_cast<float>(corner.x), static_cast<float>(corner.y));
			if (height<m_world.waterSurfaceHeight(corner.x,corner.y)+2.6) { ++rejectedSlope; return false; }
			minHeight = Min(minHeight, height);
			maxHeight = Max(maxHeight, height);
		}
		constexpr float kCoastalBuildHeight = 2.6f;
		if (minHeight < kCoastalBuildHeight || maxHeight - minHeight > maximumRelief)
		{
			++rejectedSlope;
			return false;
		}
		return true;
	};
	HashTable<int64, EdgeFacingSlot> edgeFacingSlotsByCell;

	// 道路沿いスロットを地区ごとに収集し、ゾーン・道路距離・重なり判定を満たす場所へ初期建物を置く。
	for (int si = 0; si < static_cast<int>(m_districts.size()); ++si)
	{
		const auto& s = m_districts[si];


		const float radiusSq=static_cast<float>(Square(s.plan.halfExtent.length()+65));
		const Array<EdgeFacingSlot> slots = collectEdgeFacingSlots(s, m_world, m_network);
		candidateSlots += static_cast<int>(slots.size());
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
			InitialBuilding::applySettlementContext(b,s,centerPos,cellHash);

			if (b.type == BuildingType::None) continue;

			float roadScore;
			if      (slot.roadDist < kNearDist) roadScore = 1.0f;
			else if (slot.roadDist < kFarDist)  roadScore = 1.0f - (slot.roadDist - kNearDist) / (kFarDist - kNearDist);
			else                                roadScore = 0.0f;

			const float densityFactor=static_cast<float>(UrbanMorphology::sample(s.plan,planLocal(s,centerPos)).occupancy);
			const float score = roadScore * densityFactor;
			if (score < 0.03f) continue;

			const float roll = (cellHash % 1000) / 1000.0f;
			if (roll > score) { ++rejectedDensity; continue; }

			const Vec2 roadOffset = roadsideBuildingOffset(slot, b.type, globalGX, globalGZ);
			b.offsetX = static_cast<float>(roadOffset.x);
			b.offsetZ = static_cast<float>(roadOffset.y);
			b.angle = slot.angle;
			b.edgeId = slot.edgeId;
			b.edgeT = slot.edgeT;

			const Vec2 finalPos = centerPos + roadOffset;
			if (b.type != BuildingType::Farmland)
			{
				if (!isBuildableFootprint(b, finalPos))
				{
					continue;
				}
				const float minimumSetback = Max(1.05f, setbackFromRoadByModel(b.type, globalGX, globalGZ) * 0.30f);
				if (!hasRoadClearance(m_network, slot.edgeId, finalPos, buildingFootprintXZ(b.type) * 0.5f, minimumSetback)) { ++rejectedClearance; continue; }
				if (overlapsExistingBuilding(
					m_world, slot.chunkCoord, slot.col, slot.row,
					static_cast<float>(finalPos.x), static_cast<float>(finalPos.y), buildingFootprintXZ(b.type) * 0.5f, b.angle)) { ++rejectedNeighbor; continue; }
			}

			chunk->buildingGrid[{ slot.col, slot.row }] = b;
			chunk->meshDirty = true;
			++placed;
		}
	}

	int midrise = 0, towers = 0, offices = 0, detached = 0;
	for (int z = 0; z < WORLD_CHUNKS; ++z)
	{
		for (int x = 0; x < WORLD_CHUNKS; ++x)
		{
			if (const Chunk* chunk = m_world.getChunk(Point{x,z}))
			{
				for (const auto& building : chunk->buildingGrid)
				{
					midrise += building.type == BuildingType::MidApartment;
					towers += building.type == BuildingType::HighApartment;
					offices += building.type == BuildingType::Office;
					detached += building.type == BuildingType::Detached;
				}
			}
		}
	}
	DebugLog::print(U"[UrbanMix] midrise={} towers={} offices={} detached={}"_fmt(midrise,towers,offices,detached));
	DebugLog::print(U"[PlacementReview] slots={} placed={} density={} clearance={} neighbor={}"_fmt(candidateSlots, placed, rejectedDensity, rejectedClearance, rejectedNeighbor));
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
					InitialBuilding::applySettlementContext(b,settlement,centerPos,cellHash);
					if (b.type == BuildingType::None || b.type == BuildingType::Farmland) continue;

					const int64 roadSlotKey = zoneCellKey(Point{ chunkX, chunkY }, col, row);
					if (const auto roadSlot = edgeFacingSlotsByCell.find(roadSlotKey); roadSlot != edgeFacingSlotsByCell.end())
					{
						const EdgeFacingSlot& slot = roadSlot->second;
						const float setbackM = setbackFromRoadByModel(b.type, globalGX, globalGZ);
						const Vec2 roadOffset = roadsideBuildingOffset(slot, b.type, globalGX, globalGZ);
						b.offsetX = static_cast<float>(roadOffset.x);
						b.offsetZ = static_cast<float>(roadOffset.y);
						b.angle = slot.angle;
						b.edgeId = slot.edgeId;
						b.edgeT = slot.edgeT;

						const Vec2 finalPos = centerPos + roadOffset;
						if (!isBuildableFootprint(b, finalPos))
						{
							continue;
						}
						const float minimumSetback = Max(1.05f, setbackM * 0.30f);
						if (!hasRoadClearance(m_network, slot.edgeId, finalPos, buildingFootprintXZ(b.type) * 0.5f, minimumSetback)) { ++rejectedClearance; continue; }
						if (overlapsExistingBuilding(
							m_world, Point{ chunkX, chunkY }, col, row,
							static_cast<float>(finalPos.x), static_cast<float>(finalPos.y), buildingFootprintXZ(b.type) * 0.5f, b.angle)) { ++rejectedNeighbor; continue; }
					}
					else
					{
						continue;
					}
					building = b;
					++infillPlaced;
					changed = true;
				}
			}
			if (changed) chunk->meshDirty = true;
		}
	}

	int blockCount=0,emptyBefore=0,blockInfill=0,emptyAfter=0;
	const auto blocks=StreetBlocks::collect(m_network);
	for (const auto& block : blocks)
	{
		const MapGenerator::Settlement* town=nullptr;
		for (const auto& candidate : m_districts)
		{
			if (!candidate.plan.ready || candidate.plan.frontageRoads) { continue; }
			bool within=true;
			for (const Vec2 point : block.outline)
			{
				const Vec2 local=planLocal(candidate,point);
				if (Abs(local.x)>candidate.plan.halfExtent.x+1 || Abs(local.y)>candidate.plan.halfExtent.y+1) { within=false; break; }
			}
			if (within) { town=&candidate; break; }
		}
		if (!town) { continue; }
		++blockCount; bool occupied=false;
		Point lowChunk,highChunk; int lowX,lowZ,highX,highZ;
		worldToZoneCell(static_cast<float>(block.bounds.x),static_cast<float>(block.bounds.y),lowChunk,lowX,lowZ);
		worldToZoneCell(static_cast<float>(block.bounds.x+block.bounds.w),static_cast<float>(block.bounds.y+block.bounds.h),highChunk,highX,highZ);
		for (int globalZ=lowChunk.y*ZONE_CELLS+lowZ;globalZ<=highChunk.y*ZONE_CELLS+highZ && !occupied;++globalZ)
		{
			for (int globalX=lowChunk.x*ZONE_CELLS+lowX;globalX<=highChunk.x*ZONE_CELLS+highX;++globalX)
			{
				const Point coord{globalX/ZONE_CELLS,globalZ/ZONE_CELLS}; const int col=globalX%ZONE_CELLS,row=globalZ%ZONE_CELLS;
				const Chunk* chunk=m_world.getChunk(coord); if (!chunk) { continue; }
				const auto& building=chunk->buildingGrid[{col,row}];
				if (building.type!=BuildingType::None && building.type!=BuildingType::Farmland && building.type!=BuildingType::Parking
					&& block.contains(cellCenterXZ(coord,col,row)+Vec2{building.offsetX,building.offsetZ})) { occupied=true; break; }
			}
		}
		if (occupied) { continue; }
		++emptyBefore; int tried=0,terrainFailures=0; const int roadBefore=rejectedRoad,slopeBefore=rejectedSlope;
		for (const int edgeId : block.edges)
		{
			if (occupied) { break; }
			const auto* edge=m_network.getEdge(edgeId); const auto curve=m_network.getBezier(edgeId);
			for (float arc=edge->cutoffA+9;arc<curve->totalLength-edge->cutoffB-9 && !occupied;arc+=6)
			{
				const float fraction=arc/curve->totalLength;
				const auto range=RoadGeometry::structuralRangeAt(*edge,fraction);
				const Vec3 point=curve->positionAt(arc),right=tangentToRight(curve->tangentAt(arc));
				for (const int side : {-1,1})
				{
					const Vec2 direction{right.x*side,right.z*side};
					const double outer=side<0 ? -range.left : range.right;
					const bool civic=town->plan.civic && town->plan.civic->contains(planLocal(*town,block.center));
					Building building=InitialBuilding::spawn(ZoneType::LowResidential,town->kind,0,static_cast<uint32>(edgeId));
					building.type=civic ? BuildingType::PublicFacility : BuildingType::Detached;
					const float half=buildingFootprintXZ(building.type)*.5f;
					const Vec2 position=Vec2{point.x,point.z}+direction*(outer+half+1.6);
					if (!block.contains(position)) { continue; }
					Point coord; int col,row; worldToZoneCell(static_cast<float>(position.x),static_cast<float>(position.y),coord,col,row);
					Chunk* chunk=m_world.getChunk(coord); if (!chunk || chunk->buildingGrid[{col,row}].type!=BuildingType::None) { continue; }
					const Vec2 cell=cellCenterXZ(coord,col,row);
					building.angle=static_cast<float>(std::atan2(-direction.x,direction.y)); building.edgeId=edgeId; building.edgeT=curve->tFromArcLength(arc);
					building.offsetX=static_cast<float>(position.x-cell.x); building.offsetZ=static_cast<float>(position.y-cell.y); ++tried;
					if (!isBuildableFootprint(building,position,2.5f)) { ++terrainFailures; continue; }
					if (overlapsExistingBuilding(m_world,coord,col,row,static_cast<float>(position.x),static_cast<float>(position.y),half,building.angle)) { continue; }
					chunk->buildingGrid[{col,row}]=building; chunk->zoneMap[{col,row}]=civic ? ZoneType::Residential : ZoneType::LowResidential; chunk->meshDirty=true;
					++blockInfill; occupied=true; break;
				}
			}
		}
		if (!occupied)
		{
			++emptyAfter;
			DBG_LOG(U"[EmptyBlock] center=({}, {}) area={} tried={} footprintRejected={} road={} slope={}"_fmt(block.center.x,block.center.y,block.area,tried,terrainFailures,rejectedRoad-roadBefore,rejectedSlope-slopeBefore));
		}
	}
	DBG_LOG(U"[BlockCoverage] blocks={} emptyBefore={} infill={} emptyAfter={}"_fmt(blockCount,emptyBefore,blockInfill,emptyAfter));
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
	DebugLog::print(U"[placeInitialBuildings] placed={} infill={} fields={} rejectedRoad={} rejectedSlope={} elapsedMs={:.1f}"_fmt(placed, infillPlaced, fieldCells, rejectedRoad, rejectedSlope, sw.msF()));
	refreshBuildingAnglesFromEdges();
	m_cityConstraintValidationPassed = validateGeneratedCityConstraints() && emptyAfter==0;
}

void GameScene::generateLandPatches(bool preserveExisting)
{
	auto isUrbanLandZone = [](ZoneType zone)
	{
		return zone == ZoneType::LowResidential || zone == ZoneType::Residential
			|| zone == ZoneType::Commercial || zone == ZoneType::Industrial;
	};

	auto parcelTypeFor = [](ZoneType zone, BuildingType buildingType, uint32 salt)
	{
		if (zone == ZoneType::Agriculture)
		{
			return ((salt >> 3) & 1u) ? LandPatchType::PaddyField : LandPatchType::FarmField;
		}
		if (buildingType == BuildingType::Office || buildingType == BuildingType::MidApartment || buildingType == BuildingType::HighApartment) { return LandPatchType::ParcelAsphalt; }
		if (buildingType == BuildingType::Parking || zone == ZoneType::Commercial || zone == ZoneType::Industrial)
		{
			return ((salt >> 5) & 1u) ? LandPatchType::ParcelAsphalt : LandPatchType::ParcelGravel;
		}
		return LandPatchType::GardenSoil;
	};

	for (int chunkY = 0; chunkY < WORLD_CHUNKS; ++chunkY)
	{
		for (int chunkX = 0; chunkX < WORLD_CHUNKS; ++chunkX)
		{
			Chunk* chunkPtr = m_world.getChunk(Point{ chunkX, chunkY });
			if (!chunkPtr) continue;
			Chunk& chunk = *chunkPtr;
			if (preserveExisting && !chunk.landPatches.isEmpty())
			{
				for (LandPatch& patch : chunk.landPatches) { UrbanParcel::normalize(patch.polygon); }
				continue;
			}
			chunk.landPatches.clear();
			const Point coord{ chunkX, chunkY };
			uint32 patchIndex = 0;
			Grid<bool> agriculturePatchCovered(ZONE_CELLS, ZONE_CELLS, false);
			for (int row = 0; row < ZONE_CELLS; ++row)
			{
				for (int col = 0; col < ZONE_CELLS; ++col)
				{
					const ZoneType zone = chunk.zoneMap[{ col, row }];
					const bool civicZone=(zone==ZoneType::UrbanControl);
					const bool agricultureZone = (zone == ZoneType::Agriculture || civicZone);
					if (!isUrbanLandZone(zone) && !agricultureZone) continue;
					if (agricultureZone && agriculturePatchCovered[{ col, row }]) continue;

					if (agricultureZone)
					{
						constexpr int kFieldColumns = 6, kFieldRows = 4;
						int width = 0;
						while (width < kFieldColumns && col + width < ZONE_CELLS
							&& chunk.zoneMap[{ col + width, row }] == zone
							&& !agriculturePatchCovered[{ col + width, row }]) { ++width; }
						int height = 1;
						for (; height < kFieldRows && row + height < ZONE_CELLS; ++height)
						{
							bool available = true;
							for (int x = 0; x < width; ++x)
							{
								available &= chunk.zoneMap[{ col + x, row + height }] == zone
									&& !agriculturePatchCovered[{ col + x, row + height }];
							}
							if (!available) { break; }
						}
						constexpr float kCellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
						const Vec2 corner{ (chunkX * ZONE_CELLS + col) * kCellSize + 0.7f,
							(chunkY * ZONE_CELLS + row) * kCellSize + 0.7f };
						const double sizeX = width * kCellSize - 1.4, sizeZ = height * kCellSize - 1.4;
						LandPatch field;
						field.id = static_cast<int>(patchIndex++);
						field.sourceParcelKey = -1;
						field.type = civicZone ? LandPatchType::GardenSoil : LandPatchType::FarmField;
						field.elevationOffset = 0.022f;
						field.materialVariant = settlementCellHash(getData().seed, 211, chunkX * ZONE_CELLS + col, chunkY * ZONE_CELLS + row);
						const float cornerHeight = m_world.sampleHeight(static_cast<float>(corner.x),static_cast<float>(corner.y));
						const float diagonalHeight = m_world.sampleHeight(static_cast<float>(corner.x+sizeX),static_cast<float>(corner.y+sizeZ));
						if (!civicZone && Abs(cornerHeight-diagonalHeight) < .8f && field.materialVariant%4u != 0u) { field.type = LandPatchType::PaddyField; }

						bool dry=true;
						for (double dz=0;dz<=sizeZ;dz+=8) for (double dx=0;dx<=sizeX;dx+=8)
						{
							const Vec2 point=corner+Vec2{dx,dz};
							dry &= m_world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.y))>m_world.waterSurfaceHeight(point.x,point.y)+1;
						}
						if (!dry) { continue; }
						field.polygon = { corner, corner + Vec2{ sizeX, 0 }, corner + Vec2{ sizeX, sizeZ }, corner + Vec2{ 0, sizeZ } };
						chunk.landPatches << field;
						for (int y = 0; y < height; ++y)
						{
							for (int x = 0; x < width; ++x) { agriculturePatchCovered[{ col + x, row + y }] = true; }
						}
						continue;
					}
					const Building& building = chunk.buildingGrid[{ col, row }];
					if (building.type == BuildingType::None || building.type == BuildingType::Farmland) { continue; }
					const int64 cellKey = zoneCellKey(coord, col, row);
					const uint32 salt = settlementCellHash(getData().seed ^ 0xA24BAED5u, 211,
						chunkX * ZONE_CELLS + col, chunkY * ZONE_CELLS + row);
					const Vec2 sampleCenter = cellCenterXZ(coord, col, row) + Vec2{ building.offsetX, building.offsetZ };
					EdgeProjection projection;
					if (!projectPointToEdgeXZ(m_network, building.edgeId, sampleCenter, projection)) { continue; }
					const RoadEdge* edge = m_network.getEdge(building.edgeId);
					if (!edge || !edge->isRoadbedBuilt()) { continue; }
					const RoadGeometry::LateralRange range = RoadGeometry::structuralRangeAt(*edge, projection.edgeT);
					if (!range.valid) { continue; }
					const bool rightSide = (sampleCenter - projection.position).dot(projection.right) >= 0.0;
					const Vec2 frontageDir = rightSide ? projection.right : -projection.right;
					const float structuralOuter = rightSide ? Max(0.0f, range.right) : Max(0.0f, -range.left);
					constexpr float kCellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
					const float buildingHalf = buildingFootprintXZ(building.type) * 0.5f;
					const float frontOffset = structuralOuter + 0.03f;
					const int districtIndex=nearestSettlementIndex(m_districts,static_cast<float>(sampleCenter.x),static_cast<float>(sampleCenter.y));
					const auto use=districtIndex>=0 ? UrbanMorphology::sample(m_districts[districtIndex].plan,planLocal(m_districts[districtIndex],sampleCenter)) : UrbanMorphology::LandUse{};
					const float plotDepth=use.district==UrbanMorphology::District::Industry ? 65.0f
						: (use.district==UrbanMorphology::District::OldTown ? 43.0f : 32.0f);
					const float backOffset = Max(structuralOuter + plotDepth, projection.distance + buildingHalf + 5.0f);
					const float halfAlong = Max(kCellSize * 0.49f,static_cast<float>(use.frontage)*0.45f);
					const Vec2 along = projection.tangent;
					LandPatch patch;
					patch.id = static_cast<int>(patchIndex++);
					patch.sourceParcelKey = cellKey;
					patch.type = parcelTypeFor(zone, building.type, salt);
					patch.elevationOffset = 0.010f;
					patch.materialVariant = salt;
					patch.polygon = {
						projection.position - along * halfAlong + frontageDir * frontOffset,
						projection.position + along * halfAlong + frontageDir * frontOffset,
						projection.position + along * halfAlong + frontageDir * backOffset,
						projection.position - along * halfAlong + frontageDir * backOffset
					};
					patch.polygon = UrbanParcel::partition(m_world, coord, col, row, sampleCenter, std::move(patch.polygon));
					if (patch.polygon.size() >= 3) { chunk.landPatches << patch; }
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
					const int stripWidth = Min(18, Max(6, runLength));
					int waterAbove = 0;
					int waterBelow = 0;
					for (int sx = x; sx < Min(ZONE_CELLS, x + stripWidth); ++sx)
					{
						if (y > 0 && chunk.heightMap[y - 1][sx] < -0.35f) ++waterAbove;
						if (y + 1 < ZONE_CELLS && chunk.heightMap[y + 1][sx] < -0.35f) ++waterBelow;
					}
					const int landwardSign = (waterAbove >= waterBelow) ? 1 : -1;
					const uint32 salt = settlementCellHash(getData().seed, 97, coord.x * ZONE_CELLS + x, coord.y * ZONE_CELLS + y);
					if (averageHeight >= -0.18f && averageHeight < 3.20f)
					{
						LandPatch patch;
						patch.id = static_cast<int>(patchIndex++);
						patch.type = LandPatchType::Beach;
						const int beachDepth = 3 + static_cast<int>((salt >> 6) & 3u);
						patch.polygon = makeCoastalBandPolygon(coord, x, y, stripWidth, beachDepth, landwardSign, salt);
						patch.elevationOffset = Max(0.010f, 0.035f - averageHeight * 0.24f);
						patch.materialVariant = salt;
						chunk.landPatches << patch;
					}
					else if (averageHeight >= 3.20f && averageHeight < 7.4f)
					{
						LandPatch patch;
						patch.id = static_cast<int>(patchIndex++);
						patch.type = LandPatchType::Seawall;
						const int seawallDepth = 2 + static_cast<int>((salt >> 9) & 1u);
						patch.polygon = makeCoastalBandPolygon(coord, x, y, stripWidth, seawallDepth, landwardSign, salt ^ 0x9E3779B9u);
						patch.elevationOffset = 0.035f;
						patch.materialVariant = salt;
						chunk.landPatches << patch;
					}
					x += stripWidth;
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
					if (building.edgeId >= 0 && m_network.getEdge(building.edgeId)) continue;

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
	ParcelRoadIndex roadIndex{ m_network,true };
	roadIndex.addRailway(m_trainNetwork);
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
	HashTable<int, int> parcelFailures;
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
					++parcelFailures[static_cast<int>(Polygon::Validate(patch.polygon))];
				}
			}
		}
	}

	for (const auto& [failure, count] : parcelFailures)
	{
		DebugLog::print(U"[ParcelValidation] failure={} count={}"_fmt(failure, count));
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
					if (building.edgeId < 0)
					{
						++noFrontageCount;
					}

					const Vec2 center = cellCenterXZ(chunkCoord, col, row) + Vec2{ building.offsetX, building.offsetZ };
					if (roadIndex.overlaps(ParcelGeometry::footprint(center, buildingFootprintXZ(building.type) * 0.5, building.angle)))
					{
						++roadOverlapCount; DBG_LOG(U"[BuildingOverlap] coord=({}, {}) cell=({}, {}) type={} edge={} center=({}, {}) line={}"_fmt(chunkX,chunkY,col,row,static_cast<int>(building.type),building.edgeId,center.x,center.y,__LINE__));
					}
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
					const RoadGeometry::LateralRange structuralRange = RoadGeometry::structuralRangeAt(*edge, projection.edgeT);
					const float signedLateral = static_cast<float>((center - projection.position).dot(projection.right));
					const float roadOuter = structuralRange.valid
						? ((signedLateral >= 0.0f) ? Max(0.0f, structuralRange.right) : Max(0.0f, -structuralRange.left))
						: edge->totalWidth() * 0.5f;
					const float buildingHalf = buildingFootprintXZ(building.type) * 0.5f;
					const float minimumClearance = roadOuter + buildingHalf * 0.45f;
					const float maximumFrontageDistance = roadOuter + buildingHalf + setbackFromRoadByModel(building.type, globalGX, globalGZ) + 8.0f;
					if (projection.distance < minimumClearance)
					{
						++roadOverlapCount; DBG_LOG(U"[BuildingOverlap] coord=({}, {}) cell=({}, {}) type={} edge={} center=({}, {}) line={}"_fmt(chunkX,chunkY,col,row,static_cast<int>(building.type),building.edgeId,center.x,center.y,__LINE__));
					}
					const float cosA = Math::Cos(building.angle);
					const float sinA = Math::Sin(building.angle);
					int footprintInsideRoadCorners = 0;
					for (const Vec2 localCorner : { Vec2{ -buildingHalf, -buildingHalf }, Vec2{ buildingHalf, -buildingHalf }, Vec2{ buildingHalf, buildingHalf }, Vec2{ -buildingHalf, buildingHalf } })
					{
						const Vec2 corner{ center.x + localCorner.x * cosA - localCorner.y * sinA, center.y + localCorner.x * sinA + localCorner.y * cosA };
						EdgeProjection cornerProjection;
						if (projectPointToEdgeXZ(m_network, building.edgeId, corner, cornerProjection)
							&& cornerProjection.distance < roadOuter + 0.20f)
						{
							++footprintInsideRoadCorners;
						}
					}
					if (footprintInsideRoadCorners >= 2)
					{
						++roadOverlapCount; DBG_LOG(U"[BuildingOverlap] coord=({}, {}) cell=({}, {}) type={} edge={} center=({}, {}) line={}"_fmt(chunkX,chunkY,col,row,static_cast<int>(building.type),building.edgeId,center.x,center.y,__LINE__));
					}
					if (projection.distance > maximumFrontageDistance)
					{
						++frontageDistanceCount;
					}
				}
			}
		}
	}
	const bool parcelsValid = parcelFailures.size() == 1 && parcelFailures.contains(static_cast<int>(PolygonFailureType::OK));
	const bool passed = (parcelsValid && noFrontageCount == 0 && missingEdgeCount == 0 && missingParcelCount == 0 && roadOverlapCount == 0
		&& frontageDistanceCount == 0 && coastalBuildingCount == 0);
	m_cityConstraintValidationSummary = U"buildings={} noFrontage={} missingEdge={} missingParcel={} roadOverlap={} frontageDistance={} coastal={} passed={}"_fmt(
		buildingCount, noFrontageCount, missingEdgeCount, missingParcelCount, roadOverlapCount, frontageDistanceCount, coastalBuildingCount, passed);
	Logger << U"[CityConstraintValidation] " + m_cityConstraintValidationSummary;
	DebugLog::print(U"[CityConstraintValidation] " + m_cityConstraintValidationSummary);
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

				const Vec2 center = cellCenterXZ(chunk->coord, col, row) + Vec2{ b.offsetX, b.offsetZ };
				EdgeProjection projection;
				if (!projectPointToEdgeXZ(m_network, b.edgeId, center, projection))
				{
					continue;
				}
				const Vec2 inward = ((center - projection.position).dot(projection.right) >= 0.0)
					? projection.right : -projection.right;
				const float newAngle = static_cast<float>(Atan2(-inward.x, inward.y));
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
	constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
	const int cellRadius = static_cast<int>(Ceil(360.0f / cellSize));
	Vec2 bestCenter{ WORLD_SIZE * 0.5f, WORLD_SIZE * 0.5f };
	double bestScore = -Math::Inf;

	for (const auto& settlement : m_districts)
	{
		int buildingCount = 0;
		int commercialCount = 0;
		int arterialNodeCount = 0;
		Point centerChunk;
		int centerCol = 0;
		int centerRow = 0;
		worldToZoneCell(static_cast<float>(settlement.center.x), static_cast<float>(settlement.center.y), centerChunk, centerCol, centerRow);

		for (int dz = -cellRadius; dz <= cellRadius; ++dz)
		{
			for (int dx = -cellRadius; dx <= cellRadius; ++dx)
			{
				int col = centerCol + dx;
				int row = centerRow + dz;
				Point chunkCoord = centerChunk;
				while (col < 0) { col += ZONE_CELLS; --chunkCoord.x; }
				while (col >= ZONE_CELLS) { col -= ZONE_CELLS; ++chunkCoord.x; }
				while (row < 0) { row += ZONE_CELLS; --chunkCoord.y; }
				while (row >= ZONE_CELLS) { row -= ZONE_CELLS; ++chunkCoord.y; }
				const Chunk* chunk = m_world.getChunk(chunkCoord);
				if (!chunk) continue;
				const Building& building = chunk->buildingGrid[{ col, row }];
				if (building.type == BuildingType::None || building.type == BuildingType::Farmland) continue;
				++buildingCount;
				if (building.type == BuildingType::Shop || building.type == BuildingType::Office || building.type == BuildingType::PublicFacility)
				{
					++commercialCount;
				}
			}
		}

		const double radiusSq = 720.0 * 720.0;
		for (const auto& node : m_network.nodes())
		{
			if (node.id < 0) continue;
			const double dx = node.position.x - settlement.center.x;
			const double dz = node.position.z - settlement.center.y;
			if (dx * dx + dz * dz > radiusSq) continue;
			for (const EdgeAttachment& attachment : node.attachments)
			{
				const RoadEdge* edge = m_network.getEdge(attachment.edgeId);
				if (edge && edge->roadType == RoadType::Arterial)
				{
					++arterialNodeCount;
					break;
				}
			}
		}

		const double kindBonus = (settlement.kind == MapGenerator::SettlementKind::RegionalCity) ? 50000.0
			: (settlement.kind == MapGenerator::SettlementKind::LocalTown) ? 28000.0 : 0.0;
		const double score = buildingCount * 1200.0 + commercialCount * 2200.0 + arterialNodeCount * 800.0 + kindBonus;
		if (score > bestScore)
		{
			bestScore = score;
			bestCenter = settlement.center;
		}
	}

	const float y = m_world.computeHeight(static_cast<float>(bestCenter.x), static_cast<float>(bestCenter.y));
	return Vec3{ bestCenter.x, y, bestCenter.y };
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
		if (node.id < 0 || node.attachments.size() < 3) continue;
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
		const double score = (buildingCount - commercialCount - parkingCount) * 14500.0 + commercialCount * 1200.0 + parkingCount * 800.0
			+ arterialCount * 3000.0 + degreeBonus - fallbackDistSq * 0.035;
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
Vec3 GameScene::captureIntersectionPoint(Vec3 fallback, int variant) const
{
	if (getData().captureNode >= 0)
	{
		if (const auto* node = m_network.getNode(getData().captureNode)) { return node->position; }
	}
	if (getData().captureRoadRenders)
	{
		Array<std::pair<double, int>> candidates;
		for (const auto& node : m_network.nodes())
		{
			if (node.id < 0 || node.attachments.size() < 3) { continue; }
			int arterial = 0;
			for (const auto& attachment : node.attachments)
			{
				const auto* edge = m_network.getEdge(attachment.edgeId);
				if (edge && edge->length > 100.0f)
				{
					for (const auto& part : edge->parts)
					{
						if (part.type == RoadPartType::Sidewalk && part.build == BuildState::Built) { ++arterial; break; }
					}
				}
			}
			if (arterial < 2) { continue; }
			const double score = node.position.distanceFromSq(fallback) + (node.attachments.size() == 3 ? 0.0 : 1e10);
			candidates << std::pair<double, int>{ score, node.id };
		}
		candidates.sort_by([](const auto& a, const auto& b) { return a.first < b.first; });
		if (!candidates.isEmpty())
		{
			const auto* node = m_network.getNode(candidates[Min(static_cast<size_t>(variant), candidates.size()-1)].second);
			const double y = m_world.computeHeight(static_cast<float>(node->position.x), static_cast<float>(node->position.z));
			if (variant == m_captureIndex / 2)
			{
				DebugLog::print(U"[RoadCapture] view={} node={} attachments={} position=({:.2f},{:.2f},{:.2f})"_fmt(m_captureIndex, node->id, node->attachments.size(), node->position.x, y, node->position.z));
			}
			return Vec3{ node->position.x, y, node->position.z };
		}
	}

	constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
	const float radius = 130.0f;
	const float radiusSq = radius * radius;
	const int cellRadius = static_cast<int>(Ceil(radius / cellSize));
	struct CaptureIntersectionCandidate
	{
		int nodeId = -1;
		double score = 0.0;
	};
	Array<CaptureIntersectionCandidate> candidates;
	for (const auto& node : m_network.nodes())
	{
		if (node.id < 0 || node.attachments.size() < 3) continue;
		const double fallbackDx = node.position.x - fallback.x;
		const double fallbackDz = node.position.z - fallback.z;
		const double fallbackDistSq = fallbackDx * fallbackDx + fallbackDz * fallbackDz;
		if (fallbackDistSq > 2600.0 * 2600.0) continue;

		int arterialCount = 0;
		int localCount = 0;
		for (const EdgeAttachment& attachment : node.attachments)
		{
			const RoadEdge* edge = m_network.getEdge(attachment.edgeId);
			if (!edge) continue;
			if (edge->roadType == RoadType::Arterial) ++arterialCount;
			else if (edge->roadType == RoadType::LocalRoad) ++localCount;
		}

		int buildingCount = 0;
		int commercialCount = 0;
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
			}
		}

		if (arterialCount <= 0) continue;
		const double hierarchyBonus = (arterialCount > 0 && localCount > 0) ? 140000.0 : (arterialCount > 1 ? 90000.0 : 0.0);
		const double degreeBonus = Min<double>(static_cast<double>(node.attachments.size()), 4.0) * 12000.0;
		const double score = hierarchyBonus + arterialCount * 26000.0 + localCount * 9000.0
			+ buildingCount * 6200.0 + commercialCount * 4200.0 + degreeBonus - fallbackDistSq * 0.020;
		candidates << CaptureIntersectionCandidate{ node.id, score };
	}
	candidates.sort_by([](const CaptureIntersectionCandidate& a, const CaptureIntersectionCandidate& b)
	{
		return a.score > b.score;
	});

	Array<Vec2> accepted;
	int acceptedIndex = 0;
	for (const CaptureIntersectionCandidate& candidate : candidates)
	{
		const RoadNode* node = m_network.getNode(candidate.nodeId);
		if (!node) continue;
		const Vec2 pos{ node->position.x, node->position.z };
		bool tooClose = false;
		for (const Vec2& other : accepted)
		{
			if ((pos - other).lengthSq() < 150.0 * 150.0)
			{
				tooClose = true;
				break;
			}
		}
		if (tooClose) continue;
		accepted << pos;
		if (acceptedIndex == variant)
		{
			const float y = m_world.computeHeight(static_cast<float>(node->position.x), static_cast<float>(node->position.z));
			return Vec3{ node->position.x, y, node->position.z };
		}
		++acceptedIndex;
	}
	return captureStreetCornerPoint(fallback);
}

Vec3 GameScene::captureCoastalBufferPoint(Vec3 fallback) const
{
	Vec3 best = fallback;
	double bestScore = Math::Inf;
	for (int chunkY = 0; chunkY < WORLD_CHUNKS; ++chunkY)
	{
		for (int chunkX = 0; chunkX < WORLD_CHUNKS; ++chunkX)
		{
			const Chunk* chunk = m_world.getChunk(Point{ chunkX, chunkY });
			if (!chunk) continue;
			for (const LandPatch& patch : chunk->landPatches)
			{
				if (patch.type != LandPatchType::Beach && patch.type != LandPatchType::Seawall) continue;
				if (patch.polygon.isEmpty()) continue;
				Vec2 center{ 0.0, 0.0 };
				for (const Vec2& p : patch.polygon) center += p;
				center /= static_cast<double>(patch.polygon.size());
				const double dx = center.x - fallback.x;
				const double dz = center.y - fallback.z;
				const double distSq = dx * dx + dz * dz;
				const double typeBonus = (patch.type == LandPatchType::Seawall) ? -220000.0 : 0.0;
				const double score = distSq + typeBonus;
				if (score < bestScore)
				{
					bestScore = score;
					best = Vec3{ center.x, m_world.computeHeight(static_cast<float>(center.x), static_cast<float>(center.y)), center.y };
				}
			}
		}
	}
	return best;
}
String GameScene::captureFileName(int index) const
{
	switch (index)
	{
	case 0: return U"city_render_01_overall.png";
	case 1: return U"city_render_02_city.png";
	case 2: return U"city_render_03_close.png";
	case 3: return U"city_render_04_rural_fringe.png";
	case 4: return U"city_render_05_intersection_overview.png";
	case 5: return U"city_render_06_intersection_zoom_a.png";
	case 6: return U"city_render_07_intersection_zoom_b.png";
	case 7: return U"city_render_08_intersection_zoom_c.png";
	case 8: return U"city_render_09_intersection_zoom_d.png";
	case 9: return U"city_render_10_intersection_topdown_a.png";
	case 10: return U"city_render_11_intersection_topdown_b.png";
	case 11: return U"city_render_12_intersection_zoom_e.png";
	case 12: return U"city_render_13_intersection_zoom_f.png";
	case 13: return U"city_render_14_intersection_marking_low_a.png";
	case 14: return U"city_render_15_intersection_marking_low_b.png";
	case 15: return U"city_render_16_intersection_marking_low_c.png";
	case 16: return U"city_render_17_intersection_topdown_c.png";
	case 17: return U"city_render_18_intersection_topdown_d.png";
	case 18: return U"city_render_19_intersection_topdown_e.png";
	case 19: return U"city_render_20_street_corner.png";
	case 20: return U"city_render_21_coastal_buffer.png";
	case 21: return U"city_render_22_rural_landuse.png";
	case 22: return U"city_render_23_traffic.png";
	case 23: return U"city_render_24_traffic_close.png";
	case 24: return U"city_render_25_satoyama.png";
	case 25: return U"city_render_26_provincial_town.png";
	default: return U"city_render_done.png";
	}
}
void GameScene::updateStreamingBenchmark()
{
	m_clock.speed = TimeSpeed::Paused;
	m_clock.hour = 13.0f;
	m_camera.setBlockInput(true);
	if (!m_benchmarkOrigin) { m_benchmarkOrigin = captureFocusPoint(); }
	const Vec3 origin = *m_benchmarkOrigin;
	const int flightFrame = Max(0, m_captureFrame - 120);
	Vec3 focus = origin + Vec3{flightFrame * 5.0, 0, Sin(flightFrame * 0.005) * 500.0};
	focus.y = m_world.computeHeight(static_cast<float>(focus.x), static_cast<float>(focus.z));
	m_camera.setCaptureState(focus, 650.0f, static_cast<float>(-45.0_deg), static_cast<float>(48.0_deg));
	const Stopwatch timer{StartImmediately::Yes};
	m_world.update(focus);
	renderWorld();
	if (m_captureFrame == 60 && m_worldRenderer.pendingTerrainJobs() > 0) { return; }
	if (m_captureFrame >= 120)
	{
		const double cpuMs = timer.msF();
		m_captureCpuTimes << cpuMs;
		m_captureFrameTimes << Scene::DeltaTime() * 1000.0;
		if (cpuMs > 40.0)
		{
			DebugLog::print(U"[FlightSpike] frame={} cpuMs={:.2f} terrainMs={:.2f} roadsMs={:.2f} shadowMs={:.2f} hudMs={:.2f}"_fmt(
				flightFrame, cpuMs, m_renderTimings.terrainOnly, m_renderTimings.roadMesh, m_cityLighting.shadowMilliseconds(),m_renderTimings.uiRenderer));
		}
	}
	if (++m_captureFrame >= 840)
	{
		TextWriter csv{U"streaming_frames.csv"};
		csv.writeln(U"frame,cpuMs,frameMs");
		for (size_t i = 0; i < m_captureCpuTimes.size(); ++i)
		{
			csv.writeln(U"{},{:.3f},{:.3f}"_fmt(i,m_captureCpuTimes[i],m_captureFrameTimes[i]));
		}
		DebugLog::print(U"[StreamingBenchmark] complete sync={} frames={}"_fmt(getData().syncTerrain,m_captureCpuTimes.size()));
		System::Exit();
	}
}

void GameScene::updateCaptureCityRenders()
{
	const int kCaptureCount = getData().captureRoadRenders ? 6 : 26;
	const int warmupFrames = m_captureIndex == 22 ? 240 : 80;

	m_clock.speed = TimeSpeed::Paused;
	m_clock.hour = 13.0f;
	m_camera.setBlockInput(true);

	if (m_captureCameraDirty)
	{
		if (getData().captureRoadRenders && m_captureIndex == 0)
		{
			int checked = 0, repaired = 0, missing = 0;
			for (const auto& node : m_network.nodes())
			{
				if (node.id < 0 || node.attachments.size() < 2) { continue; }
				bool needsCap = false;
				int builtApproaches = 0;
				for (const auto& attachment : node.attachments)
				{
					const auto* edge = m_network.getEdge(attachment.edgeId);
					if (!edge || !edge->isRoadbedBuilt() || (edge->edgeState != EdgeState::Open && edge->edgeState != EdgeState::Existing)) { continue; }
					++builtApproaches;
					needsCap = needsCap || ((edge->nodeA == node.id ? edge->cutoffA : edge->cutoffB) > 0.15f);
				}
				if (builtApproaches < 2 || !needsCap) { continue; }
				++checked;
				const auto layout = JunctionGeometry::build(m_network,node.id);
				repaired += layout.repaired;
				if (layout.asphalt.indices.isEmpty()) { ++missing; DebugLog::print(U"[JunctionAudit] missing node={}"_fmt(node.id)); }
			}
			DebugLog::print(U"[JunctionAudit] checked={} repaired={} missing={}"_fmt(checked,repaired,missing));
		}
		const Vec3 focus = captureFocusPoint();
		const Vec3 streetFocus = captureStreetCornerPoint(focus);
		const Vec3 intersectionA = captureIntersectionPoint(focus, 0);
		const Vec3 intersectionB = captureIntersectionPoint(focus, 1);
		const Vec3 intersectionC = captureIntersectionPoint(focus, 2);
		const Vec3 intersectionD = captureIntersectionPoint(focus, 3);
		const Vec3 intersectionE = captureIntersectionPoint(focus, 4);
		const Vec3 intersectionF = captureIntersectionPoint(focus, 5);
		const Vec3 fringeFocus = captureRuralFringePoint(focus);
		const Vec3 coastalFocus = captureCoastalBufferPoint(focus);
		if (getData().captureRoadRenders)
		{
			const Vec3 roadFocus = captureIntersectionPoint(focus, m_captureIndex / 2);
			const bool overhead = (m_captureIndex % 2) != 0;
			m_camera.setCaptureState(roadFocus, overhead ? 165.0f : 125.0f,
				static_cast<float>(-40.0_deg), overhead ? static_cast<float>(86.0_deg) : static_cast<float>(43.0_deg));
		}
		else
		switch (m_captureIndex)
		{
		case 0:
			m_camera.setCaptureState(focus, 1800.0f, static_cast<float>(-39.0_deg), static_cast<float>(58.0_deg));
			break;
		case 1:
			m_camera.setCaptureState(focus + Vec3{ 120.0, 0.0, -60.0 }, 820.0f, static_cast<float>(-45.0_deg), static_cast<float>(43.0_deg));
			break;
		case 2:
			m_camera.setCaptureState(streetFocus + Vec3{ 34.0, 0.0, -24.0 }, 230.0f, static_cast<float>(-50.0_deg), static_cast<float>(30.0_deg));
			break;
		case 3:
			m_camera.setCaptureState(fringeFocus + Vec3{ 70.0, 0.0, -58.0 }, 680.0f, static_cast<float>(-45.0_deg), static_cast<float>(48.0_deg));
			break;
		case 4:
			m_camera.setCaptureState(intersectionA + Vec3{ 24.0, 0.0, -18.0 }, 260.0f, static_cast<float>(-45.0_deg), static_cast<float>(48.0_deg));
			break;
		case 5:
			m_camera.setCaptureState(intersectionA + Vec3{ 18.0, 0.0, -14.0 }, 150.0f, static_cast<float>(-44.0_deg), static_cast<float>(42.0_deg));
			break;
		case 6:
			m_camera.setCaptureState(intersectionB + Vec3{ -28.0, 0.0, 22.0 }, 170.0f, static_cast<float>(-32.0_deg), static_cast<float>(44.0_deg));
			break;
		case 7:
			m_camera.setCaptureState(intersectionC + Vec3{ 28.0, 0.0, 18.0 }, 180.0f, static_cast<float>(-58.0_deg), static_cast<float>(42.0_deg));
			break;
		case 8:
			m_camera.setCaptureState(intersectionD + Vec3{ -18.0, 0.0, -26.0 }, 135.0f, static_cast<float>(34.0_deg), static_cast<float>(40.0_deg));
			break;
		case 9:
			m_camera.setCaptureState(intersectionA, 210.0f, static_cast<float>(0.0_deg), static_cast<float>(86.0_deg));
			break;
		case 10:
			m_camera.setCaptureState(intersectionB, 230.0f, static_cast<float>(90.0_deg), static_cast<float>(84.0_deg));
			break;
		case 11:
			m_camera.setCaptureState(intersectionE + Vec3{ 20.0, 0.0, 24.0 }, 140.0f, static_cast<float>(68.0_deg), static_cast<float>(43.0_deg));
			break;
		case 12:
			m_camera.setCaptureState(intersectionF + Vec3{ -22.0, 0.0, -18.0 }, 125.0f, static_cast<float>(-118.0_deg), static_cast<float>(41.0_deg));
			break;
		case 13:
			m_camera.setCaptureState(intersectionA + Vec3{ 9.0, 0.0, -7.0 }, 78.0f, static_cast<float>(-42.0_deg), static_cast<float>(31.0_deg));
			break;
		case 14:
			m_camera.setCaptureState(intersectionB + Vec3{ -9.0, 0.0, 9.0 }, 82.0f, static_cast<float>(132.0_deg), static_cast<float>(32.0_deg));
			break;
		case 15:
			m_camera.setCaptureState(intersectionC + Vec3{ 11.0, 0.0, 8.0 }, 86.0f, static_cast<float>(38.0_deg), static_cast<float>(33.0_deg));
			break;
		case 16:
			m_camera.setCaptureState(intersectionC, 190.0f, static_cast<float>(180.0_deg), static_cast<float>(86.0_deg));
			break;
		case 17:
			m_camera.setCaptureState(intersectionD, 185.0f, static_cast<float>(270.0_deg), static_cast<float>(86.0_deg));
			break;
		case 18:
			m_camera.setCaptureState(intersectionE, 170.0f, static_cast<float>(45.0_deg), static_cast<float>(86.0_deg));
			break;
		case 19:
			m_camera.setCaptureState(streetFocus + Vec3{ 26.0, 0.0, -20.0 }, 115.0f, static_cast<float>(27.0_deg), static_cast<float>(54.0_deg));
			break;
		case 20:
			m_camera.setCaptureState(coastalFocus + Vec3{ 62.0, 0.0, -42.0 }, 260.0f, static_cast<float>(38.0_deg), static_cast<float>(43.0_deg));
			break;
		case 22:
		{
			m_camera.setCaptureState(streetFocus, 300.0f, static_cast<float>(-50.0_deg), static_cast<float>(40.0_deg));
			Array<std::pair<double, int>> candidates;
			for (const auto& edge : m_network.edges())
			{
				if (!edge.isRoadbedBuilt() || edge.length < 25 || !m_simGraph->getEdge(edge.id)) { continue; }
				const auto bezier = m_network.getBezier(edge.id);
				if (!bezier) { continue; }
				const double distance = bezier->positionAt(bezier->totalLength * 0.5f).distanceFrom(streetFocus);
				if (distance < 400) { candidates.emplace_back(distance, edge.id); }
			}
			candidates.sort();
			Reseed(getData().seed);
			for (size_t index = 0; index < Min(size_t{ 200 }, candidates.size()); ++index)
			{
				const VehicleType type = index % 3 == 0 ? VehicleType::KeiCar : VehicleType::PassengerCar;
				m_vehicleManager.spawnOnEdge(candidates[index].second, *m_simGraph, type,
					candidates[(index + candidates.size() / 2) % candidates.size()].second);
			}
			DebugLog::print(U"[TrafficCapture] spawned={} candidates={}"_fmt(m_vehicleManager.vehicles().size(), candidates.size()));
			break;
		}
		case 23:
		{
			Vec3 trafficFocus = streetFocus;
			double nearest = Math::Inf;
			for (const Vehicle& vehicle : m_renderVehicles)
			{
				const double distance = vehicle.position.distanceFrom(streetFocus);
				if (vehicle.speed > 1.0f && distance < nearest) { trafficFocus = vehicle.position; nearest = distance; }
			}
			m_camera.setCaptureState(trafficFocus, 70.0f, static_cast<float>(-38.0_deg), static_cast<float>(45.0_deg));
			break;
		}
		case 24:
		{
			Vec3 woodland = fringeFocus;
			double best = Math::Inf;
			for (int z = 1; z < WORLD_CHUNKS; ++z) for (int x = 1; x < WORLD_CHUNKS; ++x)
			{
				const Chunk* chunk = m_world.getChunk(Point{x,z});
				if (!chunk) { continue; }
				for (int row = 8; row < ZONE_CELLS; row += 8) for (int col = 8; col < ZONE_CELLS; col += 8)
				{
					if (chunk->zoneMap[{col,row}] != ZoneType::Unzoned) { continue; }
					const Vec3 point{x*CHUNK_SIZE+col*16.0,chunk->heightMap[{col,row}],z*CHUNK_SIZE+row*16.0};
					if (point.y < 45 || point.y > 180) { continue; }
					const double score = point.distanceFromSq(fringeFocus);
					if (score < best) { woodland = point; best = score; }
				}
			}
			m_camera.setCaptureState(woodland,1000.0f,static_cast<float>(-55.0_deg),static_cast<float>(36.0_deg));
			DebugLog::print(U"[PhotoReferenceCapture] satoyama=({:.1f},{:.1f},{:.1f})"_fmt(woodland.x,woodland.y,woodland.z));
			break;
		}
		case 25:
		{
			Vec3 provincial = fringeFocus;
			for (const auto& settlement : m_districts)
			{
				if (settlement.kind != MapGenerator::SettlementKind::LocalTown) { continue; }
				provincial = Vec3{settlement.center.x,m_world.computeHeight(static_cast<float>(settlement.center.x),static_cast<float>(settlement.center.y)),settlement.center.y};
				break;
			}
			m_camera.setCaptureState(provincial,300.0f,static_cast<float>(-40.0_deg),static_cast<float>(35.0_deg));
			break;
		}
		case 21:
			m_camera.setCaptureState(fringeFocus + Vec3{ -80.0, 0.0, 70.0 }, 520.0f, static_cast<float>(128.0_deg), static_cast<float>(50.0_deg));
			break;
		}
		m_captureCameraDirty = false;
		m_captureFrameTimes.clear();
		m_captureCpuTimes.clear();
		m_captureGpuTimes.clear();
		m_captureFrame = 0;
	}

	m_world.update(m_camera.focusPoint());
	if (m_captureIndex >= 22)
	{
		for (auto& response : m_simThread.drainResponses())
		{
			if (const auto* route = std::get_if<RouteResponse>(&response)) { m_vehicleManager.applyRouteResponse(*route); }
		}
		constexpr double kCaptureTimeStep = 1.0 / 60.0;
		m_clock.now += kCaptureTimeStep;
		m_vehicleManager.update(kCaptureTimeStep, m_clock.now, *m_simGraph, m_network, m_roadRenderer.visibleEdges());
		for (auto& request : m_vehicleManager.collectRequests()) { m_simThread.pushRequest(std::move(request)); }
	}
	renderWorld();
	if (m_captureIndex == 22 && m_captureFrame == 20)
	{
		for (const Vehicle& vehicle : m_renderVehicles) { m_captureVehicleStartPositions[vehicle.id] = vehicle.position; }
	}

	if (m_captureFrame >= warmupFrames - 60 && m_captureFrame < warmupFrames)
	{
		m_captureFrameTimes << Scene::DeltaTime() * 1000.0;
		m_captureCpuTimes << m_renderTimings.total;
		if (m_gpuTimer.milliseconds() >= 0.0)
		{
			m_captureGpuTimes << m_gpuTimer.milliseconds();
		}
	}
	// Only sample settled geometry; async terrain must finish before the final 60 frames.
	if (m_captureFrame == warmupFrames - 61 && m_worldRenderer.pendingTerrainJobs() > 0) { return; }
	if (++m_captureFrame == warmupFrames)
	{
		FileSystem::CreateDirectories(U"Screenshot/city_generation");
		const String fileName = U"city_generation/" + captureFileName(m_captureIndex);
		ScreenCapture::SaveCurrentFrame(fileName);
		DebugLog::print(U"[CapturePerf] view={} frameMs={:.2f} cpuMs={:.2f} terrainMs={:.2f} roadsMs={:.2f} shadowMs={:.2f}"_fmt(
			m_captureIndex, Scene::DeltaTime() * 1000.0, m_renderTimings.total,
			m_renderTimings.terrainOnly, m_renderTimings.roadMesh, m_cityLighting.shadowMilliseconds()));
		auto percentile = [](Array<double> values, double fraction)
		{
			if (values.isEmpty()) { return -1.0; }
			values.sort();
			return values[Min(values.size() - 1, static_cast<size_t>(fraction * (values.size() - 1)))];
		};
		DebugLog::print(U"[CaptureDistribution] view={} samples={} frameP50={:.2f} frameP95={:.2f} cpuP50={:.2f} gpuForwardP50={:.2f} gpuForwardP95={:.2f}"_fmt(
			m_captureIndex, m_captureFrameTimes.size(), percentile(m_captureFrameTimes, 0.5),
			percentile(m_captureFrameTimes, 0.95), percentile(m_captureCpuTimes, 0.5),
			percentile(m_captureGpuTimes, 0.5), percentile(m_captureGpuTimes, 0.95)));
		DebugLog::print(U"[BuildingVisibility] submitted={} considered={}"_fmt(
			m_worldRenderer.buildingsSubmitted(), m_worldRenderer.buildingsConsidered()));
		if (m_captureIndex >= 22)
		{
			int moving = 0;
			double totalDistance = 0;
			for (const Vehicle& vehicle : m_renderVehicles)
			{
				const auto start = m_captureVehicleStartPositions.find(vehicle.id);
				if (start != m_captureVehicleStartPositions.end())
				{
					const double distance = vehicle.position.distanceFrom(start->second);
					if (distance > 1) { ++moving; totalDistance += distance; }
				}
			}
			DebugLog::print(U"[TrafficCapture] total={} active={} moved={} meanDistanceM={:.2f}"_fmt(
				m_vehicleManager.vehicles().size(), m_renderVehicles.size(), moving, totalDistance / Max(1, moving)));
		}
		DebugLog::print(U"[CaptureCity] saved Screenshot/{}"_fmt(fileName));
	}
	else if (m_captureFrame > warmupFrames + 8)
	{
		++m_captureIndex;
		if (m_captureIndex >= kCaptureCount)
		{
			{
				TextWriter writer{ U"Screenshot/city_generation/validation_report.txt" };
				if (writer)
				{
					writer << U"passed=" + String{ m_cityConstraintValidationPassed ? U"true" : U"false" };
					writer << U"\n";
					writer << m_cityConstraintValidationSummary;
				}
			}
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

bool GameScene::startRoadPlanConstruction(int planId)
{
	const RoadPlan* plan = m_network.getPlan(planId);
	if (!plan || plan->state != PlanState::Planning) return false;
	if (!m_network.startPlanConstruction(planId, m_clock.now)) return false;
	prepareConstructionSite(plan->edgeIds);
	return true;
}

void GameScene::prepareConstructionSite(const Array<int>& edgeIds)
{
	if (edgeIds.isEmpty()) return;
	Stopwatch timer{StartImmediately::Yes};
	for (const int id : edgeIds)
	{
		if (const auto* edge = m_network.getEdge(id); edge && edge->useElevation)
			m_network.generatePiersForEdge(id, m_world);
	}
	const auto affected = RoadConstruction::affectedCells(m_network, edgeIds, m_world,
		[this](const Chunk& chunk, int col, int row) -> Optional<RoadConstruction::Bounds>
		{
			const auto box = m_worldRenderer.buildingHitBox(chunk,m_world,col,row);
			if (!box) return none;
			return RoadConstruction::Bounds{box->center,box->size,chunk.buildingGrid[{col,row}].angle};
		});
	const int removed = m_clearanceLedger.clear(m_world,affected);
	m_worldRenderer.invalidateTerrainForEdges(m_network,edgeIds);
	for (const int id : edgeIds) m_roadRenderer.invalidateEdgeCache(id);
	DebugLog::print(U"[Construction] 既設物撤去工: edges={} buildings={} reservedCells={} ms={:.3f}"_fmt(
		edgeIds.size(),removed,affected.size(),timer.msF()));
}

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
	// Legacy single edges also progress and open; they previously remained under construction forever.
	for (const auto& edge : m_network.edges())
	{
		if (edge.id < 0 || edge.planId >= 0 || edge.edgeState != EdgeState::UnderConstruction) continue;
		if (RoadConstruction::progress(m_network,edge,m_clock.now).stage == RoadConstruction::Stage::Complete)
		{
			m_network.getEdge(edge.id)->edgeState = EdgeState::Open;
			dirtyNodes << edge.nodeA << edge.nodeB;
		}
	}
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

	if (m_restoreConstructionSites)
	{
		m_restoreConstructionSites = false;
		Array<int> edgeIds;
		for (const auto& edge : m_network.edges())
			if (edge.id >= 0 && edge.edgeState == EdgeState::UnderConstruction) edgeIds << edge.id;
		prepareConstructionSite(edgeIds);
	}
	if (getData().inspectNode >= 0)
	{
		if (const auto* node = m_network.getNode(getData().inspectNode))
		{
			Vec3 focus = node->position;
			focus.y = m_world.computeHeight(static_cast<float>(focus.x),static_cast<float>(focus.z));
			m_camera.setCaptureState(focus,125.0f,static_cast<float>(-40.0_deg),static_cast<float>(43.0_deg));
			m_clock.speed = TimeSpeed::Paused;
			m_clock.hour = 13.0f;
			m_world.update(focus);
		}
		getData().inspectNode = -1;
	}

	if (getData().auditRoadIntegrity)
	{
		const auto result = writeGameSnapshot(U"road_integrity_snapshot");
		DBG_LOG(U"[RoadIntegrityAudit] snapshot success={} path={}"_fmt(result.success,result.path));
		System::Exit();
		return;
	}
	if (getData().captureFirstPerson) { updateStreetReview(); return; }
	if (getData().captureTransport) { updateTransportReview(); return; }
	if (getData().captureConstruction)
	{
		updateConstructionReview();
		return;
	}
	if (getData().captureRoadPlanUx)
	{
		updateRoadPlanReview();
		return;
	}

	if (getData().benchmarkStreaming)
	{
		updateStreamingBenchmark();
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
	if (m_selectedLandParcel && !m_panelManager.isVisible(U"land_info")) { clearSelection(); }
	m_minimapRenderer.setSmallBounds(m_uiRenderer.minimapBounds());
	if (!m_showPauseMenu && (m_minimapRenderer.fullScreen() || PanelWidget::activeTextInput==nullptr))
	{
		if (const auto target=m_minimapRenderer.update(m_camera,m_network,m_trainNetwork,m_districts))
		{
			jumpToMapPosition(*target);
		}
	}
	const bool mapInput=m_minimapRenderer.consumedInput();
	if (!m_showPauseMenu && !mapInput)
		m_panelManager.handleInput();
	m_uiRenderer.updateLayout(m_hudStats);
	if (!m_showPauseMenu && !mapInput && !m_panelManager.blocksMouseInput()) { m_uiRenderer.handleInput(); }
	m_uiRenderer.updateLayout(m_hudStats);
	m_minimapRenderer.setSmallBounds(m_uiRenderer.minimapBounds());
	m_camera.setBlockInput(mapInput || m_showPauseMenu || m_panelManager.isMouseOnAnyPanel() || m_uiRenderer.isMouseOnHud());
	if (!mapInput) { m_camera.update(dt, m_world); }
	m_logicMs = swLogic.msF();

	// 車両追跡
	if (!mapInput && m_trackingVehicle && m_selectedVehicleId)
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

	if (!mapInput) { updateCursor(); handleInput(); m_debugRenderer.handleInput(); }

	renderWorld();
}
void GameScene::jumpToMapPosition(Vec2 target)
{
	m_trackingVehicle=false;
	target.x=Clamp(target.x,.5,static_cast<double>(WORLD_SIZE)-.5); target.y=Clamp(target.y,.5,static_cast<double>(WORLD_SIZE)-.5);
	m_camera.setFocus({target.x,m_world.sampleHeight(static_cast<float>(target.x),static_cast<float>(target.y)),target.y});
}

void GameScene::updateTransportReview()
{
	m_clock.speed=TimeSpeed::Paused; m_clock.hour=13;
	if (m_captureCameraDirty)
	{
		const Vec3 focus=captureFocusPoint();
		if (m_captureIndex==0) { m_camera.setCaptureState(focus,850,static_cast<float>(-35_deg),static_cast<float>(68_deg)); }
		if (m_captureIndex==1) { m_camera.setCaptureState(captureStreetCornerPoint(focus),100,static_cast<float>(-40_deg),static_cast<float>(32_deg)); }
		if (m_captureIndex==2 && !m_trainNetwork.nodes().isEmpty()) { m_camera.setCaptureState(m_trainNetwork.nodes().front().position,220,static_cast<float>(-50_deg),static_cast<float>(32_deg)); }
		if (m_captureIndex==3)
		{
			for (const auto& edge : m_network.edges())
			{
				if (edge.id<0 || !edge.useElevation) { continue; }
				const Vec3 point=m_network.getBezier(edge.id)->evaluate(.5f);
				if (m_world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z))<-1)
				{
					m_camera.setCaptureState(point,200,static_cast<float>(-40_deg),static_cast<float>(42_deg)); break;
				}
			}
		}
		if (m_captureIndex==4) { m_minimapRenderer.openFullScreen(m_camera,m_network,m_trainNetwork,m_districts); }
		if (m_captureIndex==5)
		{
			auto& view=m_minimapRenderer.mapView(); view.center={focus.x,focus.z};
			view.zoomAt(view.body(Scene::Size()).center(),32,Scene::Size());
			view.showContext(view.body(Scene::Size()).center()+Vec2{80,60},Scene::Size());
		}
		if (m_captureIndex==6)
		{
			m_minimapRenderer.mapView().close();
			for (const auto& reach : m_world.rivers().reaches)
			{
				if (reach.start.y>2 && reach.start.y<45 && reach.halfWidth>30)
				{ m_camera.setCaptureState((reach.start+reach.end)*.5,280,static_cast<float>(-40_deg),static_cast<float>(35_deg)); break; }
			}
		}
		if (m_captureIndex==7)
		{
			for (const auto& edge : m_trainNetwork.edges())
			{
				const Vec3 p=m_trainNetwork.getBezier(edge.id)->evaluate(.5f);const double gap=p.y-m_world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z));
				if (gap>8 && gap<14) { m_camera.setCaptureState(p,18,static_cast<float>(-30_deg),static_cast<float>(-18_deg)); break; }
			}
		}
		if (m_captureIndex==8 || m_captureIndex==9)
		{
			const auto visit=[&](const CubicBezier& curve)
			{
				for (float arc=0;arc<curve.totalLength;arc+=4)
				{
					const Vec3 p=curve.positionAt(arc),tangent=curve.tangentAt(arc);const double depth=m_world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z))-p.y;
					if (depth>2 && depth<5) { m_camera.setCaptureState(p+Vec3{0,2.4,0},18,static_cast<float>(Atan2(-tangent.x,-tangent.z)),0);return true; }
				}
				return false;
			};
			if (m_captureIndex==8) { for (const auto& edge : m_network.edges()) { if (edge.id>=0 && edge.tunnel && visit(*m_network.getBezier(edge.id))) { break; } } }
			else { for (const auto& edge : m_trainNetwork.edges()) { if (edge.id>=0 && visit(*m_trainNetwork.getBezier(edge.id))) { break; } } }
		}
		if (m_captureIndex==10)
		{
			m_minimapRenderer.openFullScreen(m_camera,m_network,m_trainNetwork,m_districts);auto& view=m_minimapRenderer.mapView();view.zoom=2;
			const Vec2 before=view.center;view.panKeyboard({1,-1},.1,Scene::Size());
			DBG_LOG(U"[MapKeyboardReview] movedPixels={:.2f} rivers={} boundaries={}"_fmt(view.center.distanceFrom(before)*view.scale(Scene::Size()),view.rivers.size(),view.boundaries.size()));
		}
		m_captureCameraDirty=false; m_captureFrame=0;
	}
	m_world.update(m_camera.focusPoint()); const Stopwatch frameTimer{StartImmediately::Yes}; renderWorld();
	if (m_captureFrame==70 && m_worldRenderer.pendingTerrainJobs()>0 && !m_minimapRenderer.fullScreen()) { return; }
	if (++m_captureFrame==75)
	{
		ScreenCapture::SaveCurrentFrame(U"transport_{:02}.png"_fmt(m_captureIndex));
		DBG_LOG(U"[TransportReview] rendered={} map={} cpuDrawMs={:.2f}"_fmt(m_captureIndex,m_minimapRenderer.fullScreen(),frameTimer.msF()));
	}
	if (m_captureFrame>82)
	{
		if (++m_captureIndex==11)
		{
			auto& view=m_minimapRenderer.mapView();
			view.showContext(view.body(Scene::Size()).center(),Scene::Size());
			const auto target=view.jumpFromMenu(view.menuBounds().center());
			if (target) { jumpToMapPosition(*target); }
			const Vec3 focus=m_camera.focusPoint();
			const bool passed=target && Vec2{focus.x,focus.z}.distanceFrom(*target)<.01 && !view.visible;
			DBG_LOG(U"[TransportReview] mapJumpPassed={} streets={} labels={}"_fmt(passed,view.streets.size(),view.labels.size()));
			System::Exit(); return;
		}
		m_captureCameraDirty=true;
	}
}

void GameScene::updateStreetReview()
{
	constexpr int kViewCount = 6;
	constexpr int kWarmupFrames = 140;
	constexpr int kSampleFrames = 60;
	m_clock.speed = TimeSpeed::Paused;
	m_clock.hour = 13;
	if (m_streetReviewEdges.isEmpty())
	{
		HashTable<int, std::array<int, 3>> counts;
		for (int z = 0; z < WORLD_CHUNKS; ++z)
		{
			for (int x = 0; x < WORLD_CHUNKS; ++x)
			{
				const auto* chunk = m_world.getChunk({x, z});
				if (!chunk) { continue; }
				for (const auto& building : chunk->buildingGrid)
				{
					if (building.edgeId < 0) { continue; }
					auto& count = counts[building.edgeId];
					if (building.type == BuildingType::Shop || building.type == BuildingType::Office) { ++count[0]; }
					else if (building.type == BuildingType::Detached || building.type == BuildingType::LowApartment) { ++count[1]; }
				}
			}
		}
		for (int category = 0; category < 3; ++category)
		{
			double bestScore = -1;
			Optional<int> best;
			for (const auto& edge : m_network.edges())
			{
				if (edge.id < 0 || edge.useElevation || edge.length < 85 || edge.length > 600 || edge.totalWidth() > 18) { continue; }
				const auto iterator = counts.find(edge.id);
				if (iterator == counts.end()) { continue; }
				const Vec3 point = m_network.getBezier(edge.id)->evaluate(.5f);
				double villageDistance = Math::Inf;
				for (const auto& settlement : m_districts)
				{
					if (settlement.kind == MapGenerator::SettlementKind::RuralSettlement)
					{ villageDistance = Min(villageDistance, Vec2{point.x, point.z}.distanceFrom(settlement.center)); }
				}
				const auto& count = iterator->second;
				double score = category == 0 ? count[0] * 4.0 - count[1] : count[1] * 3.0 - count[0];
				if (category == 1 && villageDistance < 1600) { continue; }
				if (category == 2) { score = villageDistance < 500 ? count[1] * 2.0 + edge.length * .02 - villageDistance * .01 : -1; }
				if (score > bestScore) { bestScore = score; best = edge.id; }
			}
			if (!best) { DBG_LOG(U"[StreetReview] no candidate category={}"_fmt(category)); System::Exit(); return; }
			m_streetReviewEdges << *best;
		}
	}
	if (m_captureCameraDirty)
	{
		const auto* edge = m_network.getEdge(m_streetReviewEdges[m_captureIndex % 3]);
		const auto curve = m_network.getBezier(edge->id);
		const float arc = curve->totalLength * (m_captureIndex < 3 ? .25f : .75f);
		Vec3 point = curve->positionAt(arc);
		Vec3 direction = curve->tangentAt(arc) * (m_captureIndex < 3 ? 1 : -1);
		point += tangentToRight(direction) * (edge->totalWidth() * .30);
		point.y = m_world.sampleHeight(static_cast<float>(point.x), static_cast<float>(point.z));
		m_camera.setWalkingState(point, static_cast<float>(Atan2(direction.x, direction.z)));
		DBG_LOG(U"[StreetReviewCamera] view={} edge={} ground={} eye={} yaw={}"_fmt(m_captureIndex, edge->id, point, m_camera.eyePosition(), Atan2(direction.x, direction.z)));
		m_captureCameraDirty = false;
		m_captureFrame = 0;
		m_captureFrameTimes.clear();
		m_captureCpuTimes.clear();
	}
	m_world.update(m_camera.focusPoint());
	renderWorld();
	if (m_captureFrame == kWarmupFrames - kSampleFrames - 1 && m_worldRenderer.pendingTerrainJobs() > 0) { return; }
	if (m_captureFrame >= kWarmupFrames - kSampleFrames && m_captureFrame < kWarmupFrames)
	{
		m_captureFrameTimes << Scene::DeltaTime() * 1000;
		m_captureCpuTimes << m_renderTimings.total;
	}
	if (++m_captureFrame == kWarmupFrames)
	{
		ScreenCapture::SaveCurrentFrame(U"street_{:02}.png"_fmt(m_captureIndex));
		m_captureFrameTimes.sort(); m_captureCpuTimes.sort();
		DBG_LOG(U"[StreetReview] view={} frames={} frameP50={:.2f} frameP95={:.2f} cpuP50={:.2f} buildings={}"_fmt(m_captureIndex, m_captureFrameTimes.size(), m_captureFrameTimes[30], m_captureFrameTimes[56], m_captureCpuTimes[30], m_worldRenderer.buildingsSubmitted()));
	}
	if (m_captureFrame > kWarmupFrames + 8)
	{
		if (++m_captureIndex == kViewCount) { System::Exit(); return; }
		m_captureCameraDirty = true;
	}
}
