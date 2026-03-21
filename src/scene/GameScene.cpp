#include "GameScene.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// 初期化
// ─────────────────────────────────────────────────────────────────────────────

GameScene::GameScene(const InitData& init)
	: IScene{ init }
{
	m_renderTexture = MSRenderTexture{ Scene::Size(), TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes };

	m_traffic.init(&m_network, &m_world, &m_zoneManager);
	m_trainManager.init(&m_trainNetwork);

	m_roadRenderer.loadStyle(U"assets/roads");

	m_sandboxActive = getData().sandboxMode;

	initWorld();
}

void GameScene::initWorld()
{
	MapGenerator gen;
	const auto genResult = gen.generate(
		getData().seed, getData().terrain,
		m_world, m_network, m_zoneManager, m_trainNetwork);

	m_camera.setFocus(genResult.cameraFocus);
	m_placeNames   = std::move(genResult.placeNames);
	m_settlements  = gen.settlements();

	for (int i = 0; i < 20; ++i)
		m_traffic.spawnVehicle();

	m_world.update(m_camera.focusPoint());
	m_world.popNewChunks();   // initWorld 生成分をクリア（リージョン(0,0)は既に登録済み）

	// リージョン(0,0) を生成済みとして登録
	m_generatedRegions[regionKey({ 0, 0 })] = true;

	m_lastEconYear  = m_clock.year;
	m_lastEconMonth = m_clock.month;
}

// ─────────────────────────────────────────────────────────────────────────────
// 無限ワールド: 未生成リージョンの地区・道路を動的追加する
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::checkAndGenerateRegions()
{
	const Array<Point> newChunks = m_world.popNewChunks();
	if (newChunks.isEmpty()) return;

	for (const Point& chunk : newChunks)
	{
		const Point region = chunkToRegion(chunk);
		const int64 key    = regionKey(region);

		if (m_generatedRegions.contains(key)) continue;
		m_generatedRegions[key] = true;

		const Vec2 offset = regionToWorldOffset(region);
		MapGenerator gen;
		gen.generateRegion(offset, getData().seed, m_world, m_network, m_zoneManager);

		// 新リージョンの地区を m_settlements に追加（地名レンダリング用）
		for (const auto& s : gen.settlements())
			m_settlements << s;

		// 道路ポスト処理（新規追加分を含む全体に適用）
		m_network.resolveIntersections();
		m_network.removeDuplicateEdges(getData().seed ^ static_cast<uint64>(key));
		m_roadRenderer.markTopologyChanged();
		m_traffic.markNetworkDirty();
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// 毎フレーム更新（ロジック + 描画）
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::update()
{
	const double dt = Scene::DeltaTime();

	// ---- ロジック ----
	m_clock.advance(dt);

	// 車両・列車の物理 dt はゲーム速度に比例させる
	// speedMultiplier() = 0/60/120/240 → /60.0 で 0/1/2/4 倍率になる
	const double physicsDt = dt * m_clock.speedMultiplier() / 60.0;

	m_world.update(m_camera.focusPoint());
	checkAndGenerateRegions();
	m_camera.update(dt, m_world);
	m_traffic.update(physicsDt, m_clock.now);
	m_trainManager.update(physicsDt, m_clock.now);

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

	m_eventSystem.update(m_clock.now, m_clock.month, dt);

	if (m_clock.year != m_lastEconYear || m_clock.month != m_lastEconMonth)
	{
		m_lastEconYear  = m_clock.year;
		m_lastEconMonth = m_clock.month;
		m_zoneManager.monthlyUpdate(m_world, m_network, m_clock.now, m_economy);
		m_eventSystem.rollMonthly(m_clock.now, m_clock.month, m_network);
	}

	for (const auto& n : m_eventSystem.popNewNotifications())
	{
		m_notifications << n;
		if (static_cast<int>(m_notifications.size()) > 5)
			m_notifications.erase(m_notifications.begin());
	}

	// ---- 描画 ----
	renderWorld();
}

// ─────────────────────────────────────────────────────────────────────────────
// 描画
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::renderWorld()
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

		m_worldRenderer.render(m_world);
		m_roadRenderer.render(m_network, m_clock.now, m_world);
		m_zoneManager.renderOverlay(m_world);
		m_vehicleRenderer.render(m_traffic.vehicles());

		m_trainRenderer.renderTracks(m_trainNetwork);
		m_trainRenderer.renderTrains(m_trainManager.trains());

		if (m_mode == EditMode::TrainDraw && m_cursorGroundPos)
		{
			Sphere{ *m_cursorGroundPos, 6.0f }.draw(ColorF{ 0.9, 0.85, 0.2, 0.8 }.removeSRGBCurve());
		}

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

		if (m_mode == EditMode::SandboxEdit)
		{
			const Vec2 cur2D = m_cursorGroundPos
				? Vec2{ m_cursorGroundPos->x, m_cursorGroundPos->z } : Vec2{ 0, 0 };

			// 制御点ハンドルを表示（ノードとの接線ライン + 小球）
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
				    m_sandboxDragCtrl->edgeId == edge.id && m_sandboxDragCtrl->isA;
				const bool dragB = m_sandboxDragCtrl &&
				    m_sandboxDragCtrl->edgeId == edge.id && !m_sandboxDragCtrl->isA;

				// nodeA ↔ ctrlA のハンドルライン
				const ColorF colA = dragA
				    ? ColorF{ 1.0, 0.5, 0.0, 1.0 }
				    : (isHovCtrl(edge.ctrlA) ? ColorF{ 1.0, 1.0, 0.3, 0.9 }
				                             : ColorF{ 0.2, 0.9, 0.4, 0.7 });
				Line3D{ nA->position + Vec3{0,2,0}, edge.ctrlA + Vec3{0,2,0} }
					.draw(ColorF{ 0.5, 0.5, 0.5, 0.5 }.removeSRGBCurve());
				Sphere{ edge.ctrlA + Vec3{0,2,0}, dragA ? 6.0 : 4.0 }
					.draw(colA.removeSRGBCurve());

				// nodeB ↔ ctrlB のハンドルライン
				const ColorF colB = dragB
				    ? ColorF{ 1.0, 0.5, 0.0, 1.0 }
				    : (isHovCtrl(edge.ctrlB) ? ColorF{ 1.0, 1.0, 0.3, 0.9 }
				                             : ColorF{ 0.2, 0.9, 0.4, 0.7 });
				Line3D{ nB->position + Vec3{0,2,0}, edge.ctrlB + Vec3{0,2,0} }
					.draw(ColorF{ 0.5, 0.5, 0.5, 0.5 }.removeSRGBCurve());
				Sphere{ edge.ctrlB + Vec3{0,2,0}, dragB ? 6.0 : 4.0 }
					.draw(colB.removeSRGBCurve());
			}

			// ノードを球で表示
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

		m_debugRenderer.render(m_network, m_traffic.vehicles(), m_world, m_camera);
	}

	Graphics3D::Flush();
	m_renderTexture.resolve();
	Shader::LinearToScreen(m_renderTexture);

	// ---- UI（2D）----
	m_placeNameRenderer.render(m_settlements, m_camera, m_world);
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

// ─────────────────────────────────────────────────────────────────────────────
// 入力処理
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::handleInput()
{
	// Space: 一時停止 / 直前の速度に復帰
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

	// Esc: 編集モードを抜ける
	if (KeyEscape.down() && m_mode != EditMode::None)
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
		// 1/2/3 キーで速度変更：Space 復帰用に直前速度を更新する
		if (Key1.down()) { m_prevSpeed = TimeSpeed::x1; m_clock.speed = TimeSpeed::x1; }
		if (Key2.down()) { m_prevSpeed = TimeSpeed::x2; m_clock.speed = TimeSpeed::x2; }
		if (Key3.down()) { m_prevSpeed = TimeSpeed::x4; m_clock.speed = TimeSpeed::x4; }
		if (Key0.down()) { m_prevSpeed = m_clock.speed != TimeSpeed::Paused ? m_clock.speed : m_prevSpeed;
		                   m_clock.speed = TimeSpeed::Paused; }
	}

	if (KeyT.down())
		m_traffic.spawnVehicle();

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
			m_editingRouteId = m_traffic.addBusRoute(newRoute);
		}
	}

	if (m_sandboxActive && KeyV.down())
	{
		m_mode = (m_mode == EditMode::SandboxEdit) ? EditMode::None : EditMode::SandboxEdit;
		m_sandboxDragNode = none;
		m_drawStartNode   = none;
		m_rectStart       = none;
	}

	if      (m_mode == EditMode::RoadDraw)     handleRoadDraw();
	else if (m_mode == EditMode::ZonePaint)    handleZonePaint();
	else if (m_mode == EditMode::BusRouteDraw) handleBusRouteDraw();
	else if (m_mode == EditMode::TerrainEdit)  handleTerrainEdit();
	else if (m_mode == EditMode::TrainDraw)    handleTrainDraw();
	else if (m_mode == EditMode::SandboxEdit)  handleSandboxEdit();
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
				m_traffic.markNetworkDirty();
				m_roadRenderer.markTopologyChanged();
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

		const int stopId = m_traffic.addBusStop(stop);
		m_traffic.addStopToRoute(m_editingRouteId, stopId);
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
			chunk->dirty = true;
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

	// ---- 左ボタンを押した瞬間：ドラッグ対象を決定 ----
	if (MouseL.down())
	{
		m_sandboxDragNode = none;
		m_sandboxDragCtrl = none;

		// 1) ノード優先
		m_sandboxDragNode = m_network.findNodeNear(*m_cursorGroundPos, 20.0f);

		// 2) ノードがなければ制御点を探す
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
		m_sandboxDragNode = none;
		m_sandboxDragCtrl = none;
	}

	// ---- 左ドラッグ中：ノード or 制御点を移動 ----
	if (MouseL.pressed())
	{
		const Vec3 delta = *m_cursorGroundPos - m_sandboxPrevCursor;

		if (m_sandboxDragNode)
		{
			RoadNode* node = m_network.getNode(*m_sandboxDragNode);
			if (node)
			{
				for (int eid : node->edgeIds)
				{
					RoadEdge* edge = m_network.getEdge(eid);
					if (!edge) continue;
					if (edge->nodeA == node->id) edge->ctrlA += delta;
					if (edge->nodeB == node->id) edge->ctrlB += delta;
					m_roadRenderer.markDirty(eid);
				}
				node->position += delta;
				m_traffic.markNetworkDirty();
			}
		}
		else if (m_sandboxDragCtrl)
		{
			RoadEdge* edge = m_network.getEdge(m_sandboxDragCtrl->edgeId);
			if (edge)
			{
				if (m_sandboxDragCtrl->isA) edge->ctrlA += delta;
				else                        edge->ctrlB += delta;
				m_roadRenderer.markDirty(edge->id);
				m_traffic.markNetworkDirty();
			}
		}

		m_sandboxPrevCursor = *m_cursorGroundPos;
	}

	// ---- 右クリック：削除 ----
	if (MouseR.down())
	{
		auto nearNode = m_network.findNodeNear(*m_cursorGroundPos, 20.0f);
		if (nearNode)
		{
			if (RoadNode* node = m_network.getNode(*nearNode))
				for (int eid : node->edgeIds)
					m_roadRenderer.markDirty(eid);
			m_network.removeNode(*nearNode);
			m_traffic.markNetworkDirty();
			m_roadRenderer.markTopologyChanged();
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
				m_roadRenderer.markDirty(bestId);
				m_network.removeEdge(bestId);
				m_traffic.markNetworkDirty();
				m_roadRenderer.markTopologyChanged();
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
