#include "GameScene.hpp"
#include "EdgeSectionState.hpp"
#include "../ui/PanelWidget.hpp"
#include "../asset/AssetRegistrar.hpp"
#include <Siv3D/ViewFrustum.hpp>
#include <fstream>

namespace
{
	// ===== 選択アウトライン描画パラメータ =====

	/// @brief アウトライン色（ゴールド寄りの黄色）
	constexpr Float4 kSelectionOutlineColor{ 1.0f, 0.85f, 0.1f, 1.0f };
	/// @brief アウトラインの幅（ピクセル単位、シェーダ内で texelSize と乗算される）
	constexpr float  kSelectionOutlineWidthPx = 6.0f;

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
			const float off = ln.centerAt(ft);
			const Vec3 perp = tangentToRight(tan);
			pos += perp * static_cast<double>(off);
		}
		return pos;
	}
}

// =============================================================================
// 空・太陽パラメータ計算
// =============================================================================

GameScene::SkyParams GameScene::calcSkyParams() const
{
	SkyParams p;
	const float hour = m_clock.hour;
	p.timeAngle  = (hour - 6.0f) * static_cast<float>(Math::Pi / 12.0);
	p.sinTime    = static_cast<float>(Math::Sin(p.timeAngle));
	p.dayFactor  = Clamp(p.sinTime, 0.0f, 1.0f);
	p.dawnFactor = Clamp(1.0f - Abs(p.sinTime) * 2.5f, 0.0f, 1.0f);
	p.exposure   = 0.15 + 0.85 * p.dayFactor + 0.30 * p.dawnFactor;
	return p;
}

// =============================================================================
// パフォーマンス統計記録
// =============================================================================

void GameScene::pushPerfStats()
{
	MainFrameStats stats;
	stats.lockWait = m_lockWaitMs;
	stats.sky      = m_renderTimings.sky;
	stats.terrain  = m_renderTimings.terrain;
	stats.road     = m_renderTimings.road;
	stats.zone     = m_renderTimings.zone;
	stats.vehicle  = m_renderTimings.vehicle;
	stats.train    = m_renderTimings.train;
	stats.debugUI  = m_renderTimings.debug + m_renderTimings.ui;
	m_mainPerfHistory.push(stats);
}

// =============================================================================
// メイン描画エントリポイント
// =============================================================================

void GameScene::renderWorld()
{
	// 3D 本体、選択エフェクト、2D UI、性能計測を 1 フレームの決まった順序で積み上げる。
	Stopwatch swStep{ StartImmediately::Yes };
	const Stopwatch swTotal{ StartImmediately::Yes };
	auto lap = [&](double& out) { out = swStep.msF(); swStep.restart(); };

	const SkyParams sky = calcSkyParams();

	double dbgRouteSign = 0, dbgGuideSign = 0, dbgRtSetup = 0;

	// 国道標識テクスチャ合成（3D シーン前・2D パイプライン有効時）
	m_roadRenderer.prepareRouteSignTextures(m_network);
	lap(dbgRouteSign);
	// 案内標識テクスチャ合成（同上）
	m_roadRenderer.prepareGuideSignTextures(m_network);
	lap(dbgGuideSign);

	// 3D シーン描画
	{
		const ScopedRenderTarget3D target{ m_renderTexture.clear(ColorF{ 0.2, 0.3, 0.4 }.removeSRGBCurve()) };
		const ScopedRenderStates3D depthState{ DepthStencilState::DepthTestWrite };
		lap(dbgRtSetup);

		Graphics3D::SetCameraTransform(m_camera.camera3D());

		const Vec3 sunDir = Vec3{ Math::Cos(sky.timeAngle), sky.sinTime, 0.3 }.normalized();
		Graphics3D::SetSunDirection(sunDir);
		Graphics3D::SetGlobalAmbientColor(ColorF{ 0.55 + 0.30 * sky.dayFactor + 0.10 * sky.dawnFactor });

		const ColorF dayZenith  { 0.10, 0.35, 0.80 };
		const ColorF dawnZenith { 0.22, 0.18, 0.38 };
		const ColorF nightZenith{ 0.01, 0.02, 0.07 };
		m_sky.zenithColor = nightZenith.lerp(dawnZenith, sky.dawnFactor).lerp(dayZenith, sky.dayFactor);

		const ColorF dayHorizon  { 0.60, 0.78, 0.95 };
		const ColorF dawnHorizon { 0.85, 0.42, 0.15 };
		const ColorF nightHorizon{ 0.02, 0.03, 0.10 };
		m_sky.horizonColor = nightHorizon.lerp(dawnHorizon, sky.dawnFactor).lerp(dayHorizon, sky.dayFactor);

		m_sky.starBrightness = Clamp(1.0 - sky.dayFactor * 3.0 - sky.dawnFactor * 2.0, 0.0, 1.0);
		m_sky.cloudTime = Scene::Time() * 0.015;
		m_sky.cloudsEnabled = false;
		m_sky.draw(sky.exposure);
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

	renderSelectionOutline();

	render2DUI();
	lap(m_renderTimings.ui);
	m_renderTimings.total = swTotal.msF();

	pushPerfStats();

	m_debugRenderer.renderProfiler(m_renderTimings.total, m_logicMs,
	                               m_renderTimings.sky, m_renderTimings.terrain,
	                               m_renderTimings.road, m_renderTimings.zone,
	                               m_renderTimings.vehicle, m_renderTimings.train,
	                               m_renderTimings.debug, m_renderTimings.ui, m_network);

	m_debugRenderer.renderPerfGraph(m_mainPerfHistory, m_simPerfHistory);

	// perf.log に 120 フレームごとの各フェーズ計測値を追記する（std::flush で即反映）
	constexpr int kPerfLogIntervalFrames = 120;
	static int           s_perfFrameCount = 0;
	static std::ofstream s_perfLog{ "perf.log", std::ios::app };
	if (++s_perfFrameCount >= kPerfLogIntervalFrames)
	{
		s_perfFrameCount = 0;
		const double fps = 1000.0 / Max(m_renderTimings.total, 0.001);
		s_perfLog
			<< "[PERF] FPS=" << fps << " total=" << m_renderTimings.total << "ms\n"
			<< "  sky="      << m_renderTimings.sky
			<< " terrain="   << m_renderTimings.terrain
			<< " road="      << m_renderTimings.road
			<< " zone="      << m_renderTimings.zone
			<< " vehicle="   << m_renderTimings.vehicle
			<< " train="     << m_renderTimings.train
			<< " ui="        << m_renderTimings.ui << "\n"
			<< "  [scene3D] terrainOnly=" << m_renderTimings.terrainOnly
			<< " roadMesh="   << m_renderTimings.roadMesh
			<< " signals="    << m_renderTimings.signals
			<< " routeSigns=" << m_renderTimings.routeSigns << "\n"
			<< "  logic=" << m_logicMs << "ms\n"
			<< "  [pre3D] routeSignPrep=" << dbgRouteSign
			<< " guideSignPrep=" << dbgGuideSign
			<< " rtSetup="       << dbgRtSetup << "\n"
			<< "  [ui] placeNames="  << m_renderTimings.uiPlaceNames
			<< " routeSigns="        << m_renderTimings.uiRouteSigns
			<< " uiRenderer="        << m_renderTimings.uiRenderer
			<< " minimap="           << m_renderTimings.uiMinimap
			<< " edgeHandles="       << m_renderTimings.uiEdgeHandles
			<< " panels="            << m_renderTimings.uiPanels << "\n"
			<< std::flush;
	}
}

// =============================================================================
// 3D シーン描画
// =============================================================================

void GameScene::renderScene3D()
{
	// 地形、道路、信号、路線標識を描画順に分け、主要サブシステムごとの時間も個別に測る。
	Stopwatch sw{ StartImmediately::Yes };
	auto lap = [&](double& out) { out = sw.msF(); sw.restart(); };

	const ViewFrustum frustum{ m_camera.camera3D(), 24000.0 };
	m_worldRenderer.render(m_world, m_network, m_camera.camera3D());
	lap(m_renderTimings.terrainOnly);

	m_roadRenderer.render(m_network, m_world, frustum,
	                     m_camera.camera3D().getEyePosition());

	// Planned / UnderConstruction エッジのワイヤーフレーム
	m_roadRenderer.renderWireframes(m_network, m_world, frustum,
	                                m_camera.camera3D().getEyePosition());
	lap(m_renderTimings.roadMesh);

	m_roadRenderer.drawSignals(m_network, *m_simGraph, m_world,
	                           m_vehicleManager.trafficLights(),
	                           m_clock.now,
	                           m_camera.camera3D().getEyePosition());
	lap(m_renderTimings.signals);

	m_roadRenderer.drawRouteSigns(m_network, m_world,
	                              m_camera.camera3D().getEyePosition());
	lap(m_renderTimings.routeSigns);
}

// =============================================================================
// 選択ハイライト描画
// =============================================================================

void GameScene::renderSelectionHighlights()
{
	// 現在の選択対象に応じて、編集や確認に必要な 3D ガイドだけを追加描画する。
	// エッジ選択時の中央線ハイライトは廃止（3D ハンドルで可視化する）

	// 選択中のノードをハイライト
	if (selectedNodeId())
	{
		if (const auto* node = m_network.getNode(*selectedNodeId()))
		{
			// 接続車線の端点を球で表示
			const int nodeId = *selectedNodeId();
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
					const float ft = (bez->totalLength > 0.0f) ? (cutoffArc / bez->totalLength) : 0.0f;
					const float off = lane.centerAt(ft);
					const Vec3 perp = tangentToRight(tan);
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

				// 接続元・接続先いずれかのエッジが useElevation の場合は高架扱い
				const RoadEdge* fromEdge = m_network.getEdge(conn.fromEdgeId);
				const RoadEdge* toEdge   = m_network.getEdge(conn.toEdgeId);
				const bool connElev = (fromEdge && fromEdge->useElevation)
				                   || (toEdge   && toEdge->useElevation);

				for (int i = 0; i < kSegments; ++i)
				{
					const float s0 = conn.path.totalLength * static_cast<float>(i) / kSegments;
					const float s1 = conn.path.totalLength * static_cast<float>(i + 1) / kSegments;
					Vec3 p0 = conn.path.positionAt(s0);
					Vec3 p1 = conn.path.positionAt(s1);
					if (connElev)
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

	// 建物選択時: 保持している接道位置(edgeId, edgeT)を 3D マーカーで表示
	if (m_selectedBuilding)
	{
		const Chunk* chunk = m_world.getChunk(Point{ m_selectedBuilding->chunkX, m_selectedBuilding->chunkZ });
		if (chunk)
		{
			const Building& b = chunk->buildingGrid[{ m_selectedBuilding->col, m_selectedBuilding->row }];
			if (b.type != BuildingType::None && b.edgeId >= 0)
			{
				const RoadEdge* edge = m_network.getEdge(b.edgeId);
				const auto bez = m_network.getBezier(b.edgeId);
				if (edge && bez)
				{
					const float t = Clamp(b.edgeT, 0.0f, 1.0f);
					const float arc = bez->totalLength * t;
					Vec3 anchor = bez->positionAt(arc);
					const Vec3 tan = bez->tangentAt(arc);

					if (edge->useElevation)
						anchor.y += 4.0;
					else
						anchor.y = m_world.sampleHeight(static_cast<float>(anchor.x), static_cast<float>(anchor.z)) + 3.0;

					Vec3 dir = Vec3{ tan.x, 0.0, tan.z };
					if (dir.lengthSq() < 1e-8) dir = Vec3{ 1.0, 0.0, 0.0 };
					dir = dir.normalized();
					Vec3 right = Vec3{ -dir.z, 0.0, dir.x };

					const ColorF mainC = ColorF{ 1.0, 0.45, 0.15, 0.95 }.removeSRGBCurve();
					const ColorF subC  = ColorF{ 1.0, 0.95, 0.2, 0.9 }.removeSRGBCurve();
					Line3D{ anchor - dir * 6.0, anchor + dir * 6.0 }.draw(mainC);
					Line3D{ anchor - right * 4.0, anchor + right * 4.0 }.draw(subC);
					Sphere{ anchor, 1.6 }.draw(mainC);
				}
			}
		}
	}

	// 建物本体の選択表示はアウトラインシェーダ (renderSelectionOutline) で行う
}

// =============================================================================
// 選択オブジェクト アウトライン描画
// =============================================================================

void GameScene::renderSelectionOutline()
{
	if (not m_outlineMask) return;
	if (not m_outlinePS) return;

	// 選択対象が無ければ何もしない
	const bool hasSelection = m_selectedBuilding || m_selectedVehicleId
		|| m_selection.kind != SelectionKind::None;
	if (!hasSelection) return;

	const ColorF maskColor{ 1.0, 1.0, 1.0, 1.0 };

	// --- 1) マスクテクスチャに選択オブジェクトを単色で描画（書き込み先を差し替えるだけ） ---
	{
		const ScopedRenderTarget3D target{ m_outlineMask.clear(ColorF{ 0, 0, 0, 0 }) };
		const ScopedRenderStates3D depthState{ DepthStencilState::Default3D };

		Graphics3D::SetCameraTransform(m_camera.camera3D());

		// 建物
		if (m_selectedBuilding)
		{
			if (const Chunk* chunk = m_world.getChunk(Point{ m_selectedBuilding->chunkX, m_selectedBuilding->chunkZ }))
				m_worldRenderer.drawBuildingSilhouette(*chunk, m_world,
					m_selectedBuilding->col, m_selectedBuilding->row, maskColor);
		}

		// 車両
		if (m_selectedVehicleId)
		{
			for (const auto& v : m_renderVehicles)
			{
				if (v.id == *m_selectedVehicleId)
				{
					m_vehicleRenderer.drawVehicleSilhouette(v, m_camera.camera3D().getEyePosition(), maskColor);
					break;
				}
			}
		}

		// 道路系（Edge/Node/Signal/GuideSign）
		switch (m_selection.kind)
		{
		case SelectionKind::Edge:
			m_roadRenderer.drawEdgeSilhouette(m_selection.id, m_network, m_world, maskColor);
			break;
		case SelectionKind::Node:
			m_roadRenderer.drawNodeSilhouette(m_selection.id, m_network, m_world, maskColor);
			break;
		case SelectionKind::Signal:
			m_roadRenderer.drawSignalSilhouette(m_selection.id, m_network, m_world, maskColor);
			break;
		case SelectionKind::GuideSign:
			m_roadRenderer.drawGuideSignSilhouette(m_selection.id, m_network, m_world, maskColor);
			break;
		default:
			break;
		}

		Graphics3D::Flush();
	}

	// --- 2) 2D: アウトライン抽出シェーダでスクリーンに加算合成 ---
	{
		m_outlineCB->texelSize    = Float4{ 1.0f / m_outlineMask.width(), 1.0f / m_outlineMask.height(), 0, 0 };
		m_outlineCB->outlineColor = kSelectionOutlineColor;
		m_outlineCB->outlineScale = Float4{ kSelectionOutlineWidthPx, 0, 0, 0 };

		Graphics2D::SetPSConstantBuffer(1, m_outlineCB);
		const ScopedCustomShader2D shader{ m_outlinePS };
		const ScopedRenderStates2D blend{ BlendState::Additive };
		m_outlineMask.draw();
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
				rv.position.y = m_world.sampleHeight(
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

		// スタート/ゴール指定モードのプレビュー描画
		if (m_autoPlaceMode)
		{
			if (m_autoPlaceStart)
			{
				// スタートマーカー（赤球）
				Sphere{ *m_autoPlaceStart + Vec3{0, 2, 0}, 6.0f }
					.draw(ColorF{ 1.0, 0.2, 0.2, 0.9 }.removeSRGBCurve());

				// スタート → カーソルの点線（短線分連続描画）
				const Vec3 from = *m_autoPlaceStart;
				const Vec3 to   = *m_cursorGroundPos;
				const double totalDist = from.distanceFrom(to);
				if (totalDist > 5.0)
				{
					constexpr double kSegLen  = 15.0;  // 実線区間 [m]
					constexpr double kGapLen  = 8.0;   // 空白区間 [m]
					constexpr double kPeriod  = kSegLen + kGapLen;
					const ColorF dotCol = ColorF{ 1.0, 0.4, 0.4, 0.7 }.removeSRGBCurve();
					const Vec3 dir = (to - from) / totalDist;
					double d = 0.0;
					while (d < totalDist)
					{
						const double segEnd = Min(d + kSegLen, totalDist);
						Line3D{ from + dir * d, from + dir * segEnd }.draw(dotCol);
						d += kPeriod;
					}
				}
			}
			else
			{
				// スタート未指定: カーソル地点に白球（既存黄色球の上に重ねない）
				Sphere{ *m_cursorGroundPos + Vec3{0, 2, 0}, 5.0f }
					.draw(ColorF{ 1.0, 0.5, 0.5, 0.7 }.removeSRGBCurve());
			}
		}

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

						const double yOff = m_world.sampleHeight(
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
	Stopwatch sw{ StartImmediately::Yes };
	auto lap = [&](double& out) { out = sw.msF(); sw.restart(); };

	m_placeNameRenderer.render(m_districts, m_camera, m_world);
	lap(m_renderTimings.uiPlaceNames);

	m_routeSignRenderer.render(m_network, m_camera);
	lap(m_renderTimings.uiRouteSigns);

	m_uiRenderer.render(m_clock, m_vehicleManager.vehicleCount(), modeString(), m_economy);
	lap(m_renderTimings.uiRenderer);

	// ミニマップ（小）をパネルより先に描画 → パネルが上に重なる
	m_minimapRenderer.update(m_panelManager);
	m_minimapRenderer.render(m_camera, m_districts);
	lap(m_renderTimings.uiMinimap);

	// 選択中エッジの 3D 編集ハンドル（パネルより後ろに描画）
	renderEdgeHandles();
	lap(m_renderTimings.uiEdgeHandles);

	// パネル（ミニマップより上）— zOrder 昇順で背景+コンテンツを描画
	for (const auto& panelId : m_panelManager.sortedPanelIds())
	{
		m_panelManager.drawBackground(panelId);

		if (panelId == U"name_list")         { drawNameListPanel(); }
		else if (panelId == U"edge_info")    { drawEdgePanel(); }
		else if (panelId == U"draw_template")
		{
			if (m_mode == EditMode::RoadPlan) drawRoadPlanPanel();
			else                              drawDrawTemplatePanel();
		}
		else if (panelId == U"node_info")    { drawNodePanel(); }
		else if (panelId == U"signal_edit")  { drawSignalEditPanel(); }
		else if (panelId == U"guide_sign_edit") { drawGuideSignEditPanel(); }
		else if (panelId == U"guide_sign_editor") { drawGuideSignEditorPanel(); }
		else if (panelId == U"vehicle_info") { drawVehiclePanel(); }
		else if (panelId == U"building_info"){ drawBuildingPanel(); }
		else if (panelId == U"route_info")   { drawRoutePanel(); }
		else if (panelId == U"minimap_expanded")
		{
			m_minimapRenderer.drawExpandedPanel(m_panelManager, m_camera, m_districts);
		}
	}

	if (m_showPauseMenu)
		drawPauseMenu();

	lap(m_renderTimings.uiPanels);
}

// =============================================================================
// 3D エッジ編集ハンドル描画
// =============================================================================

namespace
{
	/// @brief Part 種別→基本色（GameScene_Panels.cpp 側と同じ対応）
	ColorF handlePartColor(RoadPartType type)
	{
		switch (type)
		{
		case RoadPartType::Roadbed:   return ColorF{0.45, 0.45, 0.48};
		case RoadPartType::Shoulder:  return ColorF{0.55, 0.53, 0.48};
		case RoadPartType::Median:    return ColorF{0.65, 0.75, 0.45};
		case RoadPartType::Sidewalk:  return ColorF{0.80, 0.78, 0.75};
		case RoadPartType::Gutter:    return ColorF{0.40, 0.40, 0.42};
		case RoadPartType::Guardrail: return ColorF{0.75, 0.75, 0.75};
		case RoadPartType::Wall:      return ColorF{0.65, 0.62, 0.58};
		case RoadPartType::Curb:      return ColorF{0.70, 0.68, 0.64};
		case RoadPartType::Slope:     return ColorF{0.60, 0.72, 0.50};
		case RoadPartType::BikeLane:  return ColorF{0.50, 0.65, 0.75};
		default:                      return ColorF{0.5};
		}
	}

	/// @brief 道路端 A/B / L/R 向けのティント（A=暖色赤、B=寒色緑、L=明、R=暗）
	ColorF sideTint(bool atA, bool isRight)
	{
		const ColorF base = atA ? ColorF{1.0, 0.55, 0.45} : ColorF{0.45, 1.0, 0.55};
		return isRight ? (base * 0.7 + ColorF{0.15}) : base;
	}
}

void GameScene::renderEdgeHandles()
{
	if (m_selection.kind != SelectionKind::Edge) return;
	const RoadEdge* edge = m_network.getEdge(m_selection.id);
	if (!edge) return;
	const auto bezOpt = m_network.getBezier(edge->id);
	if (!bezOpt) return;
	const CubicBezier& bez = *bezOpt;
	const auto& cam = m_camera.camera3D();
	const Vec2 cur = Cursor::PosF();
	constexpr double kR = 7.0;
	static const Font& tipFontRef = FontAsset(Asset::Panel14);

	enum class Shape { Circle, Square };
	auto drawHandle = [&](Vec3 wp, ColorF baseCol, StringView tip, bool active, Shape shape)
	{
		const Vec2 sp = cam.worldToScreenPoint(wp).xy();
		const bool hover = (sp.distanceFrom(cur) <= kR + 2.0);
		const ColorF col = (active || hover) ? ColorF{1.0, 1.0, 0.4} : baseCol;
		if (shape == Shape::Circle)
		{
			Circle{ sp, kR }.draw(col).drawFrame(1.5, ColorF{0.1});
		}
		else
		{
			// 幅（サイズ変更）ハンドルは四角
			RectF{ sp.x - kR, sp.y - kR, kR * 2, kR * 2 }.draw(col).drawFrame(1.5, ColorF{0.1});
		}
		if (hover)
		{
			Cursor::RequestStyle(CursorStyle::Hand);
			PanelWidget::tipFont = &tipFontRef;
			PanelWidget::tipText = String{ tip };
			PanelWidget::tipPos  = Vec2{ sp.x + 10, sp.y + 10 };
			PanelWidget::tipActive = true;
			PanelWidget::flushTooltip();
		}
	};

	// Cutoff A/B（移動＝円）
	drawHandle(bez.positionAt(edge->cutoffA), sideTint(true, false),
		U"A端カットオフ {:.1f}m / ドラッグで調整"_fmt(edge->cutoffA),
		m_edgeHandleDrag.kind == EdgeHandleDrag::Kind::CutoffA, Shape::Circle);
	{
		const float sB = Max(0.0f, bez.totalLength - edge->cutoffB);
		drawHandle(bez.positionAt(sB), sideTint(false, false),
			U"B端カットオフ {:.1f}m / ドラッグで調整"_fmt(edge->cutoffB),
			m_edgeHandleDrag.kind == EdgeHandleDrag::Kind::CutoffB, Shape::Circle);
	}

	// Part と Lane は排他表示。Lane が選択されていれば Lane のみ、それ以外で Part が選択されていれば Part のみ
	const int partIdxSel = EdgeSectionState::selectedPart;
	const int laneIdxSel = EdgeSectionState::selectedLane;
	const bool showLane = (laneIdxSel >= 0 && laneIdxSel < static_cast<int>(edge->lanes.size()));
	const bool showPart = (!showLane) && (partIdxSel >= 0 && partIdxSel < static_cast<int>(edge->parts.size()));

	// Part: 中央=円（一律移動）, 左右=四角（A/B 両端一律）, 四隅=四角（個別）
	if (showPart)
	{
		const float sA   = edge->cutoffA;
		const float sB   = Max(0.0f, bez.totalLength - edge->cutoffB);
		const float sMid = (sA + sB) * 0.5f;
		const Vec3 pA    = bez.positionAt(sA);
		const Vec3 pB    = bez.positionAt(sB);
		const Vec3 pM    = bez.positionAt(sMid);
		const Vec3 rA    = tangentToRight(bez.tangentAt(sA));
		const Vec3 rB    = tangentToRight(bez.tangentAt(sB));
		const Vec3 rM    = tangentToRight(bez.tangentAt(sMid));

		const int i = partIdxSel;
		const auto& p = edge->parts[i];
		const ColorF pc = handlePartColor(p.type);

		// 一律操作ハンドル（代表中央/左/右）
		drawHandle(pM + rM * ((p.offsetL() + p.offsetR()) * 0.5f), pc,
			U"部品[{}] 一律移動 (代表 {:.2f}m)"_fmt(i, p.offsetL()),
			m_edgeHandleDrag.kind == EdgeHandleDrag::Kind::PartCenter && m_edgeHandleDrag.idx == i,
			Shape::Circle);
		drawHandle(pM + rM * p.offsetL(), pc * 0.85f,
			U"部品[{}] 左端（A/B 一律）"_fmt(i),
			m_edgeHandleDrag.kind == EdgeHandleDrag::Kind::PartLeft && m_edgeHandleDrag.idx == i,
			Shape::Square);
		drawHandle(pM + rM * p.offsetR(), pc * 0.85f,
			U"部品[{}] 右端（A/B 一律）"_fmt(i),
			m_edgeHandleDrag.kind == EdgeHandleDrag::Kind::PartRight && m_edgeHandleDrag.idx == i,
			Shape::Square);

		// 四隅ハンドル: A 側=暖色, B 側=寒色
		drawHandle(pA + rA * p.offsetA_L, sideTint(true,  false),
			U"部品[{}] A端左 {:.2f}m / 個別調整"_fmt(i, p.offsetA_L),
			m_edgeHandleDrag.kind == EdgeHandleDrag::Kind::PartAL && m_edgeHandleDrag.idx == i,
			Shape::Square);
		drawHandle(pA + rA * p.offsetA_R, sideTint(true,  true),
			U"部品[{}] A端右 {:.2f}m / 個別調整"_fmt(i, p.offsetA_R),
			m_edgeHandleDrag.kind == EdgeHandleDrag::Kind::PartAR && m_edgeHandleDrag.idx == i,
			Shape::Square);
		drawHandle(pB + rB * p.offsetB_L, sideTint(false, false),
			U"部品[{}] B端左 {:.2f}m / 個別調整"_fmt(i, p.offsetB_L),
			m_edgeHandleDrag.kind == EdgeHandleDrag::Kind::PartBL && m_edgeHandleDrag.idx == i,
			Shape::Square);
		drawHandle(pB + rB * p.offsetB_R, sideTint(false, true),
			U"部品[{}] B端右 {:.2f}m / 個別調整"_fmt(i, p.offsetB_R),
			m_edgeHandleDrag.kind == EdgeHandleDrag::Kind::PartBR && m_edgeHandleDrag.idx == i,
			Shape::Square);
	}

	// Lane: 中央=円(移動), L/R side=四角(両端幅), 四隅=四角(個別幅)
	if (showLane)
	{
		const auto& L = edge->lanes[laneIdxSel];
		const float sA = edge->cutoffA;
		const float sB = Max(0.0f, bez.totalLength - edge->cutoffB);
		const float sMid = (sA + sB) * 0.5f;
		const Vec3 pA = bez.positionAt(sA);
		const Vec3 pB = bez.positionAt(sB);
		const Vec3 pM = bez.positionAt(sMid);
		const Vec3 rA = tangentToRight(bez.tangentAt(sA));
		const Vec3 rB = tangentToRight(bez.tangentAt(sB));
		const Vec3 rM = tangentToRight(bez.tangentAt(sMid));

		// 中央（位置移動）
		const float midL = (L.offsetA_L + L.offsetB_L) * 0.5f;
		const float midR = (L.offsetA_R + L.offsetB_R) * 0.5f;
		drawHandle(pM + rM * ((midL + midR) * 0.5f), ColorF{0.9, 0.9, 1.0},
			U"車線位置 / ドラッグで左右移動"_fmt,
			m_edgeHandleDrag.kind == EdgeHandleDrag::Kind::LaneCenter, Shape::Circle);
		// L/R side（両端同時幅）
		drawHandle(pM + rM * midL, ColorF{1.0, 0.85, 0.3},
			U"車線左端（両端同時） / ドラッグで幅変更"_fmt,
			m_edgeHandleDrag.kind == EdgeHandleDrag::Kind::LaneLeftSide, Shape::Square);
		drawHandle(pM + rM * midR, ColorF{0.3, 0.85, 1.0},
			U"車線右端（両端同時） / ドラッグで幅変更"_fmt,
			m_edgeHandleDrag.kind == EdgeHandleDrag::Kind::LaneRightSide, Shape::Square);

		// 四隅: A 側=暖色、B 側=寒色
		drawHandle(pA + rA * L.offsetA_L, sideTint(true,  false),
			U"A端左 {:.2f}m / ドラッグで幅変更"_fmt(L.offsetA_L),
			m_edgeHandleDrag.kind == EdgeHandleDrag::Kind::LaneAL, Shape::Square);
		drawHandle(pA + rA * L.offsetA_R, sideTint(true,  true),
			U"A端右 {:.2f}m / ドラッグで幅変更"_fmt(L.offsetA_R),
			m_edgeHandleDrag.kind == EdgeHandleDrag::Kind::LaneAR, Shape::Square);
		drawHandle(pB + rB * L.offsetB_L, sideTint(false, false),
			U"B端左 {:.2f}m / ドラッグで幅変更"_fmt(L.offsetB_L),
			m_edgeHandleDrag.kind == EdgeHandleDrag::Kind::LaneBL, Shape::Square);
		drawHandle(pB + rB * L.offsetB_R, sideTint(false, true),
			U"B端右 {:.2f}m / ドラッグで幅変更"_fmt(L.offsetB_R),
			m_edgeHandleDrag.kind == EdgeHandleDrag::Kind::LaneBR, Shape::Square);
	}
}
