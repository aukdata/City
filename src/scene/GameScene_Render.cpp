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
}

// =============================================================================
// 選択ハイライト描画
// =============================================================================

void GameScene::renderSelectionHighlights()
{
	// 選択中のエッジをハイライト
	if (m_selectedEdgeId)
	{
		if (const auto bez = m_network.getBezier(*m_selectedEdgeId))
		{
			constexpr int kDiv = 30;
			const float len = bez->totalLength;
			for (int i = 0; i < kDiv; ++i)
			{
				const float tA = i / static_cast<float>(kDiv);
				const float tB = (i + 1) / static_cast<float>(kDiv);
				const Vec3 a = bez->positionAt(len * tA);
				const Vec3 b = bez->positionAt(len * tB);
				const float ha = m_world.computeHeight(static_cast<float>(a.x), static_cast<float>(a.z));
				const float hb = m_world.computeHeight(static_cast<float>(b.x), static_cast<float>(b.z));
				const Vec3 pa{ a.x, ha + 3.0, a.z };
				const Vec3 pb{ b.x, hb + 3.0, b.z };
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
			const float nh = m_world.computeHeight(
				static_cast<float>(node->position.x), static_cast<float>(node->position.z));

			// アウトラインリング
			{
				constexpr int kRingSeg = 24;
				constexpr double kRadius = 12.0;
				constexpr double kThick = 0.8;
				const Vec3 center{ node->position.x, nh + 4.0, node->position.z };
				const ColorF ringColor = ColorF{ 1.0, 0.6, 0.0, 0.8 }.removeSRGBCurve();
				for (int i = 0; i < kRingSeg; ++i)
				{
					const double a0 = Math::TwoPi * i / kRingSeg;
					const double a1 = Math::TwoPi * (i + 1) / kRingSeg;
					const Vec3 p0 = center + Vec3{ Math::Cos(a0) * kRadius, 0, Math::Sin(a0) * kRadius };
					const Vec3 p1 = center + Vec3{ Math::Cos(a1) * kRadius, 0, Math::Sin(a1) * kRadius };
					Cylinder{ p0, p1, kThick }.draw(ringColor);
				}
			}

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
					world.y = m_world.sampleHeight(static_cast<float>(world.x), static_cast<float>(world.z)) + 4.0f;

					const ColorF col = exits
						? ColorF{ 1.0, 0.3, 0.3, 0.8 }.removeSRGBCurve()
						: ColorF{ 0.3, 1.0, 0.3, 0.8 }.removeSRGBCurve();
					Sphere{ world, 2.0 }.draw(col);
				}
			}

			// LaneConnection のベジェ曲線を描画
			constexpr int kSegments = 16;
			for (const auto& conn : node->laneConnections)
			{
				if (conn.path.totalLength < 0.01f) continue;
				for (int i = 0; i < kSegments; ++i)
				{
					const float s0 = conn.path.totalLength * static_cast<float>(i) / kSegments;
					const float s1 = conn.path.totalLength * static_cast<float>(i + 1) / kSegments;
					Vec3 p0 = conn.path.positionAt(s0);
					Vec3 p1 = conn.path.positionAt(s1);
					p0.y = m_world.sampleHeight(static_cast<float>(p0.x), static_cast<float>(p0.z)) + 3.0f;
					p1.y = m_world.sampleHeight(static_cast<float>(p1.x), static_cast<float>(p1.z)) + 3.0f;
					Cylinder{ p0, p1, 0.5 }.draw(ColorF{ 0.2, 0.8, 1.0, 0.7 }.removeSRGBCurve());
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
			bool onElevated = false;
			if (v.location == VehicleLocation::OnConnection)
			{
				// 接続パスの場合、接続先ノードのエッジをチェック
			}
			else
			{
				const RoadEdge* eCheck = m_network.getEdge(v.currentEdge);
				if (eCheck && eCheck->useElevation) onElevated = true;
			}
			if (onElevated)
				rv.position.y += 2.05;  // ベジェ Y（=ノード地形高さ）+ 路面リフト
			else
				rv.position.y = m_world.computeHeight(
					static_cast<float>(rv.position.x),
					static_cast<float>(rv.position.z)) + 2.05;
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
		Sphere{ *m_cursorGroundPos, 5.0f }.draw(ColorF{ 1, 1, 0, 0.8 }.removeSRGBCurve());

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
}

// =============================================================================
// 2D UI 描画
// =============================================================================

void GameScene::render2DUI()
{
	m_placeNameRenderer.render(m_districts, m_camera, m_world);
	m_uiRenderer.render(m_clock, m_vehicleManager.vehicleCount(), modeString(), m_economy);

	// ミニマップ（小）をパネルより先に描画 → パネルが上に重なる
	m_minimapRenderer.update(m_panelManager);
	m_minimapRenderer.render(m_camera, m_districts);

	// パネル（ミニマップより上）
	m_panelManager.drawBackgrounds();

	drawNameListPanel();
	drawEdgePanel();
	drawNodePanel();
	drawVehiclePanel();

	m_minimapRenderer.drawExpandedPanel(m_panelManager, m_camera, m_districts);
}
