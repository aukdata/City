
#include "GameApp.hpp"

void GameApp::run()
{
	constexpr int kWindowWidth  = 1280;
	constexpr int kWindowHeight = 768;

	System::SetTerminationTriggers(UserAction::CloseButtonClicked);
	Window::Resize(kWindowWidth, kWindowHeight);
	Scene::SetBackground(ColorF{ 0.2, 0.3, 0.4 });
	Window::SetTitle(U"City Simulation");

	GameApp app;
	while (System::Update())
	{
		const double dt = Scene::DeltaTime();
		app.update(dt);
		app.render();
	}
}

GameApp::GameApp()
{
	// 深度バッファ付き MSAA レンダーテクスチャを作成する（Zバッファが機能するために必須）
	m_renderTexture = MSRenderTexture{ Scene::Size(), TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes };

	m_traffic.init(&m_network, &m_world, &m_zoneManager);
	m_trainManager.init(&m_trainNetwork);

	// 初期ネットワーク: 4チャンク（2048×2048）をまたぐ十字幹線 + 市街地格子 + 西側バイパス + 曲線住宅路
	// チャンク境界: x=1024（東西）, z=1024（南北）
	// ネットワーク中心: (1040, 0, 1040)
	{
		using NT = NodeType;
		const auto N = [&](float x, float z, NT t = NT::Intersection)
		{
			return m_network.addNode(Vec3{ x, 0.f, z }, t);
		};

		// ---- 幹線道路ノード（Arterial） ----
		// 東西幹線 (z≈1040 で chunk(0,x)〜chunk(1,x) をまたぐ)
		const int mW   = N(  80, 1040, NT::Endpoint);
		const int mW1  = N( 450, 1040);                // 西側交差点（chunk 0,0/0,1 内）
		const int mC   = N(1040, 1040);                // 中心（chunk 境界付近）
		const int mE1  = N(1630, 1040);                // 東側交差点（chunk 1,0/1,1 内）
		const int mE   = N(1980, 1040, NT::Endpoint);
		// 南北幹線 (x≈1040 で chunk(x,0)〜chunk(x,1) をまたぐ)
		const int mN   = N(1040,   80, NT::Endpoint);
		const int mN1  = N(1040,  450);                // 北側交差点
		const int mS1  = N(1040, 1630);                // 南側交差点
		const int mS   = N(1040, 1980, NT::Endpoint);

		// ---- 市街地格子コーナーノード（LocalRoad） ----
		// 各コーナーは異なるチャンクに分散
		const int gNW  = N( 450,  450);   // chunk (0,0)
		const int gNE  = N(1630,  450);   // chunk (1,0)
		const int gSW  = N( 450, 1630);   // chunk (0,1)
		const int gSE  = N(1630, 1630);   // chunk (1,1)

		// ---- 外周 LocalRoad 端点 ----
		const int epNN = N( 450,   80, NT::Endpoint);
		const int epNE = N(1630,   80, NT::Endpoint);
		const int epWN = N(  80,  450, NT::Endpoint);
		const int epWS = N(  80, 1630, NT::Endpoint);
		const int epEN = N(1980,  450, NT::Endpoint);
		const int epES = N(1980, 1630, NT::Endpoint);
		const int epSW = N( 450, 1980, NT::Endpoint);
		const int epSE = N(1630, 1980, NT::Endpoint);

		// ---- 曲線路端点（住宅街 + 郊外） ----
		const int rNE  = N(1880,  140, NT::Endpoint);  // NE郊外住宅端
		const int rSE  = N(1880, 1900, NT::Endpoint);  // SE郊外住宅端
		const int rNW  = N( 160,  160, NT::Endpoint);  // NW郊外住宅端（バイパス分岐）

		// ---- ヘルパー ----
		const auto P = [&](int id) { return m_network.getNode(id)->position; };

		const auto Straight = [&](int a, int b, RoadType rt, int nl)
		{
			const Vec3 pa = P(a), pb = P(b);
			m_network.addEdge(a, b, pa + (pb-pa)*(1.0/3), pa + (pb-pa)*(2.0/3), rt, nl);
		};
		const auto Curve = [&](int a, int b, Vec3 ca, Vec3 cb, RoadType rt, int nl)
		{
			m_network.addEdge(a, b, ca, cb, rt, nl);
		};

		// ==================================================
		// エッジ定義
		// ==================================================

		// ---- 東西幹線（Arterial 4 車線・直線）x=1024 をまたぐ ----
		Straight(mW,  mW1, RoadType::Arterial, 4);
		Straight(mW1, mC,  RoadType::Arterial, 4);   // chunk(0)→chunk(1) 横断
		Straight(mC,  mE1, RoadType::Arterial, 4);
		Straight(mE1, mE,  RoadType::Arterial, 4);

		// ---- 南北幹線（Arterial 4 車線）z=1024 をまたぐ ----
		Straight(mN,  mN1, RoadType::Arterial, 4);
		Straight(mN1, mC,  RoadType::Arterial, 4);   // chunk(0)→chunk(1) 横断
		Straight(mC,  mS1, RoadType::Arterial, 4);
		// 南部は緩やかに西へ流れるカーブ
		Curve(mS1, mS,
		      Vec3{1010,0,1710}, Vec3{1020,0,1900},
		      RoadType::Arterial, 4);

		// ---- 市街地格子 水平（LocalRoad 2 車線）x=1024 をまたぐ ----
		Straight(gNW, mN1, RoadType::LocalRoad, 2);   // chunk(0)→chunk(1)
		Straight(mN1, gNE, RoadType::LocalRoad, 2);   // chunk(1)
		Straight(gSW, mS1, RoadType::LocalRoad, 2);   // chunk(0)→chunk(1)
		Straight(mS1, gSE, RoadType::LocalRoad, 2);   // chunk(1)

		// ---- 市街地格子 垂直（LocalRoad 2 車線）z=1024 をまたぐ ----
		Straight(gNW, mW1, RoadType::LocalRoad, 2);   // chunk(0)
		Straight(mW1, gSW, RoadType::LocalRoad, 2);   // chunk(0)→chunk(1)
		Straight(gNE, mE1, RoadType::LocalRoad, 2);   // chunk(1)
		Straight(mE1, gSE, RoadType::LocalRoad, 2);   // chunk(1)→chunk(1)

		// ---- 外周 LocalRoad 端点接続（2 車線） ----
		Straight(epNN, gNW, RoadType::LocalRoad, 2);
		Straight(epNE, gNE, RoadType::LocalRoad, 2);
		Straight(epWN, gNW, RoadType::LocalRoad, 2);
		Straight(epWS, gSW, RoadType::LocalRoad, 2);
		Straight(gNE,  epEN, RoadType::LocalRoad, 2);
		Straight(gSE,  epES, RoadType::LocalRoad, 2);
		Straight(gSW,  epSW, RoadType::LocalRoad, 2);
		Straight(gSE,  epSE, RoadType::LocalRoad, 2);

		// ---- 西側バイパス（Arterial 4 車線・大きく西に膨らむ曲線）z=1024 をまたぐ ----
		// gNW(450,450) → 西方向に迂回 → gSW(450,1630)
		Curve(gNW, gSW,
		      Vec3{120,0,700}, Vec3{120,0,1380},
		      RoadType::Arterial, 4);

		// ---- NW 郊外住宅街曲線路（LocalRoad 2 車線） ----
		// epWN(80,450) → 北西へ弧を描いて → rNW(160,160)
		Curve(epWN, rNW,
		      Vec3{70,0,340}, Vec3{110,0,230},
		      RoadType::LocalRoad, 2);

		// ---- NE 郊外住宅街曲線路（LocalRoad 2 車線） ----
		// gNE(1630,450) → 北東へ弧を描いて → rNE(1880,140)
		Curve(gNE, rNE,
		      Vec3{1750,0,370}, Vec3{1845,0,250},
		      RoadType::LocalRoad, 2);

		// ---- SE 郊外住宅街曲線路（LocalRoad 2 車線） ----
		// gSE(1630,1630) → 南東へ弧を描いて → rSE(1880,1900)
		Curve(gSE, rSE,
		      Vec3{1760,0,1680}, Vec3{1850,0,1790},
		      RoadType::LocalRoad, 2);
	}

	// ---- 初期鉄道ネットワーク（南北幹線に沿った路線） ----
	{
		// 線路が通るチャンク (1,0) と (1,1) を事前生成して sampleHeight を有効にする
		m_world.getOrCreateChunk({ 1, 0 });
		m_world.getOrCreateChunk({ 1, 1 });

		// 駅を配置する（地形高さを考慮したY座標）
		const auto S = [&](float x, float z, const String& name) -> int
		{
			return m_trainNetwork.addStation(
				Vec3{ x, m_world.sampleHeight(x, z), z }, name);
		};
		const int sN   = S(1080,  150, U"北端駅");
		const int sN1  = S(1080,  480, U"北市駅");
		const int sC   = S(1080, 1060, U"中央駅");
		const int sS1  = S(1080, 1620, U"南市駅");
		const int sSt  = S(1080, 1960, U"南端駅");

		// 線路エッジ（1/3・2/3 の制御点で直線的に連結）
		const auto TE = [&](int a, int b)
		{
			const Vec3 pa = m_trainNetwork.getNode(a)->position;
			const Vec3 pb = m_trainNetwork.getNode(b)->position;
			m_trainNetwork.addEdge(a, b,
				pa + (pb - pa) * (1.0 / 3),
				pa + (pb - pa) * (2.0 / 3),
				120.0f);
		};
		TE(sN,  sN1);
		TE(sN1, sC);
		TE(sC,  sS1);
		TE(sS1, sSt);

		// ダイヤを設定する（南北を往復）
		TrainSchedule sched;
		sched.id = 0;
		sched.headwaySec = 300.0f;  // 5分間隔
		sched.loop = true;
		for (int nodeId : { sN, sN1, sC, sS1, sSt })
		{
			StopEntry stop;
			stop.stationNodeId = nodeId;
			stop.dwellSec = 20.0f;
			sched.stops << stop;
		}
		m_trainNetwork.addSchedule(sched);
	}

	// カメラをネットワーク中心（4チャンク交点付近）に移動する
	m_camera.setFocus(Vec3{ 1040, 0, 1040 });

	// 初期車両を生成する
	for (int i = 0; i < 20; ++i)
	{
		m_traffic.spawnVehicle();
	}

	// アクティブチャンクを確保
	m_world.update(m_camera.focusPoint());

	// 月次トリガーを現在の月に初期化（起動直後に月次が走らないよう）
	m_lastEconYear  = m_clock.year;
	m_lastEconMonth = m_clock.month;
}

void GameApp::update(double dt)
{
	m_clock.advance(dt);
	m_world.update(m_camera.focusPoint());
	m_camera.update(dt);
	m_traffic.update(dt, m_clock.now);
	m_trainManager.update(dt, m_clock.now);

	// 追従・一人称カメラ: 対象車両の位置・方向を設定する
	if (m_camera.mode() != CameraMode::Overview)
	{
		const auto& vehicles = m_traffic.vehicles();
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

	// イベントシステムを更新する
	m_eventSystem.update(m_clock.now, m_clock.month, dt);

	// 月が変わったら月次更新を実行する
	if (m_clock.year != m_lastEconYear || m_clock.month != m_lastEconMonth)
	{
		m_lastEconYear  = m_clock.year;
		m_lastEconMonth = m_clock.month;
		m_zoneManager.monthlyUpdate(m_world, m_network, m_clock.now, m_economy);
		m_eventSystem.rollMonthly(m_clock.now, m_clock.month, m_network);
	}

	// 新しいイベント通知を取得する
	for (const auto& n : m_eventSystem.popNewNotifications())
	{
		m_notifications << n;
		if (static_cast<int>(m_notifications.size()) > 5)
			m_notifications.erase(m_notifications.begin());
	}
}

void GameApp::render()
{
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
		// 深度テスト+書き込みをここで一括設定（Sky 等がステートを変えても以降で維持される）
		const ScopedRenderStates3D depthState{ DepthStencilState::DepthTestWrite };

		Graphics3D::SetCameraTransform(m_camera.camera3D());

		// 太陽・環境光を設定する
		const Vec3 sunDir = Vec3{ Math::Cos(t), sinT, 0.3 }.normalized();
		Graphics3D::SetSunDirection(sunDir);
		Graphics3D::SetGlobalAmbientColor(ColorF{ 0.55 + 0.30 * dayF + 0.10 * dawnF });

		// 空の色をブレンドする（夜→薄明→昼）
		const ColorF dayZenith  { 0.10, 0.35, 0.80 };
		const ColorF dawnZenith { 0.22, 0.18, 0.38 };
		const ColorF nightZenith{ 0.01, 0.02, 0.07 };
		m_sky.zenithColor  = nightZenith.lerp(dawnZenith, dawnF).lerp(dayZenith, dayF);

		const ColorF dayHorizon  { 0.60, 0.78, 0.95 };
		const ColorF dawnHorizon { 0.85, 0.42, 0.15 };
		const ColorF nightHorizon{ 0.02, 0.03, 0.10 };
		m_sky.horizonColor = nightHorizon.lerp(dawnHorizon, dawnF).lerp(dayHorizon, dayF);

		m_sky.starBrightness = Clamp(1.0 - dayF * 3.0 - dawnF * 2.0, 0.0, 1.0);
		m_sky.cloudTime = Scene::Time() * 0.015;
		m_sky.draw(exposure);

		// 地形・道路・建物・車両
		m_worldRenderer.render(m_world);
		m_roadRenderer.render(m_network, m_clock.now, m_world);
		m_zoneManager.renderOverlay(m_world);
		m_vehicleRenderer.render(m_traffic.vehicles());

		// 線路・列車
		m_trainRenderer.renderTracks(m_trainNetwork);
		m_trainRenderer.renderTrains(m_trainManager.trains());

		// 線路描画モードのカーソルプレビュー
		if (m_mode == EditMode::TrainDraw && m_cursorGroundPos)
		{
			Sphere{ *m_cursorGroundPos, 6.0f }.draw(ColorF{ 0.9, 0.85, 0.2, 0.8 }.removeSRGBCurve());
		}

		// バス停
		for (const auto& stop : m_traffic.busStops())
		{
			Cylinder{ stop.position, stop.position + Vec3{0,4,0}, 2.0 }.draw(ColorF{0.2, 0.5, 0.9}.removeSRGBCurve());
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

		m_debugRenderer.render(m_network, m_traffic.vehicles(), m_world, m_camera);
	}

	// 3D コマンドをフラッシュして MSAA を解決し、スクリーンに転送する
	Graphics3D::Flush();
	m_renderTexture.resolve();
	Shader::LinearToScreen(m_renderTexture);

	// ---- UI（2D）は レンダーテクスチャの外で描く ----
	m_uiRenderer.render(m_clock, m_traffic.vehicleCount(), modeString(), m_economy);

	{
		static const Font notifFont{ 13 };
		double y = 80.0;
		for (const auto& ev : m_eventSystem.activeEvents())
		{
			const RectF bg{ Scene::Width() - 360.0, y, 350.0, 42.0 };
			bg.draw(ColorF{ 0.05, 0.05, 0.25, 0.8 });
			bg.drawFrame(1.0, ColorF{ 0.4, 0.4, 0.8, 0.6 });
			notifFont(U"[!] " + ev.title).draw(Vec2{ Scene::Width() - 350.0, y + 4 }, ColorF{ 1.0, 0.9, 0.3 });
			notifFont(ev.description).draw(Vec2{ Scene::Width() - 350.0, y + 22 }, ColorF{ 0.85, 0.85, 0.85 });
			y += 48.0;
		}
	}
}

void GameApp::handleInput()
{
	// Tab: ゾーンオーバーレイ切替
	if (KeyTab.down())
	{
		m_zoneManager.showOverlay = !m_zoneManager.showOverlay;
	}

	// モード切り替え
	if (KeyR.down())
	{
		m_mode = (m_mode == EditMode::RoadDraw) ? EditMode::None : EditMode::RoadDraw;
		m_drawStartNode = none;
		m_rectStart = none;
	}
	if (KeyZ.down())
	{
		m_mode = (m_mode == EditMode::ZonePaint) ? EditMode::None : EditMode::ZonePaint;
		m_drawStartNode = none;
		m_rectStart = none;
	}

	// ゾーン種別選択（1〜6 キー）
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
		// 時刻速度切り替え（ゾーン塗りモード以外）
		if (Key1.down()) m_clock.speed = TimeSpeed::x1;
		if (Key2.down()) m_clock.speed = TimeSpeed::x2;
		if (Key3.down()) m_clock.speed = TimeSpeed::x4;
		if (Key0.down()) m_clock.speed = TimeSpeed::Paused;
	}

	// 車両生成
	if (KeyT.down())
	{
		m_traffic.spawnVehicle();
	}

	// F: カメラモード切り替え（俯瞰 / 追従 / 一人称）
	if (KeyF.down())
	{
		m_camera.cycleMode();
	}

	// G: 地形編集モード
	if (KeyG.down())
	{
		m_mode = (m_mode == EditMode::TerrainEdit) ? EditMode::None : EditMode::TerrainEdit;
		m_drawStartNode = none;
		m_rectStart = none;
	}

	// 地形編集モードのブラシサイズ変更（Ctrl + ホイール）
	if (m_mode == EditMode::TerrainEdit && KeyControl.pressed())
	{
		const double wheel = Mouse::Wheel();
		m_terrainBrushRadius = Clamp(
			m_terrainBrushRadius + static_cast<float>(wheel * -20.0),
			20.0f, 400.0f);
	}

	// X: 線路描画モード
	if (KeyX.down())
	{
		m_mode = (m_mode == EditMode::TrainDraw) ? EditMode::None : EditMode::TrainDraw;
		m_trainDrawStartNode = none;
	}

	// B: バス路線描画モード
	if (KeyB.down())
	{
		if (m_mode == EditMode::BusRouteDraw)
		{
			// 路線確定（2バス停以上あれば登録）
			m_mode = EditMode::None;
			m_editingRouteId = -1;
		}
		else
		{
			m_mode = EditMode::BusRouteDraw;
			// 新しい路線を開始する
			BusRoute newRoute;
			newRoute.headwaySec = 120.0f;
			m_editingRouteId = m_traffic.addBusRoute(newRoute);
		}
	}

	// 道路描画モード中の左クリック
	if (m_mode == EditMode::RoadDraw)
	{
		handleRoadDraw();
	}
	else if (m_mode == EditMode::ZonePaint)
	{
		handleZonePaint();
	}
	else if (m_mode == EditMode::BusRouteDraw)
	{
		handleBusRouteDraw();
	}
	else if (m_mode == EditMode::TerrainEdit)
	{
		handleTerrainEdit();
	}
	else if (m_mode == EditMode::TrainDraw)
	{
		handleTrainDraw();
	}
}

void GameApp::handleRoadDraw()
{
	if (MouseL.down() && m_cursorGroundPos)
	{
		// 近傍ノードを探す
		auto nearNode = m_network.findNodeNear(*m_cursorGroundPos, 20.0f);
		int nodeId;
		if (!nearNode)
		{
			nodeId = m_network.addNode(*m_cursorGroundPos);
		}
		else
		{
			nodeId = *nearNode;
		}

		if (!m_drawStartNode)
		{
			m_drawStartNode = nodeId;
		}
		else
		{
			int from = *m_drawStartNode;
			if (from != nodeId)
			{
				Vec3 pA = m_network.getNode(from)->position;
				Vec3 pB = m_network.getNode(nodeId)->position;
				Vec3 mid = (pA + pB) / 2.0;
				m_network.addEdgeWithIntersection(from, nodeId, mid, mid, RoadType::LocalRoad, 2);
				m_traffic.markNetworkDirty();
			}
			m_drawStartNode = nodeId;
		}
	}
}

void GameApp::handleZonePaint()
{
	if (!m_cursorGroundPos) return;

	if (KeyShift.pressed())
	{
		// Shift + 左クリック ドラッグ: 矩形塗り
		if (MouseL.down())
		{
			m_rectStart = m_cursorGroundPos;
		}
		if (MouseL.up() && m_rectStart)
		{
			m_zoneManager.paintZoneRect(m_world, *m_rectStart, *m_cursorGroundPos, m_paintZone);
			m_rectStart = none;
		}
	}
	else
	{
		m_rectStart = none;
		// 左クリック / ドラッグ: ブラシ塗り
		if (MouseL.pressed())
		{
			m_zoneManager.paintZone(m_world, *m_cursorGroundPos, m_paintZone, 2);
		}
	}
}

void GameApp::handleBusRouteDraw()
{
	if (!m_cursorGroundPos) return;
	if (m_editingRouteId < 0) return;

	if (MouseL.down())
	{
		// バス停を追加する
		BusStop stop;
		stop.position = *m_cursorGroundPos;

		// 近傍エッジを探す
		float bestDist = 30.0f;
		for (const auto& edge : m_network.edges())
		{
			if (edge.id < 0) continue;
			if (const auto bez = m_network.getBezier(edge.id))
			{
				// ベジェの中点との距離で簡易判定
				const Vec3 mid = bez->positionAt(bez->totalLength * 0.5f);
				const float d = static_cast<float>(stop.position.distanceFrom(mid));
				if (d < bestDist)
				{
					bestDist = d;
					stop.edgeId = edge.id;
					stop.arcPos = edge.length * 0.5f;  // 簡易: エッジ中点
				}
			}
		}

		const int stopId = m_traffic.addBusStop(stop);
		m_traffic.addStopToRoute(m_editingRouteId, stopId);
	}
}

void GameApp::handleTerrainEdit()
{
	if (!m_cursorGroundPos) return;

	const float dt    = static_cast<float>(Scene::DeltaTime());
	const float raise = MouseL.pressed() ? m_terrainBrushStrength * dt : 0.0f;
	const float lower = MouseR.pressed() ? m_terrainBrushStrength * dt : 0.0f;
	const float delta = raise - lower;
	if (delta == 0.0f) return;

	const Vec3 center = *m_cursorGroundPos;

	// アクティブチャンクのハイトマップを編集する
	for (Chunk* chunk : m_world.getActiveChunks())
	{
		if (!chunk) continue;

		constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / HEIGHT_CELLS;
		const Vec3 origin = chunk->worldOrigin();

		bool modified = false;
		for (int row = 0; row <= HEIGHT_CELLS; ++row)
		{
			for (int col = 0; col <= HEIGHT_CELLS; ++col)
			{
				const Vec3 vpos = origin + Vec3{ col * cellSize, 0.0, row * cellSize };
				const double dist = Vec2{ vpos.x, vpos.z }.distanceFrom(Vec2{ center.x, center.z });
				if (dist > m_terrainBrushRadius) continue;

				// ブラシの影響は中心ほど強い（コサイン減衰）
				const float t = static_cast<float>(dist / m_terrainBrushRadius);
				const float weight = static_cast<float>(Math::Cos(t * Math::Pi / 2.0));
				chunk->heightMap[{ col, row }] = Clamp(
				chunk->heightMap[{ col, row }] + delta * weight,
				-200.0f, 350.0f);  // カメラ eye 高さ (~386m) を超えないよう制限
				modified = true;
			}
		}

		if (modified)
		{
			chunk->dirty = true;
		}
	}
}

void GameApp::handleTrainDraw()
{
	if (!m_cursorGroundPos) return;

	if (MouseL.down())
	{
		// 近傍ノードを探す（20m 以内）
		Optional<int> nearNode;
		float bestDist = 20.0f;
		for (const auto& node : m_trainNetwork.nodes())
		{
			const float d = static_cast<float>(node.position.distanceFrom(*m_cursorGroundPos));
			if (d < bestDist)
			{
				bestDist = d;
				nearNode = node.id;
			}
		}

		// ノードがなければ駅を新設する
		int nodeId;
		if (!nearNode)
		{
			nodeId = m_trainNetwork.addStation(*m_cursorGroundPos, U"駅");
		}
		else
		{
			nodeId = *nearNode;
		}

		if (!m_trainDrawStartNode)
		{
			m_trainDrawStartNode = nodeId;
		}
		else
		{
			const int from = *m_trainDrawStartNode;
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

	// 右クリックで描画を中断する
	if (MouseR.down())
		m_trainDrawStartNode = none;
}

void GameApp::updateCursor()
{
	m_cursorGroundPos = m_camera.screenToGround(Vec2(Cursor::Pos()));
}

String GameApp::modeString() const
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
	default:
		return U"";
	}
}
