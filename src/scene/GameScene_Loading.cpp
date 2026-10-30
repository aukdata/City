#include <exception>
#include "../debug/DebugLog.hpp"
#include "GameScene.hpp"
#include "../gen/DistrictRoads.hpp"
#include "../render/VegetationProfile.hpp"
#include "../gen/RoadTerrainFit.hpp"
#include "../asset/AssetRegistrar.hpp"
#include "../ui/LoadingRecovery.hpp"
#include "../gen/SettlementNames.hpp"

/// @file
/// @brief ロード状態の管理。生成処理の完了を確認してから通常更新へ遷移する。

void GameScene::initLoadGame()
{
	startLoadingPhase(LoadingTask::LoadGame, U"ロード中...", U"セーブデータを読み込み中...", [this]()
	{
		m_loadGameResult = loadGame();
		if (m_loadGameResult) { publishForestCover(); }
	});
}

void GameScene::publishForestCover()
{
	const Stopwatch timer{StartImmediately::Yes};
	VegetationProfile::ForestCover::publish(std::make_shared<const VegetationProfile::ForestCover>(m_world));
	DBG_LOG(U"[ForestCover] published ms={:.0f}"_fmt(timer.msF()));
}

void GameScene::initNewGame()
{
	m_worldRenderer.setWoodlandEnabled(getData().generation.enabled(GenerationOptions::Element::Trees));
	// 新規ゲーム開始時は地形から建物配置までの生成パイプラインを非同期ロード段階へ載せる。
	const auto initResult = MapGenerator::initWorld(getData().seed, m_world);
	m_placeNames = std::move(initResult.placeNames);

	m_world.reserveChunks();

	const float worldCenter = WORLD_SIZE * 0.5f;
	m_camera.setFocus(Vec3{ worldCenter, 0.0, worldCenter });

	m_totalInitChunks = WORLD_CHUNKS * WORLD_CHUNKS;

	startLoadingPhase(LoadingTask::NewGame, U"マップ生成中...", U"地形生成中", [this]()
	{
		using Element = GenerationOptions::Element;
		const auto& options = getData().generation;
		m_world.generateSourceWater(options.enabled(Element::Rivers));
		generateAllTerrain();
		if (options.enabled(Element::Settlements))
		{
			placeAllSettlements();
		}
		if (options.enabled(Element::Roads))
		{
			generateAllRoads();
			generateDistrictRoads();
			postProcessRoads();
		}
		if (options.enabled(Element::Expressway))
		{
			generateExpressways();
		}
		if (options.enabled(Element::Railway))
		{
			MapGenerator::setupTrain(m_trainNetwork, m_world, m_districts, &m_network);
		}
		if (options.enabled(Element::Roads))
		{
			// 鉄道用地が決まってから、街路のない深い街区に路地を通す。
			const auto lanes = DistrictRoads::subdivideDeepBlocks(m_districts, m_world, m_network, m_trainNetwork);
			if (!lanes.empty()) { RoadTerrainFit::apply(m_network, m_world, lanes); }
		}
		m_districtHierarchy.generate(m_world,m_districts);
		m_network.recomputeAllAutoSigns();
		m_roadRenderer.setMunicipalityLookup([this](Vec2 point)
		{
			const int id = m_districtHierarchy.at(point, 0);
			return id >= 0 ? m_districtHierarchy.areas[id].name : U"";
		});
		m_trainNetwork.synchronize();
		// 建物は計画水位だけでなく、実際に流した初期の川の水面も避けて配置・検証する。
		setLoadingStatus(U"川の流れを準備中...");
		m_world.prepareSourceWaterForNewGame();
		if (options.enabled(Element::Buildings))
		{
			applyZonesGlobal();
			placeInitialBuildings();
		}
		publishForestCover();
	});
	DebugLog::print(U"[Loading] {} チャンク生成開始"_fmt(m_totalInitChunks));
}

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

void GameScene::updateLoading()
{
	// Only consumed futures may leave this scene: the pipeline captures this.
	if (m_loadingFailed && !m_generationFuture.valid())
	{
		drawLoadingScreen(Clamp(m_genProgress.load(), 0.0f, 1.0f));
		if (LoadingRecovery::draw(FontAsset(Asset::UI20), Scene::Size(), m_loadingFailed, m_generationFuture.valid()))
		{
			DBG_LOG(U"[Loading] Return to title after failure: {}"_fmt(m_loadingError));
			changeScene(SceneState::Title, 0s);
		}
		return;
	}

	// バックグラウンド生成の完了待ちと、成功時の最終初期化・失敗時の画面維持をここで分岐する。
	if (m_generationFuture.valid())
	{
		// 建物模型の読み込みは生成結果に依存しない。生成スレッドの処理中に描画スレッドで済ませ、完了後の待ち時間を減らす。
		if (!m_buildingModelsPreloaded && m_generationFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		{
			m_worldRenderer.preloadBuildingModels();
			m_buildingModelsPreloaded = true;
		}
		if (m_generationFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			const auto failLoading = [this](const String& reason)
			{
				m_loadingFailed = true;
				m_loadingError = reason;
				setLoadingStatus(U"ロード/生成中に例外が発生しました");
				DBG_LOG(U"[Loading] Exception task={} elapsed={:.1f}s reason={}"_fmt(
					m_loadingTask == LoadingTask::LoadGame ? U"load" : U"generate", m_loadingTimer.sF(), reason));
				if (getData().auditRoadIntegrity) { System::Exit(); }
			};
			try
			{
				m_generationFuture.get();
			}
			catch (const Error& error)
			{
				failLoading(error.what());
			}
			catch (const std::exception& error)
			{
				failLoading(Unicode::FromUTF8(error.what()));
			}
			catch (...)
			{
				failLoading(U"unknown exception");
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
					DebugLog::print(U"[Load] Failed ({:.1f}秒)"_fmt(m_loadingTimer.sF()));
					drawLoadingScreen(Clamp(m_genProgress.load(), 0.0f, 1.0f));
					return;
				}

				DebugLog::print(U"[Load] 完了 ({:.1f}秒)"_fmt(m_loadingTimer.sF()));
			}
			else
			{

				DebugLog::print(U"[Loading] 全パイプライン完了 ({:.1f}秒)"_fmt(m_loadingTimer.sF()));
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

			}

			// Apply an explicitly supplied initial benchmark view before preparing its assets.
			if (getData().playtest && !getData().playtestCommands.isEmpty())
			{
				const JSON command = JSON::Load(getData().playtestCommands);
				if (command && command.contains(U"action") && command[U"action"].getOr<String>(U"") == U"benchmark_view")
				{
					processPlaytestCommand(); m_world.update(m_camera.focusPoint());
				}
			}
			m_riverRenderer.build(m_world,m_network);
			m_tunnelRenderer.build(m_world,m_network,m_trainNetwork);
			m_worldRenderer.setTunnelOpenings(m_tunnelRenderer.openings);
			m_minimapRenderer.setGeography(m_world,m_districtHierarchy);
			m_minimapRenderer.buildTerrainTexture(m_world);
			m_minimapRenderer.updateRoadOverlay(m_network, m_world);

			for (const auto name : {Asset::Grass, Asset::Ground, Asset::Gravel, Asset::SparseGrass, Asset::Sand, Asset::CoastSand, Asset::Asphalt, Asset::Concrete})
			{
				(void)TextureAsset{name}.size();
			}
			if (!m_buildingModelsPreloaded) { m_worldRenderer.preloadBuildingModels(); m_buildingModelsPreloaded = true; }
			m_worldRenderer.setTransportSites(m_trainNetwork, m_network, m_world);
			m_worldRenderer.prepareWorld(m_world, m_network);
			m_roadRenderer.preloadFallbacks(m_network,m_world,m_camera.focusPoint());
			m_roadRenderer.preloadDetails(m_network,m_world,m_camera.eyePosition());
			FontAsset(Asset::Small16).preload(U"街の情報交通通知停止再開操作速度人口資金周辺地図混雑平均最大住宅充足率月次収支幸福度道路工事中同じタブを押すと閉じます0123456789×億円年月日時分−＋?…/.: ");
			String placeGlyphs=U"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz .,-()";
			for(const auto& town:m_districts) { placeGlyphs+=SettlementNames::name(town)+SettlementNames::reading(town); }
			for(const auto& area:m_districtHierarchy.areas) { placeGlyphs+=area.name+area.reading; }
			FontAsset(Asset::CJK24).preload(placeGlyphs);
			FontAsset(Asset::CJK14).preload(placeGlyphs);
			(void)m_cityLighting.initialize();
			m_roadRenderer.prepareSignTextures();
			m_roadRenderer.prepareGuideSignTextures(m_network);
			m_trainRenderer.preloadGeometry(m_trainNetwork,m_world);
			m_vehicleRenderer.preload();
			m_pedestrianRenderer.preload();
			m_minimapRenderer.preload(m_camera,m_network,m_trainNetwork,m_districts);
			m_trainRenderer.prepareFacilityTextures();
			String debugGlyphs = U"全体秒へ遷移ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789[]{}()=:_.,+-/<> ";
			for (const auto& entry : DebugLog::entries()) { debugGlyphs += entry.text; }
			FontAsset(Asset::Small16).preload(debugGlyphs);
			m_housingCapacity.update(m_world,WORLD_CHUNKS*WORLD_CHUNKS);
			if (m_loadingTask == LoadingTask::NewGame)
			{
				m_economy.initializeGeneratedCity(m_housingCapacity.capacity());
				DBG_LOG(U"[InitialEconomy] housing={} population={} grant={:.3f} maintenance={:.3f}"_fmt(
					m_housingCapacity.capacity(),m_economy.population,m_economy.monthlyGrant(),m_economy.roadMaintenanceCost(m_network)));
			}
			DebugLog::print(U"[Phase] 全体 {:.1f}秒 → Playing へ遷移"_fmt(m_loadingTimer.sF()));
			m_phase = GamePhase::Playing;
			return;
		}
	}

	drawLoadingScreen(Clamp(m_genProgress.load(), 0.0f, 1.0f));
}

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
