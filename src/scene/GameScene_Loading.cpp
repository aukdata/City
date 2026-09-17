#include <exception>
#include "GameScene.hpp"
#include "../asset/AssetRegistrar.hpp"

/// @file
/// @brief ロード状態の管理。生成処理の完了を確認してから通常更新へ遷移する。

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
	m_network.recomputeAllAutoSigns();
	m_roadRenderer.setMunicipalityLookup([this](Vec2 point)
	{
		const int id=m_districtHierarchy.at(point,0);
		return id>=0 ? m_districtHierarchy.areas[id].name : U"";
	});
		applyZonesGlobal();
		placeInitialBuildings();
		m_trainNetwork.synchronize();
	});
	Logger << U"[Loading] {} チャンク生成開始"_fmt(m_totalInitChunks);
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
			m_roadRenderer.preloadFallbacks(m_network,m_world);
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
			m_roadRenderer.preloadFallbacks(m_network,m_world);
			m_housingCapacity.update(m_world,WORLD_CHUNKS*WORLD_CHUNKS);
			m_economy.initializeGeneratedCity(m_housingCapacity.capacity());
			DBG_LOG(U"[InitialEconomy] housing={} population={} grant={:.3f} maintenance={:.3f}"_fmt(
				m_housingCapacity.capacity(),m_economy.population,m_economy.monthlyGrant(),m_economy.roadMaintenanceCost(m_network)));
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
