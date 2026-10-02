#include "GameScene.hpp"
#include "WorldSelection.hpp"
#include "RoadSelection.hpp"
#include "../road/RoadGeometry.hpp"
#include "../railway/TrainConsist.hpp"
#include "../ui/NavigationHeader.hpp"
#include "../ui/KeyboardActions.hpp"
#include "../gen/RoadTerrainFit.hpp"
#include "../gen/RoadAlignment.hpp"
#include "EdgeSectionState.hpp"
#include "../world/LandPlot.hpp"
#include "../ui/PanelWidget.hpp"

// =============================================================================
// ヘルパー
// =============================================================================

constexpr double kPanelMarginRight = 10.0;
constexpr double kPanelMarginTop   = 10.0;

Vec2 GameScene::panelRightPos(StringView panelId) const
{
	const double w = m_panelManager.getSize(panelId).x;
	return { Scene::Width() - w - kPanelMarginRight, kPanelMarginTop };
}

GameScene::ElevatedHitResult GameScene::raycastElevated(Vec2 screenPos) const
{
	ElevatedHitResult result;
	const Ray ray = m_camera.screenToRay(screenPos);
	const Float3 ro = ray.origin;
	const Float3 rd = ray.direction;
	if (rd.y >= -1e-6f) return result;

	// 高架ノード
	for (const auto& node : m_network.nodes())
	{
		if (node.id < 0 || !m_network.isNodeElevated(node.id)) continue;
		const float planeY = static_cast<float>(node.position.y + kRoadSurfaceLift);
		const float t = (planeY - ro.y) / rd.y;
		if (t < 0) continue;
		const float hx = ro.x + rd.x * t;
		const float hz = ro.z + rd.z * t;
		const float dx = hx - static_cast<float>(node.position.x);
		const float dz = hz - static_cast<float>(node.position.z);
		if (dx * dx + dz * dz < 20.0f * 20.0f)
		{
			result.nodeId = node.id;
			return result;
		}
	}

	// 高架エッジ
	for (const auto& edge : m_network.edges())
	{
		if (edge.id < 0 || !edge.useElevation) continue;
		const auto bez = m_network.getBezier(edge.id);
		if (!bez) continue;
		const auto* nA = m_network.getNode(edge.nodeA);
		const auto* nB = m_network.getNode(edge.nodeB);
		if (!nA || !nB) continue;
		const float planeY = static_cast<float>(
			(nA->position.y + nB->position.y) * 0.5 + kRoadSurfaceLift);
		const float t = (planeY - ro.y) / rd.y;
		if (t < 0) continue;
		const float hx = ro.x + rd.x * t;
		const float hz = ro.z + rd.z * t;
		for (int si = 0; si <= 10; ++si)
		{
			const float s = bez->totalLength * (si / 10.0f);
			const Vec3 p = bez->positionAt(s);
			const float ddx = hx - static_cast<float>(p.x);
			const float ddz = hz - static_cast<float>(p.z);
			if (ddx * ddx + ddz * ddz < 15.0f * 15.0f)
			{
				result.edgeId = edge.id;
				return result;
			}
		}
	}

	return result;
}

// =============================================================================
// 入力処理 (GameScene のメソッド分割)
// =============================================================================

void GameScene::handleInput()
{
	// ゲーム内入力の仲裁をここに集約し、UI フォーカス・一時停止・編集モードの優先順位を先に確定する。
	// テキスト入力フォーカス中はゲーム入力を抑制（ESC のみ通す）
	if (GameInput::keyboardBlocked())
	{
		if (KeyEscape.down() || GameInput::buffer.down(KeyEscape.code())) { GameInput::releaseTextFocus(); }
		return;
	}

	if (!m_showPauseMenu && GameInput::down(KeyU))
	{
		toggleUnderground();
		return;
	}

	if (m_mode == EditMode::RoadPlan && GameInput::down(KeyEscape) && !m_draftRoadPlan.editor.points().isEmpty())
	{
		clearDraftRoadPlan();
		return;
	}

	if (!m_showPauseMenu && GameInput::down(KeyEscape) && m_panelManager.isVisible(U"rail_timetable"))
	{
		setRailTimetableVisible(false); return;
	}
	// ---- ESC: ポーズメニュートグル ----
	if (GameInput::down(KeyEscape))
	{
		if (m_showPauseMenu)
		{
			resumeFromPauseMenu();
		}
		else if (m_mode != EditMode::None
			|| m_selection.kind != SelectionKind::None)
		{
			if (m_mode != EditMode::None)
			{
				if (m_mode == EditMode::RoadPlan)
					clearDraftRoadPlan();
				m_mode               = EditMode::None;
				m_drawStartNode      = none;
				m_rectStart          = none;
				m_trainDrawStartNode = none;
				m_sandboxDragNode    = none;
				m_sandboxDragCtrl    = none;
				m_editingRouteId     = -1;
				m_zoneManager.showOverlay = false;
			}
			clearSelection();
			m_panelManager.hide(U"edge_info");
			m_panelManager.hide(U"node_info");
			m_panelManager.hide(U"guide_sign_edit");
			m_panelManager.hide(U"signal_edit");
			m_panelManager.hide(U"draw_template");
			m_panelManager.hide(U"route_info");
		}
		else
		{
			m_showPauseMenu = true;
			m_pauseMenu.selected=0;
			m_pauseResumeSpeed=m_clock.speed;
			if (m_clock.speed != TimeSpeed::Paused)
			{
				m_prevSpeed   = m_clock.speed;
				m_clock.speed = TimeSpeed::Paused;
			}
		}
	}

	// ポーズメニュー表示中は他の入力をブロック
	if (m_showPauseMenu) return;

	// ---- F3 コマンド ----
	if (GameInput::pressed(KeyF3) && GameInput::down(KeyR))
	{
		if (GameInput::down(KeyR))
		{
			Console << U"[Reload] Reloading all assets...";
			m_roadRenderer.loadAssets();
			m_roadRenderer.invalidateAllCaches();
			Console << U"[Reload] Done.";
		}
		return;  // F3 押下中は通常操作を無効化
	}

	if (GameInput::down(KeyH))
	{
		setRailTimetableVisible(!m_panelManager.isVisible(U"rail_timetable")); return;
	}
	if (MouseL.down() && !m_panelManager.blocksMouseInput() && !m_uiRenderer.isMouseOnHud()
		&& m_routeSignRenderer.hitTest(Cursor::PosF())) { handleSelectionClick();return; }
	if (handleDrivingShortcuts()) { return; }

	if (GameInput::down(KeySpace))
	{
		if (m_clock.speed == TimeSpeed::Paused)
			m_clock.speed = m_prevSpeed;
		else
		{
			m_prevSpeed   = m_clock.speed;
			m_clock.speed = TimeSpeed::Paused;
		}
	}

	if (GameInput::down(KeyTab))
		m_zoneManager.showOverlay = !m_zoneManager.showOverlay;

	if (m_sandboxActive && GameInput::pressed(KeyControl) && GameInput::down(KeyR))
	{
		if (m_mode == EditMode::RoadPlan)
			clearDraftRoadPlan();
		m_mode = (m_mode == EditMode::RoadDraw) ? EditMode::None : EditMode::RoadDraw;
		m_drawStartNode = none;
		m_drawElevation = 0.0f;
		m_rectStart     = none;
		if (m_mode == EditMode::RoadDraw)
			m_panelManager.show(U"draw_template", U"道路敷設デバッグ", panelRightPos(U"draw_template"));
		else
			m_panelManager.hide(U"draw_template");
	}
	else if (GameInput::down(KeyR))
	{
		const bool enable = (m_mode != EditMode::RoadPlan || !m_panelManager.isVisible(U"draw_template"));
		if (!enable)
		{
			clearDraftRoadPlan();
			m_mode = EditMode::None;
			m_panelManager.hide(U"draw_template");
		}
		else
		{
			m_mode = EditMode::RoadPlan;
			m_drawStartNode = none;
			m_drawElevation = 0.0f;
			m_rectStart = none;
			m_autoPlaceMode = false;
			m_autoPlaceStart = none;
			m_panelManager.show(U"draw_template", U"道路計画  [R]", panelRightPos(U"draw_template"));
			m_roadPlanSnapIndex.rebuild(m_network);
			if (m_draftRoadPlan.editor.points().isEmpty()) { m_drawTemplate = RoadPlanDraft::makeRoadTemplate(m_draftRoadPlan.preset); }
			clearSelection();
		}
	}
	if (GameInput::down(KeyZ) && !GameInput::pressed(KeyControl))
	{
		setZonePaintMode(m_mode!=EditMode::ZonePaint);
	}

	if (m_mode == EditMode::ZonePaint)
	{
		if (GameInput::down(Key1)) m_paintZone = ZoneType::UrbanControl;
		if (GameInput::down(Key2)) m_paintZone = ZoneType::LowResidential;
		if (GameInput::down(Key3)) m_paintZone = ZoneType::Residential;
		if (GameInput::down(Key4)) m_paintZone = ZoneType::Commercial;
		if (GameInput::down(Key5)) m_paintZone = ZoneType::Industrial;
		if (GameInput::down(Key6)) m_paintZone = ZoneType::Agriculture;
		if (GameInput::down(Key0)) m_paintZone = ZoneType::Unzoned;
	}
	else
	{
		if (GameInput::down(Key1)) { m_prevSpeed = TimeSpeed::x1; m_clock.speed = TimeSpeed::x1; }
		if (GameInput::down(Key2)) { m_prevSpeed = TimeSpeed::x2; m_clock.speed = TimeSpeed::x2; }
		if (GameInput::down(Key3)) { m_prevSpeed = TimeSpeed::x4; m_clock.speed = TimeSpeed::x4; }
		if (GameInput::down(Key0)) { m_prevSpeed = m_clock.speed != TimeSpeed::Paused ? m_clock.speed : m_prevSpeed;
		                   m_clock.speed = TimeSpeed::Paused; }
	}

	if (GameInput::down(KeyT) && m_simGraph)
		m_vehicleManager.spawnRandom(*m_simGraph);

	if (GameInput::down(KeyF))
		m_camera.cycleMode();

	if (GameInput::down(KeyG))
	{
		if (m_mode == EditMode::RoadPlan)
			clearDraftRoadPlan();
		m_mode = (m_mode == EditMode::TerrainEdit) ? EditMode::None : EditMode::TerrainEdit;
		m_drawStartNode = none;
		m_rectStart     = none;
	}

	if (m_mode == EditMode::TerrainEdit && GameInput::pressed(KeyControl))
	{
		const double wheel = Mouse::Wheel();
		m_terrainBrushRadius = Clamp(
			m_terrainBrushRadius + static_cast<float>(wheel * -20.0),
			20.0f, 400.0f);
	}

	if (GameInput::down(KeyX))
	{
		if (m_mode == EditMode::RoadPlan)
			clearDraftRoadPlan();
		m_mode = (m_mode == EditMode::TrainDraw) ? EditMode::None : EditMode::TrainDraw;
		m_trainDrawStartNode = none;
	}

	if (GameInput::down(KeyB))
	{
		if (m_mode == EditMode::RoadPlan)
			clearDraftRoadPlan();
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
			m_editingRouteId = -1;
		}
	}

	if (m_sandboxActive && GameInput::down(KeyV))
	{
		if (m_mode == EditMode::RoadPlan)
			clearDraftRoadPlan();
		m_mode = (m_mode == EditMode::SandboxEdit) ? EditMode::None : EditMode::SandboxEdit;
		m_sandboxDragNode = none;
		m_drawStartNode   = none;
		m_rectStart       = none;
	}

	if (GameInput::down(KeyN))
	{
		if (m_panelManager.isVisible(U"name_list"))
			m_panelManager.hide(U"name_list");
		else
			m_panelManager.show(U"name_list", U"地名リスト (N)", panelRightPos(U"name_list"));
	}

	// ノード選択中: PgUp/PgDown で Y 座標を上下移動
	if (selectedNodeId())
	{
		constexpr float kNodeYStep = 1.0f;
		const bool up   = GameInput::pressed(KeyPageUp);
		const bool down = GameInput::pressed(KeyPageDown);
		if (up || down)
		{
			if (auto* node = m_network.getNode(*selectedNodeId()))
			{
				const float dy = up ? kNodeYStep : -kNodeYStep;
				node->position.y += dy;

				// 接続エッジのコントロールポイント Y も同じ差分で移動し、elevation を自動判定
				for (const int eid : node->edgeIds())
				{
					if (auto* edge = m_network.getEdge(eid))
					{
						if (edge->nodeA == *selectedNodeId())
							edge->ctrlA.y += dy;
						else
							edge->ctrlB.y += dy;
						m_network.updateEdgeElevation(eid, m_world);
					}
				}

				m_roadRenderer.invalidateCachesAroundNode(*selectedNodeId(), m_network);
			}
		}
	}

	if (m_landParcelEditing && m_selectedLandParcel && m_mode==EditMode::None)
	{
		handleLandParcelEditInput();
		return;
	}

	if (StartScreenControls::layerButton(Scene::Size(), m_frameRateGraph.visible).contains(Cursor::PosF()) ||
		m_uiRenderer.isMouseOnHud() || NavigationHeader::placeBounds(Scene::Size()).contains(Cursor::PosF()))
	{
		return;
	}

	if      (m_mode == EditMode::RoadPlan)     handleRoadPlan();
	else if (m_mode == EditMode::RoadDraw)     handleRoadDraw();
	else if (m_mode == EditMode::ZonePaint)    handleZonePaint();
	else if (m_mode == EditMode::BusRouteDraw) handleBusRouteDraw();
	else if (m_mode == EditMode::TerrainEdit)  handleTerrainEdit();
	else if (m_mode == EditMode::TrainDraw)    handleTrainDraw();
	else if (m_mode == EditMode::SandboxEdit)  handleSandboxEdit();
	else if (m_mode == EditMode::None)
	{
		// 通常モードでは 3D ハンドル操作を最優先し、未消費時だけ通常の選択クリックに流す。
		// エッジ選択中は 3D ハンドルのドラッグ/ヒットテストを優先
		const bool handleConsumed = handleEdgeHandleInput();
		if (!handleConsumed
			&& MouseL.down() && m_cursorGroundPos && !m_panelManager.blocksMouseInput())
			handleSelectionClick();
	}
}

// =============================================================================
// 3D エッジ編集ハンドル（Cutoff A/B）
// =============================================================================

bool GameScene::handleEdgeHandleInput()
{
	if (m_selection.kind != SelectionKind::Edge)
	{
		m_edgeHandleDrag.kind = EdgeHandleDrag::Kind::None;
		return false;
	}
	RoadEdge* edge = m_network.getEdge(m_selection.id);
	if (!edge)
	{
		m_edgeHandleDrag.kind = EdgeHandleDrag::Kind::None;
		return false;
	}
	const auto bezOpt = m_network.getBezier(edge->id);
	if (!bezOpt)
	{
		return false;
	}
	const CubicBezier& bez = *bezOpt;
	const auto& cam = m_camera.camera3D();

	// 各ハンドル種別のワールド位置 / ドラッグ方向を算出
	auto handleWorld = [&](EdgeHandleDrag::Kind k, int idx, Vec3& outPos, Vec3& outDir) -> bool
	{
		switch (k)
		{
		case EdgeHandleDrag::Kind::CutoffA:
			outPos = bez.positionAt(edge->cutoffA);
			outDir = bez.tangentAt(edge->cutoffA);
			return true;
		case EdgeHandleDrag::Kind::CutoffB:
		{
			const float s = Max(0.0f, bez.totalLength - edge->cutoffB);
			outPos = bez.positionAt(s);
			outDir = -bez.tangentAt(s);
			return true;
		}
		case EdgeHandleDrag::Kind::PartCenter:
		case EdgeHandleDrag::Kind::PartLeft:
		case EdgeHandleDrag::Kind::PartRight:
		{
			if (idx < 0 || idx >= static_cast<int>(edge->parts.size()))
			{
				return false;
			}
			const auto& p = edge->parts[idx];
			const float s = bez.totalLength * 0.5f;
			const Vec3 center = bez.positionAt(s);
			const Vec3 right = tangentToRight(bez.tangentAt(s));
			float localOffs = (p.offsetL() + p.offsetR()) * 0.5f;
			if      (k == EdgeHandleDrag::Kind::PartLeft)  localOffs = p.offsetL();
			else if (k == EdgeHandleDrag::Kind::PartRight) localOffs = p.offsetR();
			outPos = center + right * localOffs;
			outDir = right;
			return true;
		}
		case EdgeHandleDrag::Kind::PartAL:
		case EdgeHandleDrag::Kind::PartAR:
		case EdgeHandleDrag::Kind::PartBL:
		case EdgeHandleDrag::Kind::PartBR:
		{
			if (idx < 0 || idx >= static_cast<int>(edge->parts.size()))
			{
				return false;
			}
			const auto& p = edge->parts[idx];
			const bool atA = (k == EdgeHandleDrag::Kind::PartAL || k == EdgeHandleDrag::Kind::PartAR);
			const float s  = atA ? edge->cutoffA : Max(0.0f, bez.totalLength - edge->cutoffB);
			const Vec3 p0  = bez.positionAt(s);
			const Vec3 right = tangentToRight(bez.tangentAt(s));
			float offs = 0.0f;
			if      (k == EdgeHandleDrag::Kind::PartAL) offs = p.offsetA_L;
			else if (k == EdgeHandleDrag::Kind::PartAR) offs = p.offsetA_R;
			else if (k == EdgeHandleDrag::Kind::PartBL) offs = p.offsetB_L;
			else                                         offs = p.offsetB_R;
			outPos = p0 + right * offs;
			outDir = right;
			return true;
		}
		case EdgeHandleDrag::Kind::LaneCenter:
		case EdgeHandleDrag::Kind::LaneLeftSide:
		case EdgeHandleDrag::Kind::LaneRightSide:
		{
			if (idx < 0 || idx >= static_cast<int>(edge->lanes.size()))
			{
				return false;
			}
			const auto& L = edge->lanes[idx];
			const float s = bez.totalLength * 0.5f;
			const Vec3 center = bez.positionAt(s);
			const Vec3 right = tangentToRight(bez.tangentAt(s));
			const float midL = (L.offsetA_L + L.offsetB_L) * 0.5f;
			const float midR = (L.offsetA_R + L.offsetB_R) * 0.5f;
			float offs = (midL + midR) * 0.5f;
			if      (k == EdgeHandleDrag::Kind::LaneLeftSide)  offs = midL;
			else if (k == EdgeHandleDrag::Kind::LaneRightSide) offs = midR;
			outPos = center + right * offs;
			outDir = right;
			return true;
		}
		case EdgeHandleDrag::Kind::LaneAL:
		case EdgeHandleDrag::Kind::LaneAR:
		case EdgeHandleDrag::Kind::LaneBL:
		case EdgeHandleDrag::Kind::LaneBR:
		{
			if (idx < 0 || idx >= static_cast<int>(edge->lanes.size()))
			{
				return false;
			}
			const auto& L = edge->lanes[idx];
			const bool atA = (k == EdgeHandleDrag::Kind::LaneAL || k == EdgeHandleDrag::Kind::LaneAR);
			const float s = atA ? edge->cutoffA : Max(0.0f, bez.totalLength - edge->cutoffB);
			const Vec3 p0 = bez.positionAt(s);
			const Vec3 right = tangentToRight(bez.tangentAt(s));
			float offs = 0;
			if      (k == EdgeHandleDrag::Kind::LaneAL) offs = L.offsetA_L;
			else if (k == EdgeHandleDrag::Kind::LaneAR) offs = L.offsetA_R;
			else if (k == EdgeHandleDrag::Kind::LaneBL) offs = L.offsetB_L;
			else                                         offs = L.offsetB_R;
			outPos = p0 + right * offs;
			outDir = right;
			return true;
		}
		default: return false;
		}
	};

	// ドラッグ中: スクリーン上のマウス移動を dir 方向に投影して値を更新
	if (m_edgeHandleDrag.kind != EdgeHandleDrag::Kind::None && m_edgeHandleDrag.edgeId == edge->id)
	{
		if (!MouseL.pressed())
		{
			m_edgeHandleDrag.kind = EdgeHandleDrag::Kind::None;
			return true;
		}
		// マウスが動いていないフレームはキャッシュ無効化を含む更新自体をスキップ
		if (Cursor::Delta().isZero())
		{
			return true;
		}

		Vec3 wp, dir;
		if (!handleWorld(m_edgeHandleDrag.kind, m_edgeHandleDrag.idx, wp, dir))
		{
			m_edgeHandleDrag.kind = EdgeHandleDrag::Kind::None;
			return true;
		}

		const Vec2 sp0 = cam.worldToScreenPoint(wp).xy();
		const Vec2 sp1 = cam.worldToScreenPoint(wp + dir).xy();
		const Vec2 ds = sp1 - sp0;
		if (ds.lengthSq() < 1e-6)
		{
			return true;
		}
		const Vec2 dsN = ds.normalized();
		const double mPerPx = 1.0 / ds.length();
		const Vec2 mouseDelta = Cursor::PosF() - m_edgeHandleDrag.anchorScreen;
		const float deltaM = static_cast<float>(mouseDelta.dot(dsN) * mPerPx);

		const int pi = m_edgeHandleDrag.idx;
		const int li = m_edgeHandleDrag.idx;
		switch (m_edgeHandleDrag.kind)
		{
		case EdgeHandleDrag::Kind::CutoffA:
			edge->cutoffA = Clamp(m_edgeHandleDrag.anchorValue + deltaM, 0.0f, bez.totalLength * 0.45f);
			break;
		case EdgeHandleDrag::Kind::CutoffB:
			edge->cutoffB = Clamp(m_edgeHandleDrag.anchorValue + deltaM, 0.0f, bez.totalLength * 0.45f);
			break;
		case EdgeHandleDrag::Kind::PartCenter:
			// 4 隅を一律に平行移動
			if (pi >= 0 && pi < static_cast<int>(edge->parts.size()))
			{
				auto& pp = edge->parts[pi];
				pp.offsetA_L = m_edgeHandleDrag.anchorValue + deltaM;
				pp.offsetA_R = m_edgeHandleDrag.anchorA    + deltaM;
				pp.offsetB_L = m_edgeHandleDrag.anchorB    + deltaM;
				pp.offsetB_R = m_edgeHandleDrag.anchorC    + deltaM;
			}
			break;
		case EdgeHandleDrag::Kind::PartLeft:
			// 左端ドラッグ（A/B 両端の左端を一律移動）: 右端固定
			if (pi >= 0 && pi < static_cast<int>(edge->parts.size()))
			{
				auto& pp = edge->parts[pi];
				pp.offsetA_L = m_edgeHandleDrag.anchorValue + deltaM;
				pp.offsetB_L = m_edgeHandleDrag.anchorA    + deltaM;
			}
			break;
		case EdgeHandleDrag::Kind::PartRight:
			// 右端ドラッグ（A/B 両端の右端を一律移動）: 左端固定
			if (pi >= 0 && pi < static_cast<int>(edge->parts.size()))
			{
				auto& pp = edge->parts[pi];
				pp.offsetA_R = m_edgeHandleDrag.anchorValue + deltaM;
				pp.offsetB_R = m_edgeHandleDrag.anchorA    + deltaM;
			}
			break;
		case EdgeHandleDrag::Kind::PartAL:
			if (pi >= 0 && pi < static_cast<int>(edge->parts.size()))
			{
				edge->parts[pi].offsetA_L = m_edgeHandleDrag.anchorValue + deltaM;
			}
			break;
		case EdgeHandleDrag::Kind::PartAR:
			if (pi >= 0 && pi < static_cast<int>(edge->parts.size()))
			{
				edge->parts[pi].offsetA_R = m_edgeHandleDrag.anchorValue + deltaM;
			}
			break;
		case EdgeHandleDrag::Kind::PartBL:
			if (pi >= 0 && pi < static_cast<int>(edge->parts.size()))
			{
				edge->parts[pi].offsetB_L = m_edgeHandleDrag.anchorValue + deltaM;
			}
			break;
		case EdgeHandleDrag::Kind::PartBR:
			if (pi >= 0 && pi < static_cast<int>(edge->parts.size()))
			{
				edge->parts[pi].offsetB_R = m_edgeHandleDrag.anchorValue + deltaM;
			}
			break;
		case EdgeHandleDrag::Kind::LaneCenter:
			if (li >= 0 && li < static_cast<int>(edge->lanes.size()))
			{
				auto& L = edge->lanes[li];
				L.offsetA_L = m_edgeHandleDrag.anchorValue + deltaM;
				L.offsetA_R = m_edgeHandleDrag.anchorA    + deltaM;
				L.offsetB_L = m_edgeHandleDrag.anchorB    + deltaM;
				L.offsetB_R = m_edgeHandleDrag.anchorC    + deltaM;
			}
			break;
		case EdgeHandleDrag::Kind::LaneLeftSide:
			if (li >= 0 && li < static_cast<int>(edge->lanes.size()))
			{
				edge->lanes[li].offsetA_L = m_edgeHandleDrag.anchorValue + deltaM;
				edge->lanes[li].offsetB_L = m_edgeHandleDrag.anchorA    + deltaM;
			}
			break;
		case EdgeHandleDrag::Kind::LaneRightSide:
			if (li >= 0 && li < static_cast<int>(edge->lanes.size()))
			{
				edge->lanes[li].offsetA_R = m_edgeHandleDrag.anchorValue + deltaM;
				edge->lanes[li].offsetB_R = m_edgeHandleDrag.anchorA    + deltaM;
			}
			break;
		case EdgeHandleDrag::Kind::LaneAL:
			if (li >= 0 && li < static_cast<int>(edge->lanes.size()))
			{
				edge->lanes[li].offsetA_L = m_edgeHandleDrag.anchorValue + deltaM;
			}
			break;
		case EdgeHandleDrag::Kind::LaneAR:
			if (li >= 0 && li < static_cast<int>(edge->lanes.size()))
			{
				edge->lanes[li].offsetA_R = m_edgeHandleDrag.anchorValue + deltaM;
			}
			break;
		case EdgeHandleDrag::Kind::LaneBL:
			if (li >= 0 && li < static_cast<int>(edge->lanes.size()))
			{
				edge->lanes[li].offsetB_L = m_edgeHandleDrag.anchorValue + deltaM;
			}
			break;
		case EdgeHandleDrag::Kind::LaneBR:
			if (li >= 0 && li < static_cast<int>(edge->lanes.size()))
			{
				edge->lanes[li].offsetB_R = m_edgeHandleDrag.anchorValue + deltaM;
			}
			break;
		default: break;
		}
		// メッシュキャッシュ無効化
		m_roadRenderer.invalidateEdgeCache(edge->id, edge->nodeA, edge->nodeB);
		m_roadRenderer.invalidateCachesAroundNode(edge->nodeA, m_network);
		m_roadRenderer.invalidateCachesAroundNode(edge->nodeB, m_network);
		return true;
	}

	// ヒットテスト
	if (!MouseL.down() || m_panelManager.blocksMouseInput())
	{
		return false;
	}
	const Vec2 cur = Cursor::PosF();
	constexpr double kHitRadius = 10.0;

	auto beginDrag = [&](EdgeHandleDrag::Kind k, int idx,
	                     float v, float a = 0.0f, float b = 0.0f, float c = 0.0f)
	{
		m_edgeHandleDrag.kind = k;
		m_edgeHandleDrag.edgeId = edge->id;
		m_edgeHandleDrag.idx   = idx;
		m_edgeHandleDrag.anchorScreen = cur;
		m_edgeHandleDrag.anchorValue = v;
		m_edgeHandleDrag.anchorA = a;
		m_edgeHandleDrag.anchorB = b;
		m_edgeHandleDrag.anchorC = c;
	};
	auto hitAt = [&](EdgeHandleDrag::Kind k, int idx) -> bool
	{
		Vec3 wp, dir;
		if (!handleWorld(k, idx, wp, dir))
		{
			return false;
		}
		const Vec2 sp = cam.worldToScreenPoint(wp).xy();
		return sp.distanceFrom(cur) <= kHitRadius;
	};

	// Cutoff
	if (hitAt(EdgeHandleDrag::Kind::CutoffA, -1)) { beginDrag(EdgeHandleDrag::Kind::CutoffA, -1, edge->cutoffA); return true; }
	if (hitAt(EdgeHandleDrag::Kind::CutoffB, -1)) { beginDrag(EdgeHandleDrag::Kind::CutoffB, -1, edge->cutoffB); return true; }
	// Part/Lane は排他。Lane 選択中は Part のヒットは取らない
	const int partIdx = EdgeSectionState::selectedPart;
	const int laneIdx = EdgeSectionState::selectedLane;
	const bool laneActive = (laneIdx >= 0 && laneIdx < static_cast<int>(edge->lanes.size()));
	const bool partActive = (!laneActive) && (partIdx >= 0 && partIdx < static_cast<int>(edge->parts.size()));
	if (partActive)
	{
		const auto& p = edge->parts[partIdx];
		// 四隅ハンドル優先
		if (hitAt(EdgeHandleDrag::Kind::PartAL, partIdx)) { beginDrag(EdgeHandleDrag::Kind::PartAL, partIdx, p.offsetA_L); return true; }
		if (hitAt(EdgeHandleDrag::Kind::PartAR, partIdx)) { beginDrag(EdgeHandleDrag::Kind::PartAR, partIdx, p.offsetA_R); return true; }
		if (hitAt(EdgeHandleDrag::Kind::PartBL, partIdx)) { beginDrag(EdgeHandleDrag::Kind::PartBL, partIdx, p.offsetB_L); return true; }
		if (hitAt(EdgeHandleDrag::Kind::PartBR, partIdx)) { beginDrag(EdgeHandleDrag::Kind::PartBR, partIdx, p.offsetB_R); return true; }
		// 一律操作ハンドル
		if (hitAt(EdgeHandleDrag::Kind::PartLeft, partIdx))
		{ beginDrag(EdgeHandleDrag::Kind::PartLeft, partIdx, p.offsetA_L, p.offsetB_L); return true; }
		if (hitAt(EdgeHandleDrag::Kind::PartRight, partIdx))
		{ beginDrag(EdgeHandleDrag::Kind::PartRight, partIdx, p.offsetA_R, p.offsetB_R); return true; }
		if (hitAt(EdgeHandleDrag::Kind::PartCenter, partIdx))
		{ beginDrag(EdgeHandleDrag::Kind::PartCenter, partIdx, p.offsetA_L, p.offsetA_R, p.offsetB_L, p.offsetB_R); return true; }
	}
	// Lane: 四隅 > L/R side > 中央 の優先順
	if (laneActive)
	{
		const auto& L = edge->lanes[laneIdx];
		if (hitAt(EdgeHandleDrag::Kind::LaneAL, laneIdx)) { beginDrag(EdgeHandleDrag::Kind::LaneAL, laneIdx, L.offsetA_L); return true; }
		if (hitAt(EdgeHandleDrag::Kind::LaneAR, laneIdx)) { beginDrag(EdgeHandleDrag::Kind::LaneAR, laneIdx, L.offsetA_R); return true; }
		if (hitAt(EdgeHandleDrag::Kind::LaneBL, laneIdx)) { beginDrag(EdgeHandleDrag::Kind::LaneBL, laneIdx, L.offsetB_L); return true; }
		if (hitAt(EdgeHandleDrag::Kind::LaneBR, laneIdx)) { beginDrag(EdgeHandleDrag::Kind::LaneBR, laneIdx, L.offsetB_R); return true; }
		if (hitAt(EdgeHandleDrag::Kind::LaneLeftSide, laneIdx))
		{ beginDrag(EdgeHandleDrag::Kind::LaneLeftSide, laneIdx, L.offsetA_L, L.offsetB_L); return true; }
		if (hitAt(EdgeHandleDrag::Kind::LaneRightSide, laneIdx))
		{ beginDrag(EdgeHandleDrag::Kind::LaneRightSide, laneIdx, L.offsetA_R, L.offsetB_R); return true; }
		if (hitAt(EdgeHandleDrag::Kind::LaneCenter, laneIdx))
		{ beginDrag(EdgeHandleDrag::Kind::LaneCenter, laneIdx,
			L.offsetA_L, L.offsetA_R, L.offsetB_L, L.offsetB_R); return true; }
	}
	return false;
}

// =============================================================================
// 通常モード クリック選択処理
// =============================================================================

void GameScene::handleSelectionClick()
{
	// ── 国道路線標識のヒットテスト（ノード・エッジより優先） ──
	if (const auto hitRoute = m_routeSignRenderer.hitTest(Vec2{ Cursor::Pos() }))
	{
		if (const RoadRoute* r = m_network.getRoute(*hitRoute))
		{
			selectRoute(*hitRoute);
			m_routeNameEditState = TextEditState{};
			m_routeNameEditState.text = r->name;
			m_panelManager.show(U"route_info",
				U"路線 #{}"_fmt(*hitRoute), panelRightPos(U"route_info"));
			m_panelManager.hide(U"edge_info");
			m_panelManager.hide(U"node_info");
			m_panelManager.hide(U"guide_sign_edit");
			m_panelManager.hide(U"signal_edit");
			m_panelManager.hide(U"building_info");
			return;
		}
	}

	// ワールド上の候補を優先順位つきで走査し、対応する情報パネルと選択状態を一貫して切り替える。
	// 車両 -> ノード -> エッジの優先順でクリック判定
	Optional<int> hitVehicleId,hitTrainId;
	{
		const Ray ray = m_camera.screenToRay(Vec2{ Cursor::Pos() });
		double bestDist = !m_underground && m_camera.mode() == CameraMode::Overview && m_cursorGroundPos
							  ? Vec3{ray.getOrigin()}.distanceFrom(*m_cursorGroundPos) + 1
							  : 1e9;
		for (const auto& train:m_trainManager.trains())
		{
			if (const auto distance = TrainConsist::hitDistance(train, m_trainNetwork, ray,
					[&](Vec3 point) { return !m_underground || SubsurfaceView::below(point, m_world); });
				distance && *distance < bestDist)
			{
				bestDist = *distance;
				hitTrainId = train.id;
			}
		}
		for (const auto& v : m_renderVehicles)
		{
			if (m_underground && !SubsurfaceView::below(v.position, m_world))
			{
				continue;
			}
			const Vec3 size = Vec3{ 2.0, 3.0, 5.0 };
			const Vec3 center = v.position + Vec3{ 0, size.y / 2, 0 };
			const Quaternion rot = Quaternion::RotateY(v.heading);
			const OrientedBox box{ center, size, rot };
			if (const auto d = box.intersects(ray))
			{
				if (*d < bestDist)
				{
					bestDist = *d;
					hitVehicleId = v.id; hitTrainId.reset();
				}
			}
		}
	}

	if (hitTrainId)
	{
		selectTrain(*hitTrainId);
		return;
	}

	if (hitVehicleId)
	{
		m_selectedVehicleId = hitVehicleId;
		clearSelection();
		m_panelManager.show(U"vehicle_info", U"車両 #{}"_fmt(*hitVehicleId),
			panelRightPos(U"vehicle_info"));
		m_panelManager.hide(U"edge_info");
		m_panelManager.hide(U"node_info");
		return;
	}

	if (m_selectedVehicleId)
	{
		m_selectedVehicleId = none;
		m_trackingVehicle = false;
		m_panelManager.hide(U"vehicle_info");
	}

	const Ray selectionRay = m_camera.screenToRay(Cursor::PosF());
	if (const auto station = SubsurfaceView::stationHit(m_world, m_trainNetwork, selectionRay, m_underground))
	{
		selectStation(*station->station);
		return;
	}
	if (m_underground)
	{
		if (const auto hit = m_subsurface.hit(selectionRay); hit && hit->edge)
		{
			selectEdge(*hit->edge);
			m_panelManager.show(U"edge_info", U"地下の道路・線路 #{}"_fmt(*hit->edge), panelRightPos(U"edge_info"));
		}
		else
		{
			clearSelection();
			m_panelManager.hide(U"edge_info");
		}
		return;
	}

	// ── 付帯設備（看板・信号）のヒットテスト（ノード/エッジより優先） ──
	constexpr float kInfraHitRadius = 10.0f;
	Optional<double> buildingDistance;
	const Ray surfaceRay = m_camera.screenToRay(Vec2{Cursor::Pos()});
	const auto hitBuilding = findBuildingAt(surfaceRay, &buildingDistance);
	const auto hitGuideSignId = findGuideSignAt(*m_cursorGroundPos, kInfraHitRadius);
	const auto hitSignalNodeId = findSignalAt(*m_cursorGroundPos, kInfraHitRadius);
	const Optional<double> guideDistance = buildingDistance && hitGuideSignId
		? m_roadRenderer.guideSignHitDistance(*hitGuideSignId, m_network, m_world, surfaceRay) : none;
	const Optional<double> signalDistance = buildingDistance && hitSignalNodeId
		? m_roadRenderer.signalHitDistance(*hitSignalNodeId, m_network, m_world, surfaceRay, m_camera.eyePosition()) : none;
	const auto surface = WorldSelection::choose(buildingDistance, hitGuideSignId.has_value(), guideDistance,
		hitSignalNodeId.has_value(), signalDistance);
	if (getData().playtest)
	{
		const String buildingCell = hitBuilding ? U"{},{},{},{}"_fmt(hitBuilding->chunkX, hitBuilding->chunkZ, hitBuilding->col, hitBuilding->row) : U"none";
		DBG_LOG(U"[PickDepth] frame={} lowSpec={} native={} cursor={} eye={} ray={} ground={} buildingCell={} buildingT={} guideId={} guideT={} signalId={} signalT={} kind={}"_fmt(
			m_playtestFrame, getData().lowSpec, Scene::Size(), Cursor::Pos(), m_camera.eyePosition(), Vec3{surfaceRay.direction.xyz()}, *m_cursorGroundPos,
			buildingCell, buildingDistance.value_or(-1), hitGuideSignId.value_or(-1), guideDistance.value_or(-1),
			hitSignalNodeId.value_or(-1), signalDistance.value_or(-1), static_cast<int>(surface)));
	}
	if (surface == WorldSelection::Surface::GuideSign && hitGuideSignId)
	{
		selectGuideSign(*hitGuideSignId);
		m_guideSignEditor.open(*hitGuideSignId, m_network);
		m_panelManager.show(U"guide_sign_edit",
			U"案内標識 #{}"_fmt(*hitGuideSignId), Vec2{ 20, 100 });
		m_panelManager.hide(U"edge_info");
		m_panelManager.hide(U"node_info");
		m_panelManager.hide(U"signal_edit");
		return;
	}
	if (surface == WorldSelection::Surface::Signal && hitSignalNodeId)
	{
		selectSignal(*hitSignalNodeId);
		m_panelManager.show(U"signal_edit",
			U"信号 N#{}"_fmt(*hitSignalNodeId), panelRightPos(U"signal_edit"));
		m_panelManager.hide(U"edge_info");
		m_panelManager.hide(U"node_info");
		m_panelManager.hide(U"guide_sign_edit");
		m_panelManager.hide(U"building_info");
		return;
	}

	// ── 建物のヒットテスト（道路より先に判定） ──
	{
		if (surface == WorldSelection::Surface::Building && hitBuilding)
		{
			selectBuilding(*hitBuilding);
			m_panelManager.show(U"building_info", U"建物", panelRightPos(U"building_info"));
			m_panelManager.hide(U"edge_info");
			m_panelManager.hide(U"node_info");
			m_panelManager.hide(U"guide_sign_edit");
			m_panelManager.hide(U"signal_edit");
			return;
		}
	}

	// 地上カーソルで検索（ノード・エッジ）
	auto hitNode = visibleNodeNear(*m_cursorGroundPos, 20.0f);
	auto hitEdge = m_network.findEdgeNear(*m_cursorGroundPos, 15.0f);
	Optional<Vec2> nodeScreenPosition;
	if (hitNode)
	{
		if (const auto* node = m_network.getNode(*hitNode))
		{
			nodeScreenPosition = m_camera.camera3D().worldToScreenPoint(node->position).xy();
		}
	}
	const auto roadTarget = RoadSelection::choose(nodeScreenPosition, Cursor::PosF(), hitEdge.has_value());
	if (roadTarget != RoadSelection::Target::Node) { hitNode = none; }
	if (roadTarget != RoadSelection::Target::Edge) { hitEdge = none; }

	bool hitElevated=false;
	// 高架面とのレイ交差で追加検索
	{
		const auto elev = raycastElevated(Vec2{ Cursor::Pos() });
		if (elev.nodeId) { hitNode = elev.nodeId; hitEdge = none; hitElevated=true; }
		else if (elev.edgeId) { hitNode=none; hitEdge = elev.edgeId; hitElevated=true; }
	}

	// The elevated road is in front of the ground parcel. Ground roads are excluded by the plot mesh.
	if (!hitElevated && selectLandParcelAt(Vec2{m_cursorGroundPos->x,m_cursorGroundPos->z})) { return; }

	if (hitNode)
	{
		selectNode(*hitNode);
		m_panelManager.show(U"node_info", U"道路ノード #{}"_fmt(*hitNode),
			panelRightPos(U"node_info"));
		m_panelManager.hide(U"edge_info");
		m_panelManager.hide(U"guide_sign_edit");
		m_panelManager.hide(U"signal_edit");
		m_panelManager.hide(U"building_info");
		recomputeGuideSignsAroundNode(*hitNode);
	}
	else if (hitEdge)
	{
		selectEdge(*hitEdge);
		m_panelManager.show(U"edge_info", U"道路エッジ #{}"_fmt(*hitEdge),
			panelRightPos(U"edge_info"));
		m_panelManager.hide(U"node_info");
		m_panelManager.hide(U"guide_sign_edit");
		m_panelManager.hide(U"signal_edit");
		m_panelManager.hide(U"building_info");
	}
	else
	{
		clearSelection();
		m_panelManager.hide(U"edge_info");
		m_panelManager.hide(U"node_info");
		m_panelManager.hide(U"guide_sign_edit");
		m_panelManager.hide(U"signal_edit");
		m_panelManager.hide(U"building_info");
	}
}

// =============================================================================
// モード別入力ハンドラ
// =============================================================================

std::pair<Vec3, Vec3> GameScene::calcRoadDrawControlPoints(int startNodeId, Vec3 endPos) const
{
	const RoadNode* startNode = m_network.getNode(startNodeId);
	if (!startNode) return { endPos, endPos };

	const Vec3 pA = startNode->position;
	const Vec3 mid = (pA + endPos) / 2.0;
	Vec3 ctrlB = mid;
	Vec3 ctrlA = mid;

	// 有効な接続エッジを列挙
	Array<int> validEdges;
	for (int eid : startNode->edgeIds())
		if (m_network.getEdge(eid)) validEdges << eid;

	if (validEdges.size() == 1)
	{
		// 既存エッジのCP → 始点方向の延長線上に配置
		const RoadEdge* prevEdge = m_network.getEdge(validEdges[0]);
		const Vec3 cpp = (prevEdge->nodeA == startNodeId) ? prevEdge->ctrlA : prevEdge->ctrlB;
		const Vec3 dir = pA - cpp;
		const double dirLen = dir.length();
		if (dirLen > 1e-6)
		{
			const double dist = pA.distanceFrom(endPos) / 3.0;
			ctrlA = pA + (dir / dirLen) * dist;
		}
	}

	return { ctrlA, ctrlB };
}

void GameScene::handleRoadDraw()
{
	// 道路敷設モードでは、自動敷設・テンプレート取得・既存道路への接続を同じ入力フローで扱う。
	// PgUp/PgDown: 高さオフセットを変更（キー操作はパネル上でも有効）
	{
		constexpr float kElevStep = 1.0f;
		if (GameInput::pressed(KeyPageUp))   m_drawElevation += kElevStep;
		if (GameInput::pressed(KeyPageDown)) m_drawElevation = Max(m_drawElevation - kElevStep, -100.0f);
	}

	// パネル上にカーソルがあるときはマウス操作をすべて吸収
	if (m_panelManager.blocksMouseInput()) return;

	// ────────────────────────────────────────────────────────────────
	// スタート/ゴール指定モード: 左クリックで2点指定して自動敷設
	// ────────────────────────────────────────────────────────────────
	if (m_autoPlaceMode)
	{
		// ESC でスタートをクリア（モードは継続）
		if (GameInput::down(KeyEscape))
		{
			m_autoPlaceStart = none;
			return;
		}

		if (MouseL.down() && m_cursorGroundPos)
		{
			const Vec3 clickPos = *m_cursorGroundPos;

			// 水域チェック
			if (m_world.sampleHeight(static_cast<float>(clickPos.x),
			                          static_cast<float>(clickPos.z)) < 0.0f)
			{
				Console << U"[AutoPlace] 水域の地点は選択できません";
				return;
			}

			if (!m_autoPlaceStart)
			{
				m_autoPlaceStart = clickPos;
			}
			else
			{
				invokeAutoPlace(*m_autoPlaceStart, clickPos);
				m_autoPlaceStart = none;
			}
		}
		// 通常ドロー操作は抑止
		return;
	}

	// ホイールクリック: 既存道路の構成をテンプレートにコピー
	if (MouseM.down() && m_cursorGroundPos)
	{
		auto hitEdge = m_network.findEdgeNear(*m_cursorGroundPos, 15.0f);
		if (!hitEdge)
			hitEdge = raycastElevated(Vec2{ Cursor::Pos() }).edgeId;
		if (hitEdge)
		{
			const RoadEdge* src = m_network.getEdge(*hitEdge);
			if (src)
			{
				m_drawTemplate.roadType   = src->roadType;
				m_drawTemplate.speedLimit = src->speedLimit;
				m_drawTemplate.parts      = src->parts;
				m_drawTemplate.lanes      = src->lanes;
			}
		}
	}

	// 左クリック: 道路設置
	if (MouseL.down() && m_cursorGroundPos)
	{
		Vec3 clickPos = *m_cursorGroundPos;
		clickPos.y += m_drawElevation;

		// ノード決定: 既存ノード → エッジ分割 → 新規作成
		auto nearNode = visibleNodeNear(clickPos, 20.0f);
		int nodeId;
		if (nearNode)
		{
			nodeId = *nearNode;
		}
		else
		{
			auto edgeHit = m_network.findEdgeNearDetailed(*m_cursorGroundPos, 15.0f);
			if (edgeHit)
			{
				nodeId = m_network.splitEdgeAt(edgeHit->first, edgeHit->second);
				if (nodeId < 0)
					nodeId = m_network.addNode(clickPos);
				else
				{
					notifyNetworkChanged({ nodeId });
					m_roadRenderer.invalidateCachesAroundNode(nodeId, m_network);
				}
			}
			else
			{
				nodeId = m_network.addNode(clickPos);
			}
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
				const auto [ctrlA, ctrlB] = calcRoadDrawControlPoints(from, m_network.getNode(nodeId)->position);
				const int numLanes = static_cast<int>(m_drawTemplate.lanes.size());
				auto newEdgeId = m_network.addEdgeWithIntersection(
					from, nodeId, ctrlA, ctrlB, m_drawTemplate.roadType, numLanes);
				if (newEdgeId)
				{
					m_network.applyEdgeTemplate(*newEdgeId, m_drawTemplate);
					m_network.smoothCurveAt(*newEdgeId, from);
					m_network.updateEdgeElevation(*newEdgeId, m_world);
					if (RoadEdge* newEdge = m_network.getEdge(*newEdgeId))
					{
						newEdge->edgeState = EdgeState::Open;
						if (newEdge->useElevation)
						{
							m_network.generatePiersForEdge(*newEdgeId, m_world);
						}
					}

					// 敷設パネルで選択中のルートに新規エッジを追加
					if (!m_pendingRouteIds.isEmpty())
					{
						for (const int rid : m_pendingRouteIds)
						{
							RoadRoute* route = m_network.getRoute(rid);
							if (!route) continue;
							route->edgeIds << *newEdgeId;
						}
						m_network.rebuildEdgeRouteIndex();
					}

				}
				notifyNetworkChanged({ from, nodeId });
				m_roadRenderer.invalidateCachesAroundNode(from, m_network);
				m_roadRenderer.invalidateCachesAroundNode(nodeId, m_network);
			}
			m_drawStartNode = nodeId;
		}
	}

	// 右クリック: 敷設中の始点をキャンセル
	if (MouseR.down())
		m_drawStartNode = none;
}

void GameScene::invokeAutoPlace(Vec3 start, Vec3 goal)
{
	const Array<int> edgeIds = RoadAutoPlace::buildPlanned(m_network, m_world, start, goal, m_pendingRouteIds, m_drawTemplate);

	if (edgeIds.isEmpty())
	{
		Console << U"[AutoPlace] 敷設失敗: 経路が見つかりませんでした";
		return;
	}

	// 生成エッジの高さ・橋脚更新
	for (const int eid : edgeIds)
	{
		m_network.updateEdgeElevation(eid, m_world);
		if (const RoadEdge* edge = m_network.getEdge(eid))
		{
			if (edge->useElevation)
				m_network.generatePiersForEdge(eid, m_world);
		}
	}

	RoadTerrainFit::apply(m_network,m_world,HashSet<int>{edgeIds.begin(),edgeIds.end()});
	m_worldRenderer.invalidateTerrainForEdges(m_network,edgeIds);

	// 影響ノードを収集して差分更新
	Array<int> dirtyNodes;
	for (const int eid : edgeIds)
	{
		if (const RoadEdge* edge = m_network.getEdge(eid))
		{
			dirtyNodes << edge->nodeA << edge->nodeB;
			m_roadRenderer.invalidateCachesAroundNode(edge->nodeA, m_network);
			m_roadRenderer.invalidateCachesAroundNode(edge->nodeB, m_network);
		}
	}
	notifyNetworkChanged(dirtyNodes);

	Console << U"[AutoPlace] 敷設完了: {} エッジ"_fmt(edgeIds.size());
}

void GameScene::handleZonePaint()
{
	if (!m_cursorGroundPos) return;
	if (m_panelManager.blocksMouseInput()) return;

	if (GameInput::pressed(KeyShift))
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
			m_zoneManager.paintZone(m_world, *m_cursorGroundPos, m_paintZone, m_zoneBrushRadius);
	}
}

void GameScene::handleBusRouteDraw()
{
	if (!m_cursorGroundPos) return;
	if (m_editingRouteId < 0) return;
	if (m_panelManager.blocksMouseInput()) return;

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
	}
}

void GameScene::handleTerrainEdit()
{
	if (!m_cursorGroundPos) return;
	if (m_panelManager.blocksMouseInput()) return;

	const float dt    = static_cast<float>(Scene::DeltaTime());
	const float raise = MouseL.pressed() ? m_terrainBrushStrength * dt : 0.0f;
	const float lower = MouseR.pressed() ? m_terrainBrushStrength * dt : 0.0f;
	const float delta = raise - lower;
	if (delta == 0.0f) return;

	const Vec3 center = *m_cursorGroundPos;
	constexpr float kMinimumEditableHeight = -2000.0f;
	constexpr float kMaximumEditableHeight = 6000.0f;

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
				chunk->heightMap[{col, row}] = Clamp(
					chunk->heightMap[{col, row}] + delta * weight, kMinimumEditableHeight, kMaximumEditableHeight);
				modified = true;
			}
		}

		if (modified)
		{
			chunk->meshDirty = true;
			chunk->updateHeightBounds();
			m_subsurface.invalidate();
			m_tunnelRenderer.dirty = true;
		}
	}
}

void GameScene::handleTrainDraw()
{
	if (!m_cursorGroundPos) return;
	if (m_panelManager.blocksMouseInput()) return;

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
			nodeId = m_trainNetwork.addStation(*m_cursorGroundPos, U"新駅{}"_fmt(m_trainNetwork.nodes().size()+1));
		else
		{
			nodeId = *nearNode;
			auto* node = m_trainNetwork.getNode(nodeId);
			if (node->type != TrackNodeType::Station)
			{
				if (RailwaySite::stationMinimumRadius(m_trainNetwork,nodeId) < RailwaySite::kStationMinimumRadius)
				{
					DebugLog::print(U"駅には半径500m以上のホーム区間が必要です");
					m_soundEffects.play(SoundEffects::Cue::Reject); return;
				}
				node->type = TrackNodeType::Station;
				if (node->name.isEmpty()) { node->name=U"新駅{}"_fmt(nodeId); }
			}
		}

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
				// 既設の軌道がつながっていれば、駅とダイヤだけを追加する。
				if (m_trainNetwork.findRoute(from,nodeId).isEmpty())
				{
					if (m_network.getNode(from)->attachments.size() >= 6 || m_network.getNode(nodeId)->attachments.size() >= 6)
					{
						DebugLog::print(U"この駅にはこれ以上線路を接続できません"); return;
					}
					const auto alignment = RoadAlignment::find(m_world,pa,pb,RoadType::LocalRoad,60000,TransportMode::Rail);
					if (!alignment)
					{
						DebugLog::print(U"鉄道の曲率・勾配を満たす経路がありません");
						m_soundEffects.play(SoundEffects::Cue::Reject); return;
					}
					Array<int> changed{from,nodeId}; int previous = from;
					for (size_t i=0;i<alignment->curves.size();++i)
					{
						const auto& curve = alignment->curves[i];
						const int next = i+1==alignment->curves.size() ? nodeId : m_trainNetwork.addNode(curve.p3);
						const int edgeId = m_trainNetwork.addEdge(previous,next,curve.p1,curve.p2,80,true);
						if (edgeId >= 0) { m_network.updateEdgeElevation(edgeId,m_world); m_network.generatePiersForEdge(edgeId,m_world); }
						changed << next; previous = next;
					}
					notifyNetworkChanged(changed);
				}
				const auto schedule = RailTimetable::makeDefault(m_trainNetwork,from,nodeId);
				String error;
				if (m_trainNetwork.applySchedule(schedule,error)) { m_trainRenderer.clearTrackCache(); m_soundEffects.play(SoundEffects::Cue::Complete); }
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
	// サンドボックス編集ではノード移動と制御点移動を直接反映し、周辺キャッシュだけ局所的に失効させる。
	// ドラッグ継続中（MouseL.pressed かつドラッグ対象が確定済み）はパネル上でも処理を続ける
	const bool dragging = MouseL.pressed() && (m_sandboxDragNode || m_sandboxDragCtrl);
	if (!dragging && m_panelManager.blocksMouseInput()) return;

	const Vec2 cur2D{ m_cursorGroundPos->x, m_cursorGroundPos->z };
	auto refreshDraggedConnectivity = [&](const Array<int>& nodeIds)
	{
		HashSet<int> uniqueIds;
		for (const int nodeId : nodeIds)
		{
			if (nodeId < 0 || uniqueIds.contains(nodeId)) continue;
			uniqueIds.insert(nodeId);
			m_network.updateNodeCutoffs(nodeId);
		}
		for (const int nodeId : uniqueIds)
		{
			m_network.updateLaneConnectionPaths(nodeId);
		}
	};

	if (MouseL.down())
	{
		m_sandboxDragNode = none;
		m_sandboxDragNodeStartPos = none;
		m_sandboxDragCtrl = none;

		m_sandboxDragNode = visibleNodeNear(*m_cursorGroundPos, 20.0f);
		if (m_sandboxDragNode)
		{
			if (const RoadNode* node = m_network.getNode(*m_sandboxDragNode))
			{
				m_sandboxDragNodeStartPos = node->position;
			}
		}

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
		if (m_sandboxDragNode)
		{
			NetworkChangeContext context;
			context.kind = NetworkChangeKind::MovedIntersectionNode;
			context.dirtyNodeIds = Array<int>{ *m_sandboxDragNode };
			context.movedNodeId = *m_sandboxDragNode;
			context.oldNodePos = m_sandboxDragNodeStartPos;
			notifyNetworkChanged(context);
		}
		else if (m_sandboxDragCtrl)
		{
			const RoadEdge* e = m_network.getEdge(m_sandboxDragCtrl->edgeId);
			if (e) notifyNetworkChanged({ e->nodeA, e->nodeB });
			else   notifyNetworkChanged();
		}
		m_sandboxDragNode = none;
		m_sandboxDragNodeStartPos = none;
		m_sandboxDragCtrl = none;
	}

	if (MouseL.pressed())
	{
		const Vec3 delta = *m_cursorGroundPos - m_sandboxPrevCursor;

		if (m_sandboxDragNode)
		{
			RoadNode* node = m_network.getNode(*m_sandboxDragNode);
			if (node)
			{
				Array<int> dirtyNodes;
				dirtyNodes << node->id;
				for (const auto& att : node->attachments)
				{
					const int eid = att.edgeId;
					RoadEdge* edge = m_network.getEdge(eid);
					if (!edge) continue;
					if (edge->nodeA == node->id) edge->ctrlA += delta;
					if (edge->nodeB == node->id) edge->ctrlB += delta;
					m_roadRenderer.invalidateEdgeCache(eid, edge->nodeA, edge->nodeB);
					dirtyNodes << edge->nodeA << edge->nodeB;
				}
				node->position += delta;
				refreshDraggedConnectivity(dirtyNodes);
			}
		}
		else if (m_sandboxDragCtrl)
		{
			RoadEdge* edge = m_network.getEdge(m_sandboxDragCtrl->edgeId);
			if (edge)
			{
				if (m_sandboxDragCtrl->isControlPointA) edge->ctrlA += delta;
				else                        edge->ctrlB += delta;
				m_roadRenderer.invalidateEdgeCache(edge->id, edge->nodeA, edge->nodeB);
				refreshDraggedConnectivity({ edge->nodeA, edge->nodeB });
			}
		}

		m_sandboxPrevCursor = *m_cursorGroundPos;
	}

	if (MouseR.down())
	{
		auto nearNode = visibleNodeNear(*m_cursorGroundPos, 20.0f);
		if (nearNode)
		{
			Array<int> neighborNodes;
			if (const RoadNode* node = m_network.getNode(*nearNode))
			{
				m_worldRenderer.invalidateTerrainForNode(*nearNode);
				for (const auto& att : node->attachments)
				{
					const int eid = att.edgeId;
					m_roadRenderer.invalidateEdgeCache(eid);
					m_worldRenderer.invalidateTerrainForEdge(eid);
					if (const RoadEdge* e = m_network.getEdge(eid))
					{
						const int other = (e->nodeA == *nearNode) ? e->nodeB : e->nodeA;
						neighborNodes << other;
					}
				}
			}
			m_network.removeNode(*nearNode);
			notifyNetworkChanged(neighborNodes);
			for (const int nid : neighborNodes)
				m_roadRenderer.invalidateCachesAroundNode(nid, m_network);
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
				int nA = -1, nB = -1;
				if (const RoadEdge* e = m_network.getEdge(bestId))
				{ nA = e->nodeA; nB = e->nodeB; }
				m_worldRenderer.invalidateTerrainForNode(nA);
				m_worldRenderer.invalidateTerrainForNode(nB);
				m_roadRenderer.invalidateEdgeCache(bestId, nA, nB);
				m_worldRenderer.invalidateTerrainForEdge(bestId);
				m_network.removeEdge(bestId);
				{
					Array<int> dirty;
					if (nA >= 0) dirty << nA;
					if (nB >= 0) dirty << nB;
					notifyNetworkChanged(dirty);
				}
				if (nA >= 0) m_roadRenderer.invalidateCachesAroundNode(nA, m_network);
				if (nB >= 0) m_roadRenderer.invalidateCachesAroundNode(nB, m_network);
			}
		}
	}
}

// =============================================================================
// カーソル更新
// =============================================================================

void GameScene::updateCursor()
{
	const Ray    ray  = m_camera.screenToRay(Vec2{ Cursor::Pos() });
	const Float3 orig = ray.origin;
	const Float3 dir  = ray.direction;
	if (m_underground)
	{
		m_subsurface.prepare(m_world, m_network, m_trainNetwork);
		if (const auto hit = m_subsurface.hit(ray))
		{
			m_cursorGroundPos = hit->position;
			return;
		}
		const double distance = Abs(dir.y) > 1e-6 ? (m_camera.focusPoint().y - orig.y) / dir.y : -1;
		m_cursorGroundPos =
			Abs(dir.y) > 1e-6 && distance > 0 ? Optional<Vec3>{Vec3{ray.point_at(static_cast<float>(distance))}} : none;
		return;
	}

	if (dir.y >= 0.0f)
	{
		m_cursorGroundPos = none;
		return;
	}

	const float kStep = Max(8.0f,m_camera.distance()/1000.0f);
	const float kMaxDist = Max(8000.0f,m_camera.distance()*3);

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

// =============================================================================
// モード文字列
// =============================================================================

String GameScene::modeString() const
{
	if (m_drivingNoticeSeconds>0) { return U"近くに走れる道路がありません"; }
	switch (m_mode)
	{
	case EditMode::RoadPlan:
		return U"道路計画 · 右側のパネルで経路を編集";
	case EditMode::RoadDraw:
		return U"道路敷設デバッグ（Sandbox 限定 / Ctrl+R）";
	case EditMode::ZonePaint:
		return U"用途：{} · 左ドラッグで指定"_fmt(zoneName(m_paintZone));
	case EditMode::BusRouteDraw:
		return U"バス · クリックで停留所を指定";
	case EditMode::TerrainEdit:
		return U"地形 · 左で盛土 / 右で掘削";
	case EditMode::TrainDraw:
		return U"線路 · クリックで接続 / 右で中断";
	case EditMode::SandboxEdit:
		return U"調整 · 左ドラッグで移動 / 右で削除";
	default:
		return U"";
	}
}

// =============================================================================
// ヒットテストヘルパー（付帯設備）
// =============================================================================

Optional<int> GameScene::findGuideSignAt(Vec3 pos, float radius) const
{
	Optional<int> best;
	float bestDistSq = radius * radius;
	for (const auto& gs : m_network.guideSigns())
	{
		if (gs.id < 0) continue;
		const RoadEdge* edge = m_network.getEdge(gs.parentEdgeId);
		if (!edge) continue;
		const auto bez = m_network.getBezier(gs.parentEdgeId);
		if (!bez) continue;
		const bool atA = (gs.nodeEndId == edge->nodeA);
		const float cutoff = atA ? edge->cutoffA : edge->cutoffB;
		const float arc = atA
			? (cutoff + gs.arcOffset)
			: (bez->totalLength - cutoff - gs.arcOffset);
		const float clampedArc = Clamp(arc, 0.0f, bez->totalLength);
		const Vec3  roadPos = bez->positionAt(clampedArc);
		const Vec3  right   = tangentToRight(bez->tangentAt(clampedArc));
		// 描画位置と一致させるため横方向オフセットを反映（RoadRenderer::computeSignTransforms と同じ計算）
		const double sx = roadPos.x + right.x * static_cast<double>(gs.lateralOffset);
		const double sz = roadPos.z + right.z * static_cast<double>(gs.lateralOffset);
		const float dx = static_cast<float>(sx - pos.x);
		const float dz = static_cast<float>(sz - pos.z);
		const float d2 = dx * dx + dz * dz;
		if (d2 < bestDistSq) { bestDistSq = d2; best = gs.id; }
	}
	return best;
}

Optional<int> GameScene::findSignalAt(Vec3 pos, float radius) const
{
	Optional<int> best;
	float bestDistSq = radius * radius;
	// 描画と共通の信号柱位置を使い、道路幅や端点の向きが変わっても選択位置を一致させる。
	for (const auto& node : m_network.nodes())
	{
		if (node.id < 0 || !node.signalPlacement) continue;
		for (const auto& att : node.attachments)
		{
			if (att.control != TrafficControl::Signal) continue;
			const RoadEdge* edge = m_network.getEdge(att.edgeId);
			if (!edge) continue;
			const auto bez = m_network.getBezier(att.edgeId);
			if (!bez) continue;

			const auto anchor = RoadGeometry::signalAnchor(*edge, *bez, node.id);
			if (!anchor) { continue; }
			const double sx = anchor->position.x;
			const double sz = anchor->position.z;
			const float dx = static_cast<float>(sx - pos.x);
			const float dz = static_cast<float>(sz - pos.z);
			const float d2 = dx * dx + dz * dz;
			if (d2 < bestDistSq) { bestDistSq = d2; best = node.id; }
		}
	}
	return best;
}

Optional<GameScene::BuildingRef> GameScene::findBuildingAt(const Ray& ray, Optional<double>* hitDistance)
{
	Optional<BuildingRef> best;
	double bestDist = 1e9;

	for (const Chunk* chunk : m_world.getActiveChunks())
	{
		if (!chunk) continue;

		// チャンクの AABB で粗く弾く
		const Vec3 origin = chunk->worldOrigin();
		const Vec3 chunkMin{ origin.x, chunk->heightMin, origin.z };
		const Vec3 chunkMax{ origin.x + CHUNK_SIZE, chunk->heightMax + 60.0, origin.z + CHUNK_SIZE };
		const Box chunkAabb{
			(chunkMin + chunkMax) * 0.5,
			Vec3{ chunkMax.x - chunkMin.x, chunkMax.y - chunkMin.y, chunkMax.z - chunkMin.z } };
		if (!chunkAabb.intersects(ray)) continue;

		for (int row = 0; row < ZONE_CELLS; ++row)
		{
			for (int col = 0; col < ZONE_CELLS; ++col)
			{
				const auto obox = m_worldRenderer.buildingHitBox(*chunk, m_world, col, row);
				if (!obox) continue;
				if (!m_worldRenderer.buildingWithinRenderDistance(*chunk,col,row,*obox,m_camera.eyePosition())) { continue; }
				if (const auto d = obox->intersects(ray))
				{
					if (*d < bestDist)
					{
						bestDist = *d;
						best = BuildingRef{ chunk->coord.x, chunk->coord.y, col, row };
					}
				}
			}
		}
	}
	if (hitDistance) { *hitDistance = best ? Optional<double>{bestDist} : none; }
	return best;
}

void GameScene::refreshLandParcelMesh()
{
	if (!m_selectedLandParcel) { return; }
	const Chunk* chunk=m_world.getChunk(m_selectedLandParcel->chunkCoord);
	if (!chunk) { return; }
	for (const LandPatch& patch:chunk->landPatches)
	{
		if (patch.id!=m_selectedLandParcel->id) { continue; }
		const MeshData surface=m_worldRenderer.landPatchSurface(m_world,m_network,chunk->coord,patch);
		m_landParcelOutline=surface.indices.isEmpty() ? Mesh{} : Mesh{surface};
		m_landParcelRevision=m_worldRenderer.geometryRevision();
		return;
	}
}

/// @brief Keep a selected plot valid while dragging, inserting, or removing its vertices.
void GameScene::handleLandParcelEditInput()
{
	Chunk* chunk=m_world.getChunk(m_selectedLandParcel->chunkCoord);
	if (!chunk) { clearSelection(); return; }
	LandPatch* patch=nullptr;
	for (LandPatch& candidate:chunk->landPatches)
	{
		if (candidate.id==m_selectedLandParcel->id) { patch=&candidate; break; }
	}
	if (!patch) { clearSelection(); return; }
	if (MouseL.up()) { m_landParcelDragVertex=-1; }
	const auto screenPoint=[&](Vec2 point)
	{
		return m_camera.camera3D().worldToScreenPoint(Vec3{point.x,
			m_world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.y))+.2,point.y});
	};
	const Vec2 cursor=Cursor::PosF();
	int vertex=-1,edge=-1;
	double vertexDistance=12.0*12.0,edgeDistance=9.0*9.0;
	for (size_t index=0;index<patch->polygon.size();++index)
	{
		const Vec3 projected=screenPoint(patch->polygon[index]);
		if (projected.z>0 && projected.z<1 && cursor.distanceFromSq(projected.xy())<vertexDistance)
		{ vertex=static_cast<int>(index); vertexDistance=cursor.distanceFromSq(projected.xy()); }
		const Vec2 midpoint=(patch->polygon[index]+patch->polygon[(index+1)%patch->polygon.size()])*.5;
		const Vec3 middle=screenPoint(midpoint);
		if (middle.z>0 && middle.z<1 && cursor.distanceFromSq(middle.xy())<edgeDistance)
		{ edge=static_cast<int>(index); edgeDistance=cursor.distanceFromSq(middle.xy()); }
	}
	const auto apply=[&](Array<Vec2> vertices)
	{
		if (!LandPlot::validEditablePolygon(vertices)) { return false; }
		patch->polygon=std::move(vertices);
		chunk->meshDirty=true;
		refreshLandParcelMesh();
		return true;
	};
	if (MouseR.down() && vertex>=0 && patch->polygon.size()>3 && !m_panelManager.blocksMouseInput())
	{
		Array<Vec2> vertices=patch->polygon;
		vertices.erase(vertices.begin()+vertex);
		m_landParcelDragVertex=-1;
		apply(std::move(vertices));
		return;
	}
	if (MouseL.down() && !m_panelManager.blocksMouseInput())
	{
		if (vertex>=0) { m_landParcelDragVertex=vertex; }
		else if (edge>=0)
		{
			Array<Vec2> vertices=patch->polygon;
			vertices.insert(vertices.begin()+edge+1,(vertices[edge]+vertices[(edge+1)%vertices.size()])*.5);
			if (apply(std::move(vertices))) { m_landParcelDragVertex=edge+1; }
		}
	}
	if (m_landParcelDragVertex>=0 && MouseL.pressed() && m_cursorGroundPos)
	{
		Array<Vec2> vertices=patch->polygon;
		vertices[m_landParcelDragVertex]={m_cursorGroundPos->x,m_cursorGroundPos->z};
		apply(std::move(vertices));
	}
}

bool GameScene::selectLandParcelAt(Vec2 position)
{
	for (const Chunk* chunk : m_world.getActiveChunks())
	{
		if (!chunk) { continue; }
		const LandPatch* patch=LandPlot::find(*chunk,position);
		if (!patch) { continue; }
		MeshData surface=m_worldRenderer.landPatchSurface(m_world,m_network,chunk->coord,*patch);
		if (!LandPlot::containsSurface(surface,position)) { continue; }
		clearSelection();
		m_selection={SelectionKind::LandParcel,patch->id};
		m_selectedLandParcel=LandParcelRef{chunk->coord,patch->id};
		m_landParcelOutline=Mesh{surface};
		m_landParcelRevision=m_worldRenderer.geometryRevision();
		for (const StringView id : {U"building_info",U"edge_info",U"node_info",U"signal_edit",U"guide_sign_edit",U"route_info"}) { m_panelManager.hide(id); }
		m_panelManager.show(U"land_info",U"敷地・農地",panelRightPos(U"land_info"));
		return true;
	}
	return false;
}

void GameScene::handleGlobalShortcuts()
{
	if(!m_commandPalette.visible && !m_showPauseMenu && GameInput::down(KeySlash)) { m_commandPalette.open(); }
	if(m_commandPalette.visible)
	{
		if(const auto command=m_commandPalette.update()) { executeCommand(*command); }
		return;
	}
	// The full-screen map consumes world input, but must not swallow the save shortcut.
	if (GameInput::saveShortcutActive(m_showPauseMenu))
	{
		if (GameInput::down(KeyS)) { saveGame(); }
		// Consume the held chord until release so S cannot also move the camera or pan the map.
		GameInput::textOwnedFrame = true;
		return;
	}
	if (!m_showPauseMenu && !GameInput::keyboardBlocked() && GameInput::down(KeyF3)) { m_frameRateGraph.visible=!m_frameRateGraph.visible; }
}
