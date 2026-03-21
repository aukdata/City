#include "DebugRenderer.hpp"
#include "DebugLog.hpp"

// ===== handleInput =====

void DebugRenderer::handleInput()
{
	if (!KeyF3.pressed()) return;

	bool anyChord = false;
	if (KeyN.down())     { m_showNetwork   = !m_showNetwork;   m_debugMode = true; anyChord = true; }
	if (KeyC.down())     { m_showChunks    = !m_showChunks;    m_debugMode = true; anyChord = true; }
	if (KeyV.down())     { m_showVehicles  = !m_showVehicles;  m_debugMode = true; anyChord = true; }
	if (KeyG.down())     { m_showGrid      = !m_showGrid;      m_debugMode = true; anyChord = true; }
	if (KeyH.down())     { m_showDetailHUD = !m_showDetailHUD; m_debugMode = true; anyChord = true; }
	if (KeySlash.down())
	{
		m_showHelp  = !m_showHelp;
		if (m_showHelp) m_helpOpenTime = Scene::Time();
		m_debugMode = true;
		anyChord    = true;
	}

	if (!anyChord && KeyF3.down())
		m_debugMode = !m_debugMode;
}

// ===== render =====

void DebugRenderer::render(const RoadNetwork& network,
                           const Array<Vehicle>& vehicles,
                           const World& world,
                           const GameCamera& camera)
{
	if (!m_debugMode) return;

	// 3D オーバーレイ（描画順: グリッド → ネットワーク → チャンク → 車両）
	if (m_showGrid)     renderGrid(camera);
	if (m_showNetwork)  renderNetwork(network, camera);
	if (m_showChunks)   renderChunks(world, camera);
	if (m_showVehicles) renderVehicleInfo(vehicles, camera);

	// 2D オーバーレイ
	if (m_showDetailHUD) renderDetailHUD(network, world, camera, vehicles);
	renderLog();
	// ヘルプパネル（10s で自動クローズ）
	if (m_showHelp)
	{
		if (Scene::Time() - m_helpOpenTime >= 10.0)
			m_showHelp = false;
		else
			renderHelp();
	}
}

// ===== D-01: ネットワーク可視化 =====

void DebugRenderer::renderNetwork(const RoadNetwork& network, const GameCamera& camera)
{
	// ノード
	for (const auto& node : network.nodes())
	{
		if (node.id == -1) continue;

		ColorF color = Palette::White;
		switch (node.type)
		{
		case NodeType::Intersection: color = Palette::Yellow; break;
		case NodeType::TJunction:    color = Palette::Orange; break;
		case NodeType::Endpoint:     color = Palette::White;  break;
		case NodeType::IC:           color = Palette::Cyan;   break;
		}
		Sphere{ node.position + Vec3{ 0, 1, 0 }, 8.0 }.draw(color);

		// ノード ID ラベル
		const Float3 sp = camera.camera3D().worldToScreenPoint(
			Float3{ node.position } + Float3{ 0, 20, 0 });
		if (sp.z > 0.0f)
			m_font(U"N{}"_fmt(node.id)).draw(Vec2{ sp.x, sp.y }, Palette::Yellow);
	}

	// エッジ（ベジェ曲線を 20 分割してシリンダーで描画）
	int edgeCount = 0;
	for (const auto& edge : network.edges())
	{
		if (edge.id == -1) continue;
		++edgeCount;

		const auto bezOpt = network.getBezier(edge.id);
		if (!bezOpt) continue;
		const auto& bez = *bezOpt;

		ColorF color;
		switch (edge.roadType)
		{
		case RoadType::LocalRoad:  color = ColorF{ 0.3, 0.8, 1.0 }; break;
		case RoadType::Arterial:   color = ColorF{ 0.2, 0.9, 0.2 }; break;
		case RoadType::Expressway: color = ColorF{ 1.0, 0.3, 0.3 }; break;
		case RoadType::Highway:    color = ColorF{ 0.8, 0.3, 1.0 }; break;
		}

		constexpr int N = 20;
		Vec3 prev = bez.positionAt(0.0f) + Vec3{ 0, 1, 0 };
		for (int i = 1; i <= N; ++i)
		{
			const Vec3 cur = bez.positionAt(bez.totalLength * (i / static_cast<float>(N)))
				+ Vec3{ 0, 1, 0 };
			if ((cur - prev).length() > 0.01)
				Cylinder{ prev, cur, 0.8 }.draw(color);
			prev = cur;
		}

		// 中点に方向マーカー（黄色球）
		Sphere{ bez.positionAt(bez.totalLength * 0.5f) + Vec3{ 0, 3, 0 }, 3.0 }.draw(ColorF{ 1, 1, 0 });
	}

	// 統計ラベル
	int nodeCount = 0;
	for (const auto& n : network.nodes()) if (n.id != -1) ++nodeCount;
	m_font(U"[D-01 Network]  Nodes:{}  Edges:{}"_fmt(nodeCount, edgeCount))
		.draw(Vec2{ 10, 10 }, Palette::Lime);

	// クリックでノード / エッジ中点を選択して Console 出力
	if (MouseL.down())
	{
		const Ray ray = camera.screenToRay(Cursor::Pos());

		int   hitNode     = -1;
		float hitNodeDist = 1e30f;
		for (const auto& node : network.nodes())
		{
			if (node.id == -1) continue;
			const Sphere s{ node.position + Vec3{ 0, 1, 0 }, 8.0 };
			if (const auto d = ray.intersects(s))
				if (*d < hitNodeDist) { hitNodeDist = *d; hitNode = node.id; }
		}

		int   hitEdge     = -1;
		float hitEdgeDist = 1e30f;
		for (const auto& edge : network.edges())
		{
			if (edge.id == -1) continue;
			const auto bezOpt = network.getBezier(edge.id);
			if (!bezOpt) continue;
			const Sphere s{ bezOpt->positionAt(bezOpt->totalLength * 0.5f) + Vec3{ 0, 3, 0 }, 3.0 };
			if (const auto d = ray.intersects(s))
				if (*d < hitEdgeDist) { hitEdgeDist = *d; hitEdge = edge.id; }
		}

		// ノードとエッジが重なった場合は近い方を優先
		if (hitNode != -1 && (hitEdge == -1 || hitNodeDist <= hitEdgeDist))
		{
			const auto* node = network.getNode(hitNode);
			if (node)
			{
				static constexpr StringView nodeTypeStr[] = {
					U"Intersection", U"TJunction", U"Endpoint", U"IC"
				};
				const StringView typeStr = nodeTypeStr[static_cast<int>(node->type)];
				String edgeList;
				for (int eid : node->edgeIds)
					edgeList += U"{} "_fmt(eid);
				Console << U"[Node {}]  type={}  pos=({:.0f},{:.0f},{:.0f})  edges=[{}]"_fmt(
					node->id, typeStr,
					node->position.x, node->position.y, node->position.z,
					edgeList.trimmed());
			}
		}
		else if (hitEdge != -1)
		{
			const auto* edge = network.getEdge(hitEdge);
			if (edge)
			{
				static constexpr StringView roadTypeStr[] = {
					U"LocalRoad", U"Arterial", U"Expressway", U"Highway"
				};
				const StringView typeStr = roadTypeStr[static_cast<int>(edge->roadType)];
				Console << U"[Edge {}]  type={}  {}->{} lanes={}  len={:.0f}m  speed={:.0f}km/h"_fmt(
					edge->id, typeStr,
					edge->nodeA, edge->nodeB,
					edge->lanes.size(),
					edge->length,
					edge->speedLimit);
			}
		}
	}
}

// ===== D-02: チャンク境界 =====

void DebugRenderer::renderChunks(const World& world, const GameCamera& camera)
{
	for (const Chunk* chunk : world.getActiveChunks())
	{
		if (!chunk) continue;

		const double x0 = static_cast<double>(chunk->coord.x) * CHUNK_SIZE;
		const double z0 = static_cast<double>(chunk->coord.y) * CHUNK_SIZE;
		const double x1 = x0 + CHUNK_SIZE;
		const double z1 = z0 + CHUNK_SIZE;
		constexpr double y = 0.5;
		constexpr double r = 0.5;

		ColorF color{ 0.2, 1.0, 0.2, 0.6 };
		if (chunk->state == ChunkState::Sleeping) color = ColorF{ 0.5, 0.5, 0.5, 0.5 };
		if (chunk->state == ChunkState::Loading)  color = ColorF{ 1.0, 1.0, 0.2, 0.6 };

		Cylinder{ Vec3{x0, y, z0}, Vec3{x1, y, z0}, r }.draw(color);
		Cylinder{ Vec3{x1, y, z0}, Vec3{x1, y, z1}, r }.draw(color);
		Cylinder{ Vec3{x1, y, z1}, Vec3{x0, y, z1}, r }.draw(color);
		Cylinder{ Vec3{x0, y, z1}, Vec3{x0, y, z0}, r }.draw(color);

		// チャンク座標ラベル
		const Vec3 center{ x0 + CHUNK_SIZE * 0.5, 1.0, z0 + CHUNK_SIZE * 0.5 };
		const Float3 sp = camera.camera3D().worldToScreenPoint(Float3{ center });
		if (sp.z > 0.0f)
			m_font(U"({},{})"_fmt(chunk->coord.x, chunk->coord.y))
				.draw(Vec2{ sp.x, sp.y }, color);
	}
}

// ===== D-03: 車両デバッグ情報 =====

static StringView vehicleStateStr(VehicleState s)
{
	switch (s)
	{
	case VehicleState::Moving:            return U"Moving";
	case VehicleState::WaitingSignal:     return U"Signal";
	case VehicleState::WaitingBusStop:    return U"BusStop";
	case VehicleState::YieldingEmergency: return U"Yield";
	case VehicleState::Parking:           return U"Park";
	}
	return U"?";
}

void DebugRenderer::renderVehicleInfo(const Array<Vehicle>& vehicles, const GameCamera& camera)
{
	for (const auto& v : vehicles)
	{
		// 進行方向インジケータ（前方 6m）
		const Vec3 fwd{
			Math::Sin(v.heading) * 6.0,
			0.0,
			Math::Cos(v.heading) * 6.0
		};
		const Vec3 base = v.position + Vec3{ 0, 1, 0 };
		Cylinder{ base, base + fwd, 0.3 }.draw(ColorF{ 1, 0.8, 0 });

		// ラベル
		const Float3 sp = camera.camera3D().worldToScreenPoint(
			Float3{ v.position + Vec3{ 0, 7, 0 } });
		if (sp.z > 0.0f)
			m_font(U"#{} {:.1f}m/s E:{} [{}]"_fmt(
				v.id, v.speed, v.currentEdge, vehicleStateStr(v.state)))
				.draw(Vec2{ sp.x, sp.y }, Palette::White);
	}
}

// ===== D-04: ワールド座標グリッド =====

void DebugRenderer::renderGrid(const GameCamera& camera)
{
	constexpr double step   = static_cast<double>(CHUNK_SIZE);  // 1024m
	constexpr double extent = step * 3.5;                        // ±3584m
	constexpr double r      = 0.3;
	const ColorF majorColor{ 1, 1, 1, 0.35 };

	const Vec3   focus = camera.focusPoint();
	const double baseX = Math::Floor(focus.x / step) * step;
	const double baseZ = Math::Floor(focus.z / step) * step;

	for (int i = -3; i <= 3; ++i)
	{
		const double x = baseX + i * step;
		Cylinder{ Vec3{x, 0.1, focus.z - extent}, Vec3{x, 0.1, focus.z + extent}, r }.draw(majorColor);

		const double z = baseZ + i * step;
		Cylinder{ Vec3{focus.x - extent, 0.1, z}, Vec3{focus.x + extent, 0.1, z}, r }.draw(majorColor);
	}
}

// ===== D-05: HUD 詳細 =====

void DebugRenderer::renderDetailHUD(const RoadNetwork& network,
                                    const World& world,
                                    const GameCamera& camera,
                                    const Array<Vehicle>& vehicles)
{
	const Vec3 eye   = camera.eyePosition();
	const Vec3 focus = camera.focusPoint();

	int nodeCount = 0;
	for (const auto& n : network.nodes()) if (n.id != -1) ++nodeCount;
	int edgeCount = 0;
	for (const auto& e : network.edges()) if (e.id != -1) ++edgeCount;

	int movingCount = 0;
	for (const auto& v : vehicles)
		if (v.state == VehicleState::Moving) ++movingCount;

	const Array<String> lines = {
		U"[DEBUG]",
		U"--- Camera ---",
		U"Eye:   ({:.0f}, {:.0f}, {:.0f})"_fmt(eye.x, eye.y, eye.z),
		U"Focus: ({:.0f}, {:.0f}, {:.0f})"_fmt(focus.x, focus.y, focus.z),
		U"Yaw:{:.1f}  Pitch:{:.1f}  Dist:{:.0f}m"_fmt(
			Math::ToDegrees(camera.yaw()),
			Math::ToDegrees(camera.pitch()),
			camera.distance()),
		U"",
		U"--- Road ---",
		U"Nodes:{}  Edges:{}"_fmt(nodeCount, edgeCount),
		U"",
		U"--- World ---",
		U"ActiveChunks: {}"_fmt(world.getActiveChunks().size()),
		U"",
		U"--- Traffic ---",
		U"Vehicles: {}  (Moving:{})"_fmt(vehicles.size(), movingCount),
	};

	const Size sz = Scene::Size();
	constexpr double panelW = 280.0;
	constexpr double lineH  = 18.0;
	const double panelH = lineH * lines.size() + 16.0;

	const RectF panel{ sz.x - panelW - 10, 10, panelW, panelH };
	panel.draw(ColorF{ 0, 0, 0, 0.75 });
	panel.drawFrame(1.0, ColorF{ 0.5, 0.5, 0.5 });

	double y = 18.0;
	for (const auto& line : lines)
	{
		m_font(line).draw(Vec2{ sz.x - panelW - 2, y }, Palette::White);
		y += lineH;
	}
}

// ===== D-06: オンスクリーンログ =====

void DebugRenderer::renderLog()
{
	const double now = Scene::Time();
	const Size   sz  = Scene::Size();
	constexpr double lineH = 18.0;
	double y = sz.y - lineH - 5.0;

	for (int i = static_cast<int>(DebugLog::entries().size()) - 1; i >= 0; --i)
	{
		const auto& entry = DebugLog::entries()[i];
		const double elapsed = now - entry.time;
		if (elapsed > DebugLog::FADE_DURATION) continue;

		const double alpha = 1.0 - elapsed / DebugLog::FADE_DURATION;
		m_font(entry.text).draw(
			Arg::topRight = Vec2{ sz.x - 10.0, y },
			ColorF{ 1, 1, 1, alpha });
		y -= lineH;
		if (y < 0) break;
	}
}

// ===== ヘルプパネル (F3+/) =====

void DebugRenderer::renderHelp()
{
	const Size sz = Scene::Size();
	const Vec2 center{ sz.x * 0.5, sz.y * 0.5 };
	constexpr double panelW = 320.0;
	constexpr double panelH = 210.0;
	constexpr double lineH  = 22.0;

	const RectF panel{ Arg::center = center, panelW, panelH };
	panel.draw(ColorF{ 0, 0, 0, 0.85 });
	panel.drawFrame(1.5, Palette::White);

	const double x = center.x - panelW * 0.5 + 16.0;
	double y = center.y - panelH * 0.5 + 12.0;

	const StringView helpLines[] = {
		U"DEBUG MODE  (F3 to toggle)",
		U"",
		U"F3+N   Network visualizer",
		U"F3+C   Chunk boundaries",
		U"F3+V   Vehicle debug info",
		U"F3+G   World grid",
		U"F3+H   Detail HUD",
		U"F3+/   This help",
	};

	bool first = true;
	for (const auto line : helpLines)
	{
		m_font(line).draw(Vec2{ x, y }, first ? Palette::Yellow : Palette::White);
		first = false;
		y += lineH;
	}

	// 残り時間バー
	const double remaining = 10.0 - (Scene::Time() - m_helpOpenTime);
	const double ratio     = Clamp(remaining / 10.0, 0.0, 1.0);
	const double barX      = center.x - panelW * 0.5;
	const double barY      = center.y + panelH * 0.5 - 4.0;
	RectF{ barX, barY, panelW * ratio, 4.0 }.draw(ColorF{ 0.4, 0.8, 1.0, 0.8 });
}
