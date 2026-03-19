
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

	// 道路スタイルをロードする
	m_roadRenderer.loadStyle(U"assets/styles/road.toml");

	// シード入力の初期値を設定する
	m_seedTextState.text = U"20260316";
}

void GameApp::initWorld()
{
	// MapGenerator でマップを手続き生成する
	MapGenerator gen;
	const auto genResult = gen.generate(
		m_selectedSeed, m_selectedTerrain,
		m_world, m_network, m_zoneManager, m_trainNetwork);

	// カメラを都市核に移動する
	m_camera.setFocus(genResult.cameraFocus);

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
	if (m_gameState == GameState::Title)
		return;

	m_clock.advance(dt);
	m_world.update(m_camera.focusPoint());
	m_camera.update(dt, m_world);
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
	if (m_gameState == GameState::Title)
	{
		renderTitle();
		return;
	}

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
		m_zoneManager.showOverlay = (m_mode == EditMode::ZonePaint);
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
	// レイと地形ハイトマップの交点をレイマーチングで求める
	const Ray ray = m_camera.screenToRay(Vec2{ Cursor::Pos() });
	const Float3 orig = ray.origin;
	const Float3 dir  = ray.direction;

	// 下方向成分がなければ地形に当たらない
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
			// tPrev〜t の間で二分探索
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

	// 地形に当たらなかった場合は y=0 平面にフォールバック
	if (!hitFound)
		m_cursorGroundPos = m_camera.screenToGround(Vec2{ Cursor::Pos() });
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

// ─────────────────────────────────────────────────────────────────────────────
// タイトル画面（仕様書 §1: 生成パラメータ選択）
// ─────────────────────────────────────────────────────────────────────────────

void GameApp::renderTitle()
{
	static const Font titleFont { 46, Typeface::Bold };
	static const Font subFont   { 16 };
	static const Font labelFont { 17 };
	static const Font cardFont  { 17, Typeface::Bold };
	static const Font descFont  { 13 };

	const int W = Scene::Width();
	const int H = Scene::Height();

	// ---- 背景 ----
	Rect{ 0, 0, W, H }.draw(ColorF{ 0.07, 0.11, 0.16 });

	// ---- タイトル ----
	titleFont(U"City Simulation").drawAt(W * 0.5, 80, ColorF{ 0.90, 0.95, 1.00 });
	subFont(U"プロシージャル都市生成シミュレーター").drawAt(W * 0.5, 135, ColorF{ 0.52, 0.63, 0.74 });

	// ---- シード値 ----
	labelFont(U"シード値").draw(Vec2{ 200, 195 }, ColorF{ 0.78, 0.86, 0.93 });
	SimpleGUI::TextBox(m_seedTextState, Vec2{ 200, 222 }, 280);

	// テキストボックスの内容を uint64 に変換する
	{
		uint64 val = 0;
		bool valid = !m_seedTextState.text.isEmpty();
		for (char32 c : m_seedTextState.text)
		{
			if (c >= U'0' && c <= U'9')
				val = val * 10 + (c - U'0');
			else { valid = false; break; }
		}
		if (valid)
			m_selectedSeed = val;
	}

	if (SimpleGUI::Button(U"ランダム", Vec2{ 498, 222 }, 120))
	{
		m_selectedSeed = static_cast<uint64>(Random(10000000, 99999999));
		m_seedTextState.text = Format(m_selectedSeed);
	}

	// ---- 地形タイプ選択（仕様書 §2）----
	labelFont(U"地形タイプ").draw(Vec2{ 200, 286 }, ColorF{ 0.78, 0.86, 0.93 });

	struct TerrainInfo
	{
		TerrainType type;
		String      name;
		String      kana;
		String      desc;
		ColorF      baseColor;
	};
	static const TerrainInfo kInfos[] = {
		{ TerrainType::Basin,
		  U"山間盆地",   U"さんかんぼんち",
		  U"四方を山に囲まれた盆地\n川が中央を縦断する",
		  ColorF{ 0.17, 0.32, 0.25 } },
		{ TerrainType::Coastal,
		  U"沿岸平野",   U"えんがんへいや",
		  U"片側が海、反対側が山地\n港町が核になる",
		  ColorF{ 0.08, 0.24, 0.40 } },
		{ TerrainType::RiverFan,
		  U"河川扇状地", U"かせんせんじょうち",
		  U"山から流れ出る扇状地\n橋が重要インフラになる",
		  ColorF{ 0.30, 0.26, 0.10 } },
		{ TerrainType::Hills,
		  U"丘陵台地",   U"きゅうりょうだいち",
		  U"緩やかな丘が連続する地形\n集落は丘の上に分散する",
		  ColorF{ 0.16, 0.30, 0.16 } },
	};

	constexpr int kCardW = 238, kCardH = 160, kGap = 18;
	const int totalW    = 4 * kCardW + 3 * kGap;
	const int cardStartX = (W - totalW) / 2;
	const int cardY      = 318;

	for (int i = 0; i < 4; ++i)
	{
		const int         cx      = cardStartX + i * (kCardW + kGap);
		const RoundRect   card    { static_cast<double>(cx), static_cast<double>(cardY),
		                            static_cast<double>(kCardW), static_cast<double>(kCardH), 8.0 };
		const bool        selected = (kInfos[i].type == m_selectedTerrain);

		// 背景：選択中は明るく
		card.draw(selected ? kInfos[i].baseColor * 2.2 : kInfos[i].baseColor);
		card.drawFrame(2.5, selected
			? ColorF{ 1.0, 0.88, 0.25 }
			: ColorF{ 0.28, 0.34, 0.42 });

		// クリックで選択
		if (card.leftClicked())
			m_selectedTerrain = kInfos[i].type;

		// テキスト
		cardFont(kInfos[i].name).draw(Arg::topLeft = Vec2{ cx + 14, cardY + 12 },
		                              ColorF{ 0.95, 0.95, 0.95 });
		descFont(kInfos[i].kana).draw(Arg::topLeft = Vec2{ cx + 14, cardY + 38 },
		                              ColorF{ 0.62, 0.70, 0.77 });
		descFont(kInfos[i].desc).draw(Arg::topLeft = Vec2{ cx + 14, cardY + 60 },
		                              ColorF{ 0.80, 0.85, 0.87 });
		if (selected)
		{
			descFont(U"✓ 選択中").draw(Arg::topLeft = Vec2{ cx + 14, cardY + 128 },
			                           ColorF{ 1.0, 0.88, 0.25 });
		}
	}

	// ---- 生成開始ボタン ----
	constexpr int kBtnW = 220;
	if (SimpleGUI::Button(U"生成開始", Vec2{ (W - kBtnW) / 2, 530 }, kBtnW))
	{
		initWorld();
		m_gameState = GameState::Playing;
	}

	// ---- 操作説明 ----
	descFont(U"生成後: WASD/QE カメラ移動　R 道路　Z ゾーン　G 地形編集　X 線路　B バス　F カメラ切替").drawAt(
		W * 0.5, H - 36, ColorF{ 0.42, 0.50, 0.58 });
}
