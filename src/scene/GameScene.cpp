#include "GameScene.hpp"
#include "../ui/RoadPlanInput.hpp"
#include "../render/ShaderAsset.hpp"
#include "../render/RenderQuality.hpp"
#include "../ui/ZonePalette.hpp"

// =============================================================================
// 初期化
// =============================================================================

GameScene::GameScene(const InitData& init)
	: IScene{ init }
{
	m_worldRenderer.setAsyncTerrain(!getData().syncTerrain);
	constexpr double kRoadCacheBuildBudgetMs = 4.0;
	m_roadRenderer.setCacheBuildBudget(getData().syncRoads ? Math::Inf : kRoadCacheBuildBudgetMs);
	m_camera.setWalkSurface([this](Vec3 point)
	{
		return m_walkSurface.resolve(point,m_world,m_network,m_trainNetwork);
	});
	(void)m_worldRenderer.setRenderDistance(getData().renderDistance);
	const Size renderSize = RenderQuality::targetSize(Scene::Size(), getData().lowSpec);
	if (getData().lowSpec)
	{
		m_lowSpecRenderTexture = RenderTexture{ renderSize, TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes };
	}
	else
	{
		m_renderTexture = MSRenderTexture{ renderSize, TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes };
	}
	DebugLog::print(U"[RenderQuality] profile={} target={} native={} msaa={} shadows={}"_fmt(
		getData().lowSpec ? U"low-spec" : U"normal", renderSize, Scene::Size(), !getData().lowSpec, !getData().lowSpec));
	m_outlineMask   = RenderTexture{ Scene::Size(), TextureFormat::R8G8B8A8_Unorm,      HasDepth::Yes };
	m_outlinePS     = ShaderAsset::pixel(U"shaders/hlsl/selection_outline.hlsl", U"PS");
	if (not m_outlinePS)
	{
		DebugLog::print(U"[WARN] selection_outline.hlsl load failed");
	}
	// 自動鉄道を外した街でも、後から引く線路は共通の道路網を使う。
	m_trainNetwork.bind(&m_network);
	m_trainManager.init(&m_trainNetwork);

	m_roadRenderer.loadAssets();

	m_sandboxActive = getData().sandboxMode;

	initScene();
}

GameScene::~GameScene()
{
	GameInput::releaseTextFocus();
	m_simThread.stop();

	if (m_generationFuture.valid())
		m_generationFuture.wait();
}

void GameScene::initScene()
{
	// 初回の地図・入力判定も、描画を待たず確定したHUD領域を使う。
	m_uiRenderer.updateLayout();
	// シーン全体で使う UI パネルと、編集系の初期テンプレートをここでまとめて準備する。
	m_panelManager.registerPanel(U"edge_info", Vec2{374, static_cast<double>(Scene::Height() - 20)}, true, true);
	m_panelManager.registerPanel(U"node_info", Vec2{312, static_cast<double>(Scene::Height() - 20)}, true, true);
	m_panelManager.registerPanel(U"name_list", Vec2{250, static_cast<double>(Scene::Height() - 20)}, false, true);
	m_panelManager.registerPanel(U"vehicle_info", Vec2{280, static_cast<double>(Scene::Height() - 20)}, true, true);
	m_panelManager.registerPanel(U"land_info",Vec2{320,190},true,true);
	m_panelManager.registerPanel(U"rail_info", Vec2{RailInfoPanel::kWidth, 280}, true, true);
	m_panelManager.registerPanel(U"zone_palette",Vec2{ZonePalette::kWidth,ZonePalette::kHeight+24},true,true);
	m_panelManager.registerPanel(U"rail_timetable",Vec2{TrainTimetableEditor::kWidth,Min(TrainTimetableEditor::kHeight,Scene::Height()-40)},true,true);
	m_panelManager.registerPanel(U"building_info", Vec2{320, static_cast<double>(Scene::Height() - 20)}, true, true);
	m_panelManager.registerPanel(U"draw_template", Vec2{374, static_cast<double>(Scene::Height() - 20)}, true, true);

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


// =============================================================================
// SimThread 起動
// =============================================================================

void GameScene::startSimThread()
{
	m_simGraph = std::make_shared<const SimGraph>(SimGraph::build(m_network));
	m_vehicleManager.init(*m_simGraph, m_network, &m_world);
	m_trainManager.enablePassengerEvents();
	m_pedestrianManager.setCarsEnabled(getData().generation.enabled(GenerationOptions::Element::Cars));
	if (getData().generation.enabled(GenerationOptions::Element::Pedestrians))
	{
		m_pedestrianManager.initialize(m_world, m_network, m_vehicleManager.buildingAccess(), m_trainNetwork);
	}
	m_citySnapshot = collectCitySnapshot(m_world, m_network, m_vehicleManager.vehicles());
	m_simThread.start(m_simGraph);
}


// =============================================================================
// 毎フレーム更新
// =============================================================================

void GameScene::update()
{
	m_frameRateGraph.sampleNow();
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
	if (getData().captureTransportObjects) { updateTransportObjectsReview(); return; }
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

	if (getData().benchmarkNavigation) { updateNavigationBenchmark(); return; }
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
	m_soundEffects.setVolume(getData().effectVolume);
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
		const double simulationDt = dt * m_clock.speedMultiplier();
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
			// 自動発生だけを止め、手動運転や明示的に配置した車両の更新は継続する。
			auto demand = calculateTrafficDemand(m_economy.population, m_citySnapshot, m_clock.hour);
			if (!getData().generation.enabled(GenerationOptions::Element::Cars))
			{
				demand.targetVehicleCount = 0;
			}
			m_vehicleManager.setTrafficDemand(demand);
			m_vehicleManager.setTrafficFocus(m_camera.focusPoint());
			m_vehicleManager.update(simulationDt, m_clock.now, *m_simGraph,
			                        m_network, m_roadRenderer.visibleEdges());
		}

		m_trainManager.update(simulationDt, m_clock.now);
		if (m_simGraph && getData().generation.enabled(GenerationOptions::Element::Pedestrians))
		{
			// 乗物の到着を確定してから、降車・徒歩・次の乗車へ進める。
			m_pedestrianManager.update(simulationDt, m_camera.eyePosition(), m_world, m_network,
				*m_simGraph, m_vehicleManager, m_trainNetwork, m_trainManager);
		}
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
		updateZoneDevelopment(simulationDt);
	}

	// 経路リクエストはゲーム内時間停止中でも送信する
	for (auto& req : m_vehicleManager.collectRequests())
		m_simThread.pushRequest(std::move(req));

	// メインスレッドのロジック
	const Stopwatch swLogic{ StartImmediately::Yes };
	m_world.update(m_camera.focusPoint());
	if (m_selectedLandParcel && !m_panelManager.isVisible(U"land_info")) { clearSelection(); }
	if (m_mode==EditMode::ZonePaint && !m_panelManager.isVisible(U"zone_palette")) { setZonePaintMode(false); }
	if (m_mode!=EditMode::ZonePaint && m_panelManager.isVisible(U"zone_palette")) { setZonePaintMode(false); }
	m_minimapRenderer.setSmallBounds(m_uiRenderer.minimapBounds());
	handleGlobalShortcuts();
	if (!m_showPauseMenu && (!GameInput::keyboardBlocked()))
	{
		if (const auto target=m_minimapRenderer.update(m_camera,m_network,m_trainNetwork,m_districts))
		{
			jumpToMapPosition(*target);
		}
	}
	if (m_railTimetableWasVisible && !m_panelManager.isVisible(U"rail_timetable"))
	{
		GameInput::releaseTextFocus(); m_railTimetableWasVisible = false;
	}
	const bool mapInput=m_minimapRenderer.consumedInput();
	const bool layerHover =
		!mapInput && StartScreenControls::layerButton(Scene::Size(), m_frameRateGraph.visible).contains(Cursor::PosF());
	if (layerHover && MouseL.down() && !m_showPauseMenu && !GameInput::keyboardBlocked() &&
		!m_panelManager.blocksMouseInput())
	{
		toggleUnderground();
		GameInput::textOwnedFrame = true;
	}
	if (!m_showPauseMenu && !mapInput)
		m_panelManager.handleInput();
	m_uiRenderer.updateLayout(m_camera.mode() == CameraMode::FirstPerson,m_driving.active(),!modeString().isEmpty());
	const auto hudAction = m_uiRenderer.handleInput(!m_showPauseMenu && !mapInput
		&& !m_panelManager.blocksMouseInput() && !GameInput::keyboardBlocked());
	// HUD操作はゲーム側で適用する。描画処理から時計を変更せず、入力欄・地図・ポーズを優先する。
	if (hudAction == CityHud::Action::TogglePause)
	{
		if (m_clock.speed == TimeSpeed::Paused) { m_clock.speed = m_driving.active() ? TimeSpeed::x1 : m_prevSpeed; }
		else { m_prevSpeed = m_clock.speed; m_clock.speed = TimeSpeed::Paused; }
	}
	else if (hudAction == CityHud::Action::NextSpeed)
	{
		m_clock.speed = m_clock.speed == TimeSpeed::x1 ? TimeSpeed::x2 : m_clock.speed == TimeSpeed::x2 ? TimeSpeed::x4 : TimeSpeed::x1;
		m_prevSpeed = m_clock.speed;
	}
	m_uiRenderer.updateLayout(m_camera.mode() == CameraMode::FirstPerson,m_driving.active(),!modeString().isEmpty());
	m_minimapRenderer.setSmallBounds(m_uiRenderer.minimapBounds());
	if (!m_commandPalette.visible && MouseL.down() && GameInput::textInput) { GameInput::releaseTextFocus(); }
	if (MouseL.down() && m_panelManager.isMouseOnAnyPanel()) { GameInput::textOwnedFrame = true; }
	const bool textFocused=GameInput::keyboardBlocked();
	const bool commandChord=GameInput::pressed(KeyControl) &&
		(GameInput::pressed(KeyShift) || GameInput::pressed(KeyZ) || GameInput::pressed(KeyY) || GameInput::pressed(KeyR));
	m_camera.setKeyboardBlocked(mapInput || m_showPauseMenu || textFocused || commandChord);
	m_camera.setBlockInput(mapInput || m_showPauseMenu || textFocused || m_panelManager.isMouseOnAnyPanel() ||
						   m_uiRenderer.isMouseOnHud() || layerHover);
	if (!mapInput) { m_camera.update(dt, m_world); }
	m_logicMs = swLogic.msF();

	// 車両追跡
	if (!mapInput && !m_showPauseMenu && !textFocused && m_trackingVehicle && m_selectedVehicleId)
	{
		if (GameInput::pressed(KeyW) || GameInput::pressed(KeyA) || GameInput::pressed(KeyS) || GameInput::pressed(KeyD))
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

	if (m_selection.kind == SelectionKind::Train)
	{
		const auto found = std::find_if(m_trainManager.trains().begin(), m_trainManager.trains().end(),
			[&](const Train& train) { return train.id == m_selection.id; });
		if (found == m_trainManager.trains().end() || !m_panelManager.isVisible(U"rail_info"))
		{
			clearSelection();
		}
		else if (m_trackingTrain && !mapInput && !m_showPauseMenu && !textFocused)
		{
			if (GameInput::pressed(KeyW) || GameInput::pressed(KeyA) || GameInput::pressed(KeyS) ||
				GameInput::pressed(KeyD))
			{
				m_trackingTrain = false;
			}
			else
			{
				m_camera.setFocus(RailInfoPanel::followPosition(*found, m_trainNetwork));
			}
		}
	}
	if (m_selection.kind == SelectionKind::Station && !m_panelManager.isVisible(U"rail_info"))
	{
		clearSelection();
	}

	if (m_selectedVehicleId && !m_panelManager.isVisible(U"vehicle_info"))
	{
		m_selectedVehicleId = none;
		m_trackingVehicle = false;
	}

	RoadPlanInput::dispatchFrame(!mapInput, MouseL.pressed(), m_draftRoadPlan.draggedPoint,
		m_draftRoadPlan.dragPoints, [&]
	{
		if (!m_driving.active()) { updateCursor(); }
		handleInput();
		if (!m_driving.active()) { m_debugRenderer.handleInput(); }
	});
	updateDriving(dt,mapInput);

	if (getData().playtest && !getData().playtestCommands.isEmpty() && m_playtestFrame%15==0) { processPlaytestCommand(); }
	renderWorld();
	if (getData().playtest) { recordPlaytestFrame(); }
}
void GameScene::jumpToMapPosition(Vec2 target)
{
	leaveDriving(true);
	m_trackingVehicle=false;
	m_trackingTrain = false;
	target.x=Clamp(target.x,.5,static_cast<double>(WORLD_SIZE)-.5); target.y=Clamp(target.y,.5,static_cast<double>(WORLD_SIZE)-.5);
	m_camera.setFocus({target.x,
		m_world.sampleHeight(static_cast<float>(target.x), static_cast<float>(target.y)) - (m_underground ? 20 : 0),
		target.y});
}
