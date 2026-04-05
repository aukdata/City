#include "GameScene.hpp"

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
			m_panelManager.show(U"name_list", U"地名リスト (N)",
				Vec2{static_cast<double>(Scene::Width() - 260), 10.0});
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

				// 接続エッジのコントロールポイント Y も同じ差分で移動
				// 前後どちらかのノードが地形から離れていれば useElevation を自動設定
				for (const int eid : node->edgeIds())
				{
					if (auto* edge = m_network.getEdge(eid))
					{
						if (edge->nodeA == *m_selectedNodeId)
							edge->ctrlA.y += dy;
						else
							edge->ctrlB.y += dy;

						const auto* nA = m_network.getNode(edge->nodeA);
						const auto* nB = m_network.getNode(edge->nodeB);
						if (nA && nB)
						{
							const float gyA = m_world.computeHeight(
								static_cast<float>(nA->position.x), static_cast<float>(nA->position.z));
							const float gyB = m_world.computeHeight(
								static_cast<float>(nB->position.x), static_cast<float>(nB->position.z));
							constexpr double kElevThreshold = 0.5;
							const bool elevA = std::abs(nA->position.y - static_cast<double>(gyA)) > kElevThreshold;
							const bool elevB = std::abs(nB->position.y - static_cast<double>(gyB)) > kElevThreshold;
							edge->useElevation = elevA || elevB;
						}
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
					Vec2{static_cast<double>(Scene::Width() - 292), 10.0});
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

				const auto hitNode = m_network.findNodeNear(*m_cursorGroundPos, 20.0f);
				if (hitNode)
				{
					m_selectedNodeId = hitNode;
					m_selectedEdgeId = none;
					m_panelManager.show(U"node_info", U"RoadNode #{}"_fmt(*hitNode),
						Vec2{static_cast<double>(Scene::Width() - 322), 10.0});
					m_panelManager.hide(U"edge_info");
				}
				else
				{
					const auto hitEdge = m_network.findEdgeNear(*m_cursorGroundPos, 15.0f);
					m_selectedEdgeId = hitEdge;
					m_selectedNodeId = none;
					if (hitEdge)
						m_panelManager.show(U"edge_info", U"RoadEdge #{}"_fmt(*hitEdge),
							Vec2{static_cast<double>(Scene::Width() - 322), 10.0});
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
				notifyNetworkChanged();
				m_roadRenderer.invalidateCachesAroundNode(from, m_network);
				m_roadRenderer.invalidateCachesAroundNode(nodeId, m_network);
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
		if (m_sandboxDragNode || m_sandboxDragCtrl)
			notifyNetworkChanged();
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
			notifyNetworkChanged();
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
				notifyNetworkChanged();
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
