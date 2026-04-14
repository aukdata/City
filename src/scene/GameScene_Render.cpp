#include "GameScene.hpp"
#include <Siv3D/ViewFrustum.hpp>

namespace
{
	// 車線中心のワールド座標を返す（路盤ベジェ + 車線オフセット）
	Vec3 calcLaneWorldPos(const CubicBezier& bez, const RoadEdge& edge, int laneIdx, float arc)
	{
		const float ca = Clamp(arc, 0.0f, bez.totalLength);
		Vec3 pos = bez.positionAt(ca);
		const Vec3 tan = bez.tangentAt(ca);
		if (laneIdx >= 0 && laneIdx < static_cast<int>(edge.lanes.size()))
		{
			const Lane& ln = edge.lanes[laneIdx];
			const float ft = (bez.totalLength > 0.0f) ? (ca / bez.totalLength) : 0.0f;
			const float centerA = (ln.offsetA_L + ln.offsetA_R) * 0.5f;
			const float centerB = (ln.offsetB_L + ln.offsetB_R) * 0.5f;
			const float off = centerA + (centerB - centerA) * ft;
			const Vec3 perp{ tan.z, 0.0, -tan.x };
			const double pl = perp.length();
			if (pl > 0.001) pos += (perp / pl) * static_cast<double>(off);
		}
		return pos;
	}
}

// =============================================================================
// メイン描画エントリポイント
// =============================================================================

void GameScene::renderWorld()
{
	Stopwatch swStep{ StartImmediately::Yes };
	const Stopwatch swTotal{ StartImmediately::Yes };
	auto lap = [&](double& out) { out = swStep.msF(); swStep.restart(); };

	// 太陽・空のパラメータ計算
	const float hour = m_clock.hour;
	const float t    = (hour - 6.0f) * static_cast<float>(Math::Pi / 12.0);
	const float sinT  = static_cast<float>(Math::Sin(t));
	const float dayF  = Clamp(sinT, 0.0f, 1.0f);
	const float dawnF = Clamp(1.0f - Abs(sinT) * 2.5f, 0.0f, 1.0f);
	const double exposure = 0.15 + 0.85 * dayF + 0.30 * dawnF;

	// 国道標識テクスチャ合成（3D シーン前・2D パイプライン有効時）
	m_roadRenderer.prepareRouteSignTextures(m_network);

	// 3D シーン描画
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
		lap(m_renderTimings.sky);

		renderScene3D();
		lap(m_renderTimings.terrain);

		renderSelectionHighlights();
		lap(m_renderTimings.road);

		m_zoneManager.renderOverlay(m_world);
		lap(m_renderTimings.zone);

		renderVehicles();
		lap(m_renderTimings.vehicle);

		m_trainRenderer.renderTracks(m_trainNetwork);
		m_trainRenderer.renderTrains(m_trainManager.trains());
		lap(m_renderTimings.train);

		renderEditModeOverlays();

		m_debugRenderer.render(m_network, m_renderVehicles, m_world, m_camera);
		lap(m_renderTimings.debug);
	}

	Graphics3D::Flush();
	Shader::LinearToScreen(m_renderTexture);

	render2DUI();
	lap(m_renderTimings.ui);
	m_renderTimings.total = swTotal.msF();

	// パフォーマンスリングバッファに push
	{
		MainFrameStats mf;
		mf.lockWait = m_lockWaitMs;
		mf.sky      = m_renderTimings.sky;
		mf.terrain  = m_renderTimings.terrain;
		mf.road     = m_renderTimings.road;
		mf.zone     = m_renderTimings.zone;
		mf.vehicle  = m_renderTimings.vehicle;
		mf.train    = m_renderTimings.train;
		mf.debugUI  = m_renderTimings.debug + m_renderTimings.ui;
		m_mainPerfHistory.push(mf);
	}

	m_debugRenderer.renderProfiler(m_renderTimings.total, m_logicMs,
	                               m_renderTimings.sky, m_renderTimings.terrain,
	                               m_renderTimings.road, m_renderTimings.zone,
	                               m_renderTimings.vehicle, m_renderTimings.train,
	                               m_renderTimings.debug, m_renderTimings.ui, m_network);

	m_debugRenderer.renderPerfGraph(m_mainPerfHistory, m_simPerfHistory);
}

// =============================================================================
// 3D シーン描画
// =============================================================================

void GameScene::renderScene3D()
{
	const ViewFrustum frustum{ m_camera.camera3D(), 24000.0 };
	m_worldRenderer.render(m_world, m_camera.camera3D());
	m_roadRenderer.render(m_network, m_world, frustum,
	                     m_camera.camera3D().getEyePosition());
	m_roadRenderer.drawSignals(m_network, *m_simGraph, m_world,
	                           m_vehicleManager.trafficLights(),
	                           m_clock.now,
	                           m_camera.camera3D().getEyePosition());
	m_roadRenderer.drawRouteSigns(m_network, m_world,
	                              m_camera.camera3D().getEyePosition());
}

// =============================================================================
// 選択ハイライト描画
// =============================================================================

void GameScene::renderSelectionHighlights()
{
	// 選択中のエッジをハイライト
	if (m_selectedEdgeId)
	{
		const RoadEdge* selEdge = m_network.getEdge(*m_selectedEdgeId);
		if (const auto bez = m_network.getBezier(*m_selectedEdgeId))
		{
			const bool elev = selEdge && selEdge->useElevation;
			constexpr int kDiv = 30;
			const float len = bez->totalLength;
			for (int i = 0; i < kDiv; ++i)
			{
				const float tA = i / static_cast<float>(kDiv);
				const float tB = (i + 1) / static_cast<float>(kDiv);
				const Vec3 a = bez->positionAt(len * tA);
				const Vec3 b = bez->positionAt(len * tB);
				const double ya = elev ? a.y + 3.0
					: m_world.computeHeight(static_cast<float>(a.x), static_cast<float>(a.z)) + 3.0;
				const double yb = elev ? b.y + 3.0
					: m_world.computeHeight(static_cast<float>(b.x), static_cast<float>(b.z)) + 3.0;
				const Vec3 pa{ a.x, ya, a.z };
				const Vec3 pb{ b.x, yb, b.z };
				const ColorF cA = ColorF{ 1.0, 0.2, 0.2, 0.6 }.lerp(ColorF{ 0.2, 1.0, 0.2, 0.6 }, tA);
				const ColorF cB = ColorF{ 1.0, 0.2, 0.2, 0.6 }.lerp(ColorF{ 0.2, 1.0, 0.2, 0.6 }, tB);
				Cylinder{ pa, pb, 1.0 }.draw(cA.lerp(cB, 0.5));
			}
		}
	}

	// 選択中のノードをハイライト
	if (m_selectedNodeId)
	{
		if (const auto* node = m_network.getNode(*m_selectedNodeId))
		{
			const bool nodeElev = m_network.isNodeElevated(*m_selectedNodeId);

			// 接続車線の端点を球で表示
			const int nodeId = *m_selectedNodeId;
			for (const auto& att : node->attachments)
			{
				const RoadEdge* edge = m_network.getEdge(att.edgeId);
				if (!edge || !edge->isRoadbedBuilt()) continue;
				const auto bez = m_network.getBezier(att.edgeId);
				if (!bez) continue;

				const bool isNodeA = (edge->nodeA == nodeId);
				const float cutoff = isNodeA ? edge->cutoffA : edge->cutoffB;
				const float cutoffArc = isNodeA
					? cutoff
					: (bez->totalLength - cutoff);

				for (int li = 0; li < static_cast<int>(edge->lanes.size()); ++li)
				{
					const Lane& lane = edge->lanes[li];
					if (lane.op != OpState::Open && lane.op != OpState::Provisional) continue;

					const bool exits =
						(lane.dir == LaneDir::Forward  && edge->nodeB == nodeId) ||
						(lane.dir == LaneDir::Backward && edge->nodeA == nodeId);
					const bool enters =
						(lane.dir == LaneDir::Forward  && edge->nodeA == nodeId) ||
						(lane.dir == LaneDir::Backward && edge->nodeB == nodeId);
					if (!exits && !enters) continue;

					const Vec3 pos = bez->positionAt(cutoffArc);
					const Vec3 tan = bez->tangentAt(cutoffArc);
					const float centerA = (lane.offsetA_L + lane.offsetA_R) * 0.5f;
					const float centerB = (lane.offsetB_L + lane.offsetB_R) * 0.5f;
					const float ft = (bez->totalLength > 0.0f) ? (cutoffArc / bez->totalLength) : 0.0f;
					const float off = centerA + (centerB - centerA) * ft;
					const Vec3 perp = Vec3{ tan.z, 0.0, -tan.x }.normalized();
					Vec3 world = pos + perp * static_cast<double>(off);
					world.y = edge->useElevation
						? pos.y + 4.0
						: m_world.sampleHeight(static_cast<float>(world.x), static_cast<float>(world.z)) + 4.0;

					Sphere{ world, 0.5 }.draw(ColorF{ 1.0, 1.0, 0.0, 0.8 }.removeSRGBCurve());
				}
			}

			// LaneConnection のベジェ曲線を描画（信号状態で色分け）
			constexpr int kSegments = 16;
			const TrafficLight* tl = m_vehicleManager.getTrafficLight(node->id);
			for (const auto& conn : node->laneConnections)
			{
				if (conn.path.totalLength < 0.01f) continue;
				const bool green = tl ? tl->isGreen(conn.id) : true;
				const ColorF connColor = green
					? ColorF{ 0.2, 0.95, 0.3, 0.7 }.removeSRGBCurve()
					: ColorF{ 0.95, 0.2, 0.15, 0.7 }.removeSRGBCurve();
				constexpr double kArrowLen = 1.5;
				constexpr double kArrowRadius = 0.4;
				for (int i = 0; i < kSegments; ++i)
				{
					const float s0 = conn.path.totalLength * static_cast<float>(i) / kSegments;
					const float s1 = conn.path.totalLength * static_cast<float>(i + 1) / kSegments;
					Vec3 p0 = conn.path.positionAt(s0);
					Vec3 p1 = conn.path.positionAt(s1);
					if (nodeElev)
					{
						p0.y += 4.0;
						p1.y += 4.0;
					}
					else
					{
						p0.y = m_world.sampleHeight(static_cast<float>(p0.x), static_cast<float>(p0.z)) + 4.0;
						p1.y = m_world.sampleHeight(static_cast<float>(p1.x), static_cast<float>(p1.z)) + 4.0;
					}
					Cylinder{ p0, p1, 0.125 }.draw(connColor);

					// 4セグメントごとに矢印（コーン）を描画
					if ((i + 1) % 4 == 0)
					{
						const Vec3 dir = (p1 - p0).normalized();
						Cone{ p1, p1 + dir * kArrowLen, kArrowRadius }.draw(connColor);
					}
				}
			}
		}
	}
}

// =============================================================================
// 車両ワールド座標計算 + 描画
// =============================================================================

void GameScene::renderVehicles()
{
	m_renderVehicles.clear();

	for (const auto& v : m_vehicleManager.vehicles())
	{
		if (v.mode != VehicleMode::Active) continue;

		Vehicle rv = v;
		Vec3 tangent;

		if (v.location == VehicleLocation::OnConnection)
		{
			const RoadNode* node = m_network.getNode(v.connectionNodeId);
			if (!node) continue;
			const LaneConnection* conn = nullptr;
			for (const auto& c : node->laneConnections)
				if (c.id == v.connectionId) { conn = &c; break; }
			if (!conn) continue;
			const float ca = Clamp(v.arcPos, 0.0f, conn->path.totalLength);
			rv.position = conn->path.positionAt(ca);
			tangent = conn->path.tangentAt(ca);
		}
		else if (v.location == VehicleLocation::ChangingLane)
		{
			if (v.currentEdge < 0) continue;
			const auto bezier = m_network.getBezier(v.currentEdge);
			if (!bezier) continue;
			const RoadEdge* edgeCL = m_network.getEdge(v.currentEdge);
			if (!edgeCL) continue;
			const Vec3 posFrom = calcLaneWorldPos(*bezier, *edgeCL, v.laneFrom, v.arcPos);
			const Vec3 posTo   = calcLaneWorldPos(*bezier, *edgeCL, v.laneTo,   v.arcPos);
			rv.position = posFrom.lerp(posTo, static_cast<double>(v.laneChangeBlend));
			tangent = bezier->tangentAt(Clamp(v.arcPos, 0.0f, bezier->totalLength));
		}
		else
		{
			if (v.currentEdge < 0) continue;
			const auto bezier = m_network.getBezier(v.currentEdge);
			if (!bezier) continue;
			const RoadEdge* edgeOL = m_network.getEdge(v.currentEdge);
			if (!edgeOL) continue;
			rv.position = calcLaneWorldPos(*bezier, *edgeOL, v.currentLane, v.arcPos);
			tangent = bezier->tangentAt(Clamp(v.arcPos, 0.0f, bezier->totalLength));
		}

		{
			const bool onElevated = (v.location == VehicleLocation::OnConnection)
				? m_network.isNodeElevated(v.connectionNodeId)
				: [&]{ const RoadEdge* e = m_network.getEdge(v.currentEdge); return e && e->useElevation; }();
			if (onElevated)
				rv.position.y += kRoadLineLift;
			else
				rv.position.y = m_world.computeHeight(
					static_cast<float>(rv.position.x),
					static_cast<float>(rv.position.z)) + kRoadLineLift;
		}

		float sign = 1.0f;
		if (v.location != VehicleLocation::OnConnection)
		{
			const RoadEdge* edge = m_network.getEdge(v.currentEdge);
			const int li = (v.location == VehicleLocation::ChangingLane) ? v.laneFrom : v.currentLane;
			if (edge && li >= 0 && li < static_cast<int>(edge->lanes.size()))
				sign = (edge->lanes[li].dir == LaneDir::Forward) ? 1.0f : -1.0f;
		}
		const double fx = sign * tangent.x;
		const double fy = sign * tangent.y;
		const double fz = sign * tangent.z;
		rv.heading = static_cast<float>(Math::Atan2(fx, fz));
		rv.pitch   = static_cast<float>(Math::Atan2(fy, Math::Sqrt(fx * fx + fz * fz)));
		m_renderVehicles << rv;
	}
	m_vehicleRenderer.render(m_renderVehicles, m_camera.camera3D().getEyePosition());
}

// =============================================================================
// 編集モードオーバーレイ
// =============================================================================

void GameScene::renderEditModeOverlays()
{
	if (m_mode == EditMode::TrainDraw && m_cursorGroundPos)
		Sphere{ *m_cursorGroundPos, 6.0f }.draw(ColorF{ 0.9, 0.85, 0.2, 0.8 }.removeSRGBCurve());

	if (m_mode == EditMode::BusRouteDraw && m_cursorGroundPos)
		Sphere{ *m_cursorGroundPos, 4.0f }.draw(ColorF{ 0.2, 0.5, 0.9, 0.8 }.removeSRGBCurve());

	if (m_mode == EditMode::RoadDraw && m_cursorGroundPos)
	{
		Sphere{ *m_cursorGroundPos, 5.0f }.draw(ColorF{ 1, 1, 0, 0.8 }.removeSRGBCurve());

		// 始点が設定済みならプレビュー描画
		if (m_drawStartNode)
		{
			const RoadNode* startNode = m_network.getNode(*m_drawStartNode);
			if (startNode)
			{
				// 始点ノードを強調
				Sphere{ startNode->position + Vec3{0, 2, 0}, 5.0f }
					.draw(ColorF{ 1, 1, 0, 0.5 }.removeSRGBCurve());

				const Vec3 pA = startNode->position;
				const Vec3 pB = *m_cursorGroundPos;
				if (pA.distanceFrom(pB) > 5.0)
				{
					const auto [ctrlA, ctrlB] = calcRoadDrawControlPoints(*m_drawStartNode, pB);
					CubicBezier preview{ pA, ctrlA, ctrlB, pB };
					const float halfW = m_drawTemplate.totalWidth() * 0.5f;
					const ColorF previewCol = ColorF{ 1, 1, 1, 0.3 }.removeSRGBCurve();
					constexpr int N = 20;

					Vec3 prevL, prevR;
					for (int i = 0; i <= N; ++i)
					{
						const float s = preview.totalLength * (static_cast<float>(i) / N);
						const Vec3 center = preview.positionAt(s);
						const Vec3 tan = preview.tangentAt(s);
						Vec3 right = tan.cross(Vec3::Up());
						const double rLen = right.length();
						right = (rLen > 1e-6) ? right / rLen : Vec3::Right();

						const double yOff = m_world.computeHeight(
							static_cast<float>(center.x), static_cast<float>(center.z)) + kRoadSurfaceLift;
						const Vec3 c = Vec3{ center.x, yOff, center.z };
						const Vec3 L = c - right * halfW;
						const Vec3 R = c + right * halfW;

						if (i > 0)
						{
							Line3D{ prevL, L }.draw(previewCol);
							Line3D{ prevR, R }.draw(previewCol);
						}
						prevL = L;
						prevR = R;
					}
				}
			}
		}
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

			// 制御点の描画ラムダ
			const auto drawCtrlPoint = [&](Vec3 nodePos, Vec3 cp, bool isDragging) {
				const bool hov = m_cursorGroundPos &&
					Vec2{ cp.x, cp.z }.distanceFrom(cur2D) < 18.0f;
				const ColorF col = isDragging
					? ColorF{ 1.0, 0.5, 0.0, 1.0 }
					: (hov ? ColorF{ 1.0, 1.0, 0.3, 0.9 }
					       : ColorF{ 0.2, 0.9, 0.4, 0.7 });
				Line3D{ nodePos + Vec3{0,2,0}, cp + Vec3{0,2,0} }
					.draw(ColorF{ 0.5, 0.5, 0.5, 0.5 }.removeSRGBCurve());
				Sphere{ cp + Vec3{0,2,0}, isDragging ? 6.0 : 4.0 }
					.draw(col.removeSRGBCurve());
			};

			const bool dragA = m_sandboxDragCtrl &&
			    m_sandboxDragCtrl->edgeId == edge.id && m_sandboxDragCtrl->isControlPointA;
			const bool dragB = m_sandboxDragCtrl &&
			    m_sandboxDragCtrl->edgeId == edge.id && !m_sandboxDragCtrl->isControlPointA;
			drawCtrlPoint(nA->position, edge.ctrlA, dragA);
			drawCtrlPoint(nB->position, edge.ctrlB, dragB);
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
}

// =============================================================================
// 2D UI 描画
// =============================================================================

void GameScene::render2DUI()
{
	m_placeNameRenderer.render(m_districts, m_camera, m_world);
	m_routeSignRenderer.render(m_network, m_camera);
	m_uiRenderer.render(m_clock, m_vehicleManager.vehicleCount(), modeString(), m_economy);

	// ミニマップ（小）をパネルより先に描画 → パネルが上に重なる
	m_minimapRenderer.update(m_panelManager);
	m_minimapRenderer.render(m_camera, m_districts);

	// パネル（ミニマップより上）— zOrder 昇順で背景+コンテンツを描画
	for (const auto& panelId : m_panelManager.sortedPanelIds())
	{
		m_panelManager.drawBackground(panelId);

		if (panelId == U"name_list")         { drawNameListPanel(); }
		else if (panelId == U"edge_info")    { drawEdgePanel(); }
		else if (panelId == U"draw_template"){ drawDrawTemplatePanel(); }
		else if (panelId == U"node_info")    { drawNodePanel(); }
		else if (panelId == U"signal_edit")  { drawSignalEditPanel(); }
		else if (panelId == U"vehicle_info") { drawVehiclePanel(); }
		else if (panelId == U"minimap_expanded")
		{
			m_minimapRenderer.drawExpandedPanel(m_panelManager, m_camera, m_districts);
		}
	}

	if (m_showPauseMenu)
		drawPauseMenu();
}

void GameScene::drawPauseMenu()
{
	const double sw = Scene::Width();
	const double sh = Scene::Height();

	// 半透明オーバーレイ
	Scene::Rect().draw(ColorF{ 0.0, 0.0, 0.0, 0.6 });

	// メニューパネル
	constexpr double panelW = 320;
	constexpr double panelH = 340;
	const RectF panel{ (sw - panelW) / 2, (sh - panelH) / 2, panelW, panelH };
	panel.rounded(8).draw(ColorF{ 0.12, 0.12, 0.15, 0.95 });
	panel.rounded(8).drawFrame(1.0, ColorF{ 0.5, 0.5, 0.55, 0.6 });

	// タイトル
	const Font& font = SimpleGUI::GetFont();
	font(U"PAUSED").drawAt(32, Vec2{ sw / 2, panel.y + 40 }, ColorF{ 0.9 });

	// ボタン配置
	constexpr double btnW = 240;
	constexpr double btnH = 44;
	constexpr double gap  = 12;
	const double startY = panel.y + 90;
	const double btnX = (sw - btnW) / 2;

	struct MenuItem { String label; };
	const Array<MenuItem> items =
	{
		{ U"ゲームに戻る" },
		{ U"セーブ" },
		{ U"設定" },
		{ U"タイトルに戻る" },
		{ U"ゲーム終了" },
	};

	for (int32 i = 0; i < static_cast<int32>(items.size()); ++i)
	{
		const RectF btn{ btnX, startY + i * (btnH + gap), btnW, btnH };
		const bool hover = btn.mouseOver();

		btn.rounded(4).draw(hover ? ColorF{ 0.35, 0.38, 0.45 } : ColorF{ 0.2, 0.22, 0.28 });
		btn.rounded(4).drawFrame(1.0, hover ? ColorF{ 0.7, 0.75, 0.85 } : ColorF{ 0.4, 0.42, 0.48 });
		font(items[i].label).drawAt(20, btn.center(), ColorF{ 0.92 });

		if (hover && MouseL.down())
		{
			switch (i)
			{
			case 0: // ゲームに戻る
				m_showPauseMenu = false;
				break;

			case 1: // セーブ
				saveGame();
				break;

			case 2: // 設定（仮）
				break;

			case 3: // タイトルに戻る
				m_showPauseMenu = false;
				changeScene(SceneState::Title, 0s);
				break;

			case 4: // ゲーム終了
				System::Exit();
				break;
			}
		}
	}
}
