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
	if (KeyB.down())     { m_showBiomes    = !m_showBiomes;    m_debugMode = true; anyChord = true; }
	if (KeySlash.down()) { m_showDetailHUD = !m_showDetailHUD; m_debugMode = true; anyChord = true; }
	if (KeyH.down())
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
	if (m_showBiomes)   renderBiomes(world, camera);
	if (m_showNetwork)  renderNetwork(network, camera);
	if (m_showChunks)   renderChunks(world, camera);
	if (m_showVehicles) renderVehicleInfo(vehicles, camera);

	// 注: 2D オーバーレイ (renderBiomeLegend) は renderProfiler 側で描画する
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
		U"F3+B   Biome overlay",
		U"F3+/   Detail HUD",
		U"F3+H   This help",
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

// ===== D-06: バイオーム表示 =====

void DebugRenderer::renderBiomes(const World& world, const GameCamera& camera)
{
	const auto& activeChunks = world.getActiveChunks();
	if (activeChunks.isEmpty()) return;

	// バイオーム → 色のマッピング
	auto biomeColor = [](BiomeType b) -> ColorF
	{
		switch (b)
		{
		case BiomeType::Ocean:         return ColorF{ 0.1, 0.2, 0.8, 0.35 };
		case BiomeType::Lake:          return ColorF{ 0.2, 0.4, 0.9, 0.35 };
		case BiomeType::CoastalPlain:  return ColorF{ 0.3, 0.7, 0.9, 0.35 };
		case BiomeType::CoastalHill:   return ColorF{ 0.2, 0.6, 0.5, 0.35 };
		case BiomeType::Plain:         return ColorF{ 0.5, 0.8, 0.2, 0.35 };
		case BiomeType::Basin:         return ColorF{ 0.3, 0.5, 0.4, 0.35 };
		case BiomeType::Hill:          return ColorF{ 0.2, 0.7, 0.3, 0.35 };
		case BiomeType::Foothill:      return ColorF{ 0.8, 0.7, 0.2, 0.35 };
		case BiomeType::Plateau:       return ColorF{ 0.8, 0.5, 0.2, 0.35 };
		case BiomeType::Mountain:      return ColorF{ 0.5, 0.3, 0.1, 0.35 };
		case BiomeType::MountainRange: return ColorF{ 0.9, 0.9, 0.9, 0.35 };
		default:                       return ColorF{ 1, 1, 1, 0.2 };
		}
	};

	// 粗いグリッド（8×8）で半透明ボックスを描画
	constexpr int kStep = 8;  // HEIGHT_CELLS / kStep = 8
	constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / HEIGHT_CELLS * kStep;

	for (const Chunk* chunk : activeChunks)
	{
		if (!chunk) continue;
		const float ox = static_cast<float>(chunk->coord.x * CHUNK_SIZE);
		const float oz = static_cast<float>(chunk->coord.y * CHUNK_SIZE);

		for (int gz = 0; gz < HEIGHT_CELLS; gz += kStep)
		{
			for (int gx = 0; gx < HEIGHT_CELLS; gx += kStep)
			{
				const float wx = ox + (gx + kStep * 0.5f) * (static_cast<float>(CHUNK_SIZE) / HEIGHT_CELLS);
				const float wz = oz + (gz + kStep * 0.5f) * (static_cast<float>(CHUNK_SIZE) / HEIGHT_CELLS);
				const float h  = world.sampleHeight(wx, wz);

				const BiomeType b = world.getBiome(wx, wz);
				const ColorF col = biomeColor(b).removeSRGBCurve();

				Box{ static_cast<double>(wx), static_cast<double>(h + 2.0f), static_cast<double>(wz),
				     static_cast<double>(cellSize), 4.0, static_cast<double>(cellSize) }
					.draw(col);
			}
		}
	}
}

void DebugRenderer::renderBiomeLegend()
{
	struct Entry { StringView name; ColorF color; };
	static const Entry kEntries[] = {
		{ U"Ocean",         ColorF{ 0.1, 0.2, 0.8 } },
		{ U"Lake",          ColorF{ 0.2, 0.4, 0.9 } },
		{ U"CoastalPlain",  ColorF{ 0.3, 0.7, 0.9 } },
		{ U"CoastalHill",   ColorF{ 0.2, 0.6, 0.5 } },
		{ U"Plain",         ColorF{ 0.5, 0.8, 0.2 } },
		{ U"Basin",         ColorF{ 0.3, 0.5, 0.4 } },
		{ U"Hill",          ColorF{ 0.2, 0.7, 0.3 } },
		{ U"Foothill",      ColorF{ 0.8, 0.7, 0.2 } },
		{ U"Plateau",       ColorF{ 0.8, 0.5, 0.2 } },
		{ U"Mountain",      ColorF{ 0.5, 0.3, 0.1 } },
		{ U"MountainRange", ColorF{ 0.9, 0.9, 0.9 } },
	};

	constexpr int kLineH = 18;
	constexpr int kSwatchW = 14;
	constexpr int kPadding = 8;
	constexpr int kEntryCount = static_cast<int>(std::size(kEntries));
	const int panelH = kPadding * 2 + kLineH * kEntryCount + kLineH;
	const int panelW = 180;
	const int px = 10;
	const int py = Scene::Height() - panelH - 10;

	RectF{ static_cast<double>(px), static_cast<double>(py),
	       static_cast<double>(panelW), static_cast<double>(panelH) }
		.draw(ColorF{ 0, 0, 0, 0.6 });

	m_font(U"Biomes (F3+B)").draw(Vec2{ px + kPadding, py + kPadding }, Palette::Yellow);

	for (int i = 0; i < kEntryCount; ++i)
	{
		const int y = py + kPadding + kLineH * (i + 1);
		RectF{ static_cast<double>(px + kPadding), static_cast<double>(y + 2),
		       static_cast<double>(kSwatchW), static_cast<double>(kSwatchW) }
			.draw(kEntries[i].color);
		m_font(kEntries[i].name).draw(
			Vec2{ px + kPadding + kSwatchW + 6, y }, Palette::White);
	}
}

// ===== プロファイラ HUD =====

void DebugRenderer::renderProfiler(double total, double logic, double sky, double terrain,
                                   double road, double zone, double vehicle, double train,
                                   double debug, double ui,
                                   const RoadNetwork& network)
{
	if (!m_debugMode) return;

	const int x = 10, y = 10;
	constexpr int lineH = 14;
	constexpr int lines = 12;
	RectF{ static_cast<double>(x - 4), static_cast<double>(y - 2),
	       220.0, static_cast<double>(lineH * lines + 6) }
		.draw(ColorF{ 0.0, 0.0, 0.0, 0.55 });

	const ColorF c{ 1.0 };
	m_font(U"FPS: {}  Total: {:.1f}ms"_fmt(Profiler::FPS(), total)).draw(x, y, c);
	m_font(U"Logic:   {:.1f}ms"_fmt(logic)).draw(x, y + lineH, c);
	m_font(U"Sky:     {:.1f}ms"_fmt(sky)).draw(x, y + lineH * 2, c);
	m_font(U"Terrain: {:.1f}ms"_fmt(terrain)).draw(x, y + lineH * 3, c);
	m_font(U"Road:    {:.1f}ms"_fmt(road)).draw(x, y + lineH * 4, c);
	m_font(U"Zone:    {:.1f}ms"_fmt(zone)).draw(x, y + lineH * 5, c);
	m_font(U"Vehicle: {:.1f}ms"_fmt(vehicle)).draw(x, y + lineH * 6, c);
	m_font(U"Train:   {:.1f}ms"_fmt(train)).draw(x, y + lineH * 7, c);
	m_font(U"Debug:   {:.1f}ms"_fmt(debug)).draw(x, y + lineH * 8, c);
	m_font(U"UI:      {:.1f}ms"_fmt(ui)).draw(x, y + lineH * 9, c);

	int liveEdges = 0, liveNodes = 0;
	for (const auto& e : network.edges()) if (e.id != -1) ++liveEdges;
	for (const auto& n : network.nodes()) if (n.id != -1) ++liveNodes;
	m_font(U"Edges:{} Nodes:{} Arr:{}/{}"_fmt(
		liveEdges, liveNodes,
		network.edges().size(), network.nodes().size()
	)).draw(x, y + lineH * 10, c);

	// 2D オーバーレイ（ここは 3D レンダーターゲット外なので正しく表示される）
	if (m_showBiomes) renderBiomeLegend();
}
