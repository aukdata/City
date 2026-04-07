#include "GameScene.hpp"

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
	if (KeyControl.pressed() && KeyShift.pressed() && KeyS.down())
	{
		saveGame();
		return;
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

	if (KeyEscape.down())
	{
		if (m_mode != EditMode::None)
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
		m_selectedEdgeId = none;
		m_selectedNodeId = none;
		m_panelManager.hide(U"edge_info");
		m_panelManager.hide(U"node_info");
		m_panelManager.hide(U"draw_template");
	}

	if (KeyR.down())
	{
		m_mode = (m_mode == EditMode::RoadDraw) ? EditMode::None : EditMode::RoadDraw;
		m_drawStartNode = none;
		m_drawElevation = 0.0f;
		m_rectStart     = none;
		if (m_mode == EditMode::RoadDraw)
			m_panelManager.show(U"draw_template", U"Road Template", panelRightPos(U"draw_template"));
		else
			m_panelManager.hide(U"draw_template");
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
			m_editingRouteId = -1;
		}
	}

	if (m_sandboxActive && KeyV.down())
	{
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
	if (m_selectedNodeId)
	{
		constexpr float kNodeYStep = 1.0f;
		const bool up   = KeyPageUp.pressed();
		const bool down = KeyPageDown.pressed();
		if (up || down)
		{
			if (auto* node = m_network.getNode(*m_selectedNodeId))
			{
				const float dy = up ? kNodeYStep : -kNodeYStep;
				node->position.y += dy;

				// 接続エッジのコントロールポイント Y も同じ差分で移動し、elevation を自動判定
				for (const int eid : node->edgeIds())
				{
					if (auto* edge = m_network.getEdge(eid))
					{
						if (edge->nodeA == *m_selectedNodeId)
							edge->ctrlA.y += dy;
						else
							edge->ctrlB.y += dy;
						m_network.updateEdgeElevation(eid, m_world);
					}
				}

				m_roadRenderer.invalidateCachesAroundNode(*m_selectedNodeId, m_network);
			}
		}
	}

	if      (m_mode == EditMode::RoadDraw)     handleRoadDraw();
	else if (m_mode == EditMode::ZonePaint)    handleZonePaint();
	else if (m_mode == EditMode::BusRouteDraw) handleBusRouteDraw();
	else if (m_mode == EditMode::TerrainEdit)  handleTerrainEdit();
	else if (m_mode == EditMode::TrainDraw)    handleTrainDraw();
	else if (m_mode == EditMode::SandboxEdit)  handleSandboxEdit();
	else if (m_mode == EditMode::None)
	{
		// 車両 -> ノード -> エッジの優先順でクリック判定
		if (MouseL.down() && m_cursorGroundPos && !m_panelManager.isMouseOnAnyPanel())
		{
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
				m_selectedEdgeId = none;
				m_selectedNodeId = none;
				m_panelManager.show(U"vehicle_info", U"Vehicle #{}"_fmt(*hitVehicleId),
					panelRightPos(U"vehicle_info"));
				m_panelManager.hide(U"edge_info");
				m_panelManager.hide(U"node_info");
			}
			else
			{
				if (m_selectedVehicleId)
				{
					m_selectedVehicleId = none;
					m_trackingVehicle = false;
					m_panelManager.hide(U"vehicle_info");
				}

				// 地上カーソルで検索
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
					m_selectedNodeId = hitNode;
					m_selectedEdgeId = none;
					m_panelManager.show(U"node_info", U"RoadNode #{}"_fmt(*hitNode),
						panelRightPos(U"node_info"));
					m_panelManager.hide(U"edge_info");
				}
				else
				{
					m_selectedEdgeId = hitEdge;
					m_selectedNodeId = none;
					if (hitEdge)
						m_panelManager.show(U"edge_info", U"RoadEdge #{}"_fmt(*hitEdge),
							panelRightPos(U"edge_info"));
					else
						m_panelManager.hide(U"edge_info");
					m_panelManager.hide(U"node_info");
				}
			}
		}
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
	// PgUp/PgDown: 高さオフセットを変更
	{
		constexpr float kElevStep = 1.0f;
		if (KeyPageUp.pressed())   m_drawElevation += kElevStep;
		if (KeyPageDown.pressed()) m_drawElevation = Max(m_drawElevation - kElevStep, 0.0f);
	}

	// ホイールクリック: 既存道路の構成をテンプレートにコピー
	if (MouseM.down() && m_cursorGroundPos && !m_panelManager.isMouseOnAnyPanel())
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
		{
			chunk->meshDirty = true;
			chunk->updateHeightBounds();
		}
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
