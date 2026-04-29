#include "GameScene.hpp"
#include "EdgeSectionState.hpp"
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
	if (PanelWidget::activeTextInput != nullptr)
	{
		if (KeyEscape.down())
		{
			PanelWidget::activeTextInput->active = false;
			PanelWidget::activeTextInput = nullptr;
		}
		return;
	}

	// ---- ESC: ポーズメニュートグル ----
	if (KeyEscape.down())
	{
		if (m_showPauseMenu)
		{
			m_showPauseMenu = false;
		}
		else if (m_mode != EditMode::None
			|| m_selection.kind != SelectionKind::None)
		{
			if (m_mode != EditMode::None)
			{
				if (m_mode == EditMode::RoadPlan)
					clearDraftRoadPlan(true);
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
			if (m_clock.speed != TimeSpeed::Paused)
			{
				m_prevSpeed   = m_clock.speed;
				m_clock.speed = TimeSpeed::Paused;
			}
		}
	}

	// ポーズメニュー表示中は他の入力をブロック
	if (m_showPauseMenu) return;

	if (KeyControl.pressed() && KeyShift.pressed() && KeyS.down())
	{
		saveGame();
		return;
	}

	// ---- F3 コマンド ----
	if (KeyF3.pressed())
	{
		if (KeyR.down())
		{
			Console << U"[Reload] Reloading all assets...";
			m_roadRenderer.loadAssets();
			m_roadRenderer.invalidateAllCaches();
			Console << U"[Reload] Done.";
		}
		return;  // F3 押下中は通常操作を無効化
	}

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

	if (m_sandboxActive && KeyControl.pressed() && KeyR.down())
	{
		if (m_mode == EditMode::RoadPlan)
			clearDraftRoadPlan(true);
		m_mode = (m_mode == EditMode::RoadDraw) ? EditMode::None : EditMode::RoadDraw;
		m_drawStartNode = none;
		m_drawElevation = 0.0f;
		m_rectStart     = none;
		if (m_mode == EditMode::RoadDraw)
			m_panelManager.show(U"draw_template", U"道路敷設デバッグ", panelRightPos(U"draw_template"));
		else
			m_panelManager.hide(U"draw_template");
	}
	else if (KeyR.down())
	{
		const bool enable = (m_mode != EditMode::RoadPlan);
		if (!enable)
		{
			clearDraftRoadPlan(true);
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
			m_panelManager.show(U"draw_template", U"道路計画", panelRightPos(U"draw_template"));
		}
	}
	if (KeyZ.down())
	{
		if (m_mode == EditMode::RoadPlan)
			clearDraftRoadPlan(true);
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
		if (Key1.down()) { m_prevSpeed = TimeSpeed::x1; m_clock.speed = TimeSpeed::x1; }
		if (Key2.down()) { m_prevSpeed = TimeSpeed::x2; m_clock.speed = TimeSpeed::x2; }
		if (Key3.down()) { m_prevSpeed = TimeSpeed::x4; m_clock.speed = TimeSpeed::x4; }
		if (Key0.down()) { m_prevSpeed = m_clock.speed != TimeSpeed::Paused ? m_clock.speed : m_prevSpeed;
		                   m_clock.speed = TimeSpeed::Paused; }
	}

	if (KeyT.down() && m_simGraph)
		m_vehicleManager.spawnRandom(*m_simGraph);

	if (KeyF.down())
		m_camera.cycleMode();

	if (KeyG.down())
	{
		if (m_mode == EditMode::RoadPlan)
			clearDraftRoadPlan(true);
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
		if (m_mode == EditMode::RoadPlan)
			clearDraftRoadPlan(true);
		m_mode = (m_mode == EditMode::TrainDraw) ? EditMode::None : EditMode::TrainDraw;
		m_trainDrawStartNode = none;
	}

	if (KeyB.down())
	{
		if (m_mode == EditMode::RoadPlan)
			clearDraftRoadPlan(true);
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

	if (m_sandboxActive && KeyV.down())
	{
		if (m_mode == EditMode::RoadPlan)
			clearDraftRoadPlan(true);
		m_mode = (m_mode == EditMode::SandboxEdit) ? EditMode::None : EditMode::SandboxEdit;
		m_sandboxDragNode = none;
		m_drawStartNode   = none;
		m_rectStart       = none;
	}

	if (KeyN.down())
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
		const bool up   = KeyPageUp.pressed();
		const bool down = KeyPageDown.pressed();
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
	// ワールド上の候補を優先順位つきで走査し、対応する情報パネルと選択状態を一貫して切り替える。
	// 車両 -> ノード -> エッジの優先順でクリック判定
	Optional<int> hitVehicleId;
	{
		const Ray ray = m_camera.screenToRay(Vec2{ Cursor::Pos() });
		double bestDist = 1e9;
		for (const auto& v : m_renderVehicles)
		{
			const Vec3 size = Vec3{ 2.0, 3.0, 5.0 };
			const Vec3 center = v.position + Vec3{ 0, size.y / 2, 0 };
			const Quaternion rot = Quaternion::RotateY(v.heading);
			const OrientedBox box{ center, size, rot };
			if (const auto d = box.intersects(ray))
			{
				if (*d < bestDist)
				{
					bestDist = *d;
					hitVehicleId = v.id;
				}
			}
		}
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

	// ── 付帯設備（看板・信号）のヒットテスト（ノード/エッジより優先） ──
	constexpr float kInfraHitRadius = 10.0f;
	if (const auto hitGuideSignId = findGuideSignAt(*m_cursorGroundPos, kInfraHitRadius))
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
	if (const auto hitSignalNodeId = findSignalAt(*m_cursorGroundPos, kInfraHitRadius))
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
		const Ray ray = m_camera.screenToRay(Vec2{ Cursor::Pos() });
		if (const auto hitBuilding = findBuildingAt(ray))
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

	// 地上カーソルで検索（ノード・エッジ）
	auto hitNode = m_network.findNodeNear(*m_cursorGroundPos, 20.0f);
	Optional<int> hitEdge;
	if (!hitNode)
		hitEdge = m_network.findEdgeNear(*m_cursorGroundPos, 15.0f);

	// 高架面とのレイ交差で追加検索
	{
		const auto elev = raycastElevated(Vec2{ Cursor::Pos() });
		if (elev.nodeId) { hitNode = elev.nodeId; hitEdge = none; }
		else if (!hitNode && elev.edgeId) { hitEdge = elev.edgeId; }
	}

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
		if (KeyPageUp.pressed())   m_drawElevation += kElevStep;
		if (KeyPageDown.pressed()) m_drawElevation = Max(m_drawElevation - kElevStep, 0.0f);
	}

	// パネル上にカーソルがあるときはマウス操作をすべて吸収
	if (m_panelManager.blocksMouseInput()) return;

	// ────────────────────────────────────────────────────────────────
	// スタート/ゴール指定モード: 左クリックで2点指定して自動敷設
	// ────────────────────────────────────────────────────────────────
	if (m_autoPlaceMode)
	{
		// ESC でスタートをクリア（モードは継続）
		if (KeyEscape.down())
		{
			m_autoPlaceStart = none;
			return;
		}

		if (MouseL.down() && m_cursorGroundPos)
		{
			const Vec3 clickPos = *m_cursorGroundPos;

			// 水域チェック
			if (m_world.computeHeight(static_cast<float>(clickPos.x),
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
		auto nearNode = m_network.findNodeNear(clickPos, 20.0f);
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
					if (m_network.getEdge(*newEdgeId)->useElevation)
						m_network.generatePiersForEdge(*newEdgeId, m_world);

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

void GameScene::clearDraftRoadPlan(bool removeEdges)
{
	if (removeEdges)
	{
		Array<int> dirtyNodes;
		for (const int eid : m_draftRoadPlan.edgeIds)
		{
			if (const RoadEdge* edge = m_network.getEdge(eid))
				dirtyNodes << edge->nodeA << edge->nodeB;
		}
		for (const int eid : m_draftRoadPlan.edgeIds)
			m_network.removeEdge(eid);
		for (const int nid : dirtyNodes)
			m_roadRenderer.invalidateCachesAroundNode(nid, m_network);
		if (!dirtyNodes.isEmpty())
			notifyNetworkChanged(dirtyNodes);
	}
	m_draftRoadPlan.anchorPoints.clear();
	m_draftRoadPlan.edgeIds.clear();
	m_draftRoadPlan.viaPlacementMode = false;
}

Array<Vec3> GameScene::draftRoadPlanViaPoints() const
{
	Array<Vec3> viaPoints;
	for (size_t i = 1; i + 1 < m_draftRoadPlan.anchorPoints.size(); ++i)
		viaPoints << m_draftRoadPlan.anchorPoints[i];
	return viaPoints;
}

bool GameScene::rebuildDraftRoadPlan()
{
	const Array<Vec3> points = m_draftRoadPlan.anchorPoints;
	clearDraftRoadPlan(true);
	m_draftRoadPlan.anchorPoints = points;
	if (points.size() < 2) return false;

	m_draftRoadPlan.edgeIds = RoadAutoPlace::buildPreviewPlan(
		m_network, m_world, m_draftRoadPlan.anchorPoints, m_drawTemplate);
	if (m_draftRoadPlan.edgeIds.isEmpty())
		return false;

	Array<int> dirtyNodes;
	for (const int eid : m_draftRoadPlan.edgeIds)
	{
		m_network.updateEdgeElevation(eid, m_world);
		if (const RoadEdge* edge = m_network.getEdge(eid))
		{
			if (edge->useElevation)
				m_network.generatePiersForEdge(eid, m_world);
			dirtyNodes << edge->nodeA << edge->nodeB;
			m_roadRenderer.invalidateCachesAroundNode(edge->nodeA, m_network);
			m_roadRenderer.invalidateCachesAroundNode(edge->nodeB, m_network);
		}
	}
	if (!dirtyNodes.isEmpty())
		notifyNetworkChanged(dirtyNodes);
	return true;
}

bool GameScene::commitDraftRoadPlan()
{
	if (m_draftRoadPlan.edgeIds.isEmpty()) return false;

	int routeId = -1;
	String routeName = m_draftRoadPlan.routeNameEdit.text;
	if (m_draftRoadPlan.appendToExistingRoute && m_draftRoadPlan.routeId)
	{
		routeId = *m_draftRoadPlan.routeId;
		if (RoadRoute* route = m_network.getRoute(routeId))
		{
			for (const int eid : m_draftRoadPlan.edgeIds)
				route->edgeIds << eid;
			routeName = route->name;
		}
	}
	else
	{
		routeId = m_network.addRoute(
			RoadRouteKind::Named,
			routeName,
			m_draftRoadPlan.edgeIds,
			0);
		if (const RoadRoute* route = m_network.getRoute(routeId))
			routeName = route->name;
	}
	m_network.rebuildEdgeRouteIndex();

	RoadPlan plan;
	plan.name = m_draftRoadPlan.nameEdit.text;
	if (plan.name.isEmpty())
		plan.name = U"道路計画 {}"_fmt(m_network.plans().size() + 1);
	plan.routeId = routeId;
	plan.routeName = routeName;
	plan.roadType = m_drawTemplate.roadType;
	plan.edgeIds = m_draftRoadPlan.edgeIds;
	plan.viaPoints = draftRoadPlanViaPoints();
	plan.originName = U"始点";
	plan.destName = U"終点";
	plan.state = PlanState::Planning;

	const int planId = m_network.addPlan(std::move(plan));
	selectRoadPlan(planId);
	m_draftRoadPlan.edgeIds.clear();
	m_draftRoadPlan.anchorPoints.clear();
	m_draftRoadPlan.viaPlacementMode = false;
	return true;
}

void GameScene::handleRoadPlan()
{
	if (m_panelManager.blocksMouseInput() || !m_cursorGroundPos) return;

	if (MouseR.down())
	{
		if (!m_draftRoadPlan.anchorPoints.isEmpty() && m_draftRoadPlan.edgeIds.isEmpty())
			m_draftRoadPlan.anchorPoints.pop_back();
		return;
	}

	if (!MouseL.down()) return;

	const Vec3 clickPos = *m_cursorGroundPos;
	if (m_draftRoadPlan.anchorPoints.isEmpty())
	{
		clearDraftRoadPlan(true);
		m_draftRoadPlan.anchorPoints << clickPos;
		return;
	}

	if (m_draftRoadPlan.viaPlacementMode)
	{
		if (m_draftRoadPlan.edgeIds.isEmpty())
			m_draftRoadPlan.anchorPoints << clickPos;
		else
		{
			Array<Vec3> updated;
			for (size_t i = 0; i + 1 < m_draftRoadPlan.anchorPoints.size(); ++i)
				updated << m_draftRoadPlan.anchorPoints[i];
			updated << clickPos;
			updated << m_draftRoadPlan.anchorPoints.back();
			m_draftRoadPlan.anchorPoints = std::move(updated);
			rebuildDraftRoadPlan();
		}
		m_draftRoadPlan.viaPlacementMode = false;
		return;
	}

	if (m_draftRoadPlan.anchorPoints.size() == 1 || !m_draftRoadPlan.edgeIds.isEmpty())
	{
		if (m_draftRoadPlan.edgeIds.isEmpty())
			m_draftRoadPlan.anchorPoints << clickPos;
		else
			m_draftRoadPlan.anchorPoints.back() = clickPos;
		rebuildDraftRoadPlan();
	}
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
		{
			chunk->meshDirty = true;
			chunk->updateHeightBounds();
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
	// サンドボックス編集ではノード移動と制御点移動を直接反映し、周辺キャッシュだけ局所的に失効させる。
	// ドラッグ継続中（MouseL.pressed かつドラッグ対象が確定済み）はパネル上でも処理を続ける
	const bool dragging = MouseL.pressed() && (m_sandboxDragNode || m_sandboxDragCtrl);
	if (!dragging && m_panelManager.blocksMouseInput()) return;

	const Vec2 cur2D{ m_cursorGroundPos->x, m_cursorGroundPos->z };

	if (MouseL.down())
	{
		m_sandboxDragNode = none;
		m_sandboxDragCtrl = none;

		m_sandboxDragNode = m_network.findNodeNear(*m_cursorGroundPos, 20.0f);

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
			notifyNetworkChanged({ *m_sandboxDragNode });
		}
		else if (m_sandboxDragCtrl)
		{
			const RoadEdge* e = m_network.getEdge(m_sandboxDragCtrl->edgeId);
			if (e) notifyNetworkChanged({ e->nodeA, e->nodeB });
			else   notifyNetworkChanged();
		}
		m_sandboxDragNode = none;
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
				for (const auto& att : node->attachments)
				{
					const int eid = att.edgeId;
					RoadEdge* edge = m_network.getEdge(eid);
					if (!edge) continue;
					if (edge->nodeA == node->id) edge->ctrlA += delta;
					if (edge->nodeB == node->id) edge->ctrlB += delta;
					m_roadRenderer.invalidateEdgeCache(eid, edge->nodeA, edge->nodeB);
				}
				node->position += delta;
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
			}
		}

		m_sandboxPrevCursor = *m_cursorGroundPos;
	}

	if (MouseR.down())
	{
		auto nearNode = m_network.findNodeNear(*m_cursorGroundPos, 20.0f);
		if (nearNode)
		{
			Array<int> neighborNodes;
			if (const RoadNode* node = m_network.getNode(*nearNode))
			{
				for (const auto& att : node->attachments)
				{
					const int eid = att.edgeId;
					m_roadRenderer.invalidateEdgeCache(eid);
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
				m_roadRenderer.invalidateEdgeCache(bestId, nA, nB);
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

// =============================================================================
// モード文字列
// =============================================================================

String GameScene::modeString() const
{
	switch (m_mode)
	{
	case EditMode::RoadPlan:
		return U"道路計画モード（R:切替 左クリック:始点/終点 経由地はパネルから追加）";
	case EditMode::RoadDraw:
		return U"道路敷設デバッグ（Sandbox 限定 / Ctrl+R）";
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
	// RoadRenderer::ensureSignalAttachGeomCache と同じ計算で各 attachment の描画位置を算出する
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

			const bool  isNodeA   = (edge->nodeA == node.id);
			const float cutoff    = isNodeA ? edge->cutoffA : edge->cutoffB;
			const float cutoffArc = isNodeA ? cutoff : (bez->totalLength - cutoff);
			const Vec3  cutPos    = bez->positionAt(cutoffArc);
			const Vec3  rightVec  = tangentToRight(bez->tangentAt(cutoffArc));

			const bool entryOnRight = !isNodeA;
			float roadEdgeOffset = 0.0f;
			bool  foundRoadbed   = false;
			for (const auto& part : edge->parts)
			{
				if (part.type != RoadPartType::Roadbed) continue;
				const float oL = isNodeA ? part.offsetA_L : part.offsetB_L;
				const float oR = isNodeA ? part.offsetA_R : part.offsetB_R;
				const float edgePos = entryOnRight ? oR : oL;
				if (!foundRoadbed)
				{
					roadEdgeOffset = edgePos;
					foundRoadbed   = true;
				}
				else
				{
					roadEdgeOffset = entryOnRight
						? Max(roadEdgeOffset, edgePos)
						: Min(roadEdgeOffset, edgePos);
				}
			}
			if (!foundRoadbed)
			{
				roadEdgeOffset = entryOnRight
					? edge->totalWidth() * 0.5f
					: -edge->totalWidth() * 0.5f;
			}

			const double sx = cutPos.x - rightVec.x * roadEdgeOffset;
			const double sz = cutPos.z - rightVec.z * roadEdgeOffset;
			const float dx = static_cast<float>(sx - pos.x);
			const float dz = static_cast<float>(sz - pos.z);
			const float d2 = dx * dx + dz * dz;
			if (d2 < bestDistSq) { bestDistSq = d2; best = node.id; }
		}
	}
	return best;
}

Optional<GameScene::BuildingRef> GameScene::findBuildingAt(const Ray& ray)
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
	return best;
}
