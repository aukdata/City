#include "../../stdafx.h"
#include "MinimapRenderer.hpp"

namespace
{
	/// @brief 高さ [m] → 地形色
	Color heightToColor(float h)
	{
		if (h < 0.0f)
		{
			const float t = Clamp(-h / 30.0f, 0.0f, 1.0f);
			return Color{
				static_cast<uint8>(Math::Lerp(70.0, 30.0, t)),
				static_cast<uint8>(Math::Lerp(130.0, 80.0, t)),
				static_cast<uint8>(Math::Lerp(180.0, 150.0, t))
			};
		}
		if (h < 50.0f)
		{
			const float t = h / 50.0f;
			return Color{
				static_cast<uint8>(Math::Lerp(90.0, 120.0, t)),
				static_cast<uint8>(Math::Lerp(160.0, 140.0, t)),
				static_cast<uint8>(Math::Lerp(60.0, 70.0, t))
			};
		}
		if (h < 200.0f)
		{
			const float t = (h - 50.0f) / 150.0f;
			return Color{
				static_cast<uint8>(Math::Lerp(120.0, 160.0, t)),
				static_cast<uint8>(Math::Lerp(140.0, 130.0, t)),
				static_cast<uint8>(Math::Lerp(70.0, 90.0, t))
			};
		}
		const float t = Clamp((h - 200.0f) / 300.0f, 0.0f, 1.0f);
		return Color{
			static_cast<uint8>(Math::Lerp(160.0, 240.0, t)),
			static_cast<uint8>(Math::Lerp(130.0, 240.0, t)),
			static_cast<uint8>(Math::Lerp(90.0, 245.0, t))
		};
	}

	/// @brief 道路種別 → ミニマップ上の色
	Color roadTypeColor(RoadType rt)
	{
		switch (rt)
		{
		case RoadType::Expressway: return Color{ 100, 200, 100 };
		case RoadType::Highway:    return Color{ 100, 200, 100 };
		case RoadType::Arterial:   return Color{ 230, 200, 80 };
		default:                   return Color{ 200, 200, 200 };
		}
	}

	/// @brief 拡大パネルID
	constexpr StringView kPanelId = U"minimap_expanded";
}

void MinimapRenderer::buildTerrainTexture(const World& world)
{
	// ワールド全体を固定解像度へサンプリングし、標高ベースの地形テクスチャを事前生成する。
	m_worldMinX = 0.0f;
	m_worldMinZ = 0.0f;
	m_worldMaxX = WORLD_SIZE;
	m_worldMaxZ = WORLD_SIZE;

	Image img{ kMapSize, kMapSize, Palette::Black };

	const float stepX = (m_worldMaxX - m_worldMinX) / kMapSize;
	const float stepZ = (m_worldMaxZ - m_worldMinZ) / kMapSize;

	for (int py = 0; py < kMapSize; ++py)
	{
		for (int px = 0; px < kMapSize; ++px)
		{
			const float wx = m_worldMinX + (px + 0.5f) * stepX;
			const float wz = m_worldMinZ + (py + 0.5f) * stepZ;
			const float h = world.computeHeight(wx, wz);
			img[py][px] = heightToColor(h);
		}
	}

	m_terrainTex = DynamicTexture{ img };
}

void MinimapRenderer::updateRoadOverlay(const RoadNetwork& network, [[maybe_unused]] const World& world)
{
	m_mapDirty = true;
	// 道路はベジェを短区間へ分割して別テクスチャへ焼き込み、地形の上へ重ねて表示する。
	m_roadImage = Image{ kMapSize, kMapSize, Color{ 0, 0, 0, 0 } };

	for (const auto& edge : network.edges())
	{
		if (edge.id < 0 || edge.edgeState == EdgeState::Planned) continue;
		const auto bez = network.getBezier(edge.id);
		if (!bez) continue;

		const Color col = roadTypeColor(edge.roadType);
		constexpr int kSegments = 32;
		for (int i = 0; i < kSegments; ++i)
		{
			const Vec3 a = bez->positionAt(bez->totalLength * (static_cast<float>(i) / kSegments));
			const Vec3 b = bez->positionAt(bez->totalLength * (static_cast<float>(i + 1) / kSegments));
			Line{ worldToPixel(static_cast<float>(a.x), static_cast<float>(a.z)),
			      worldToPixel(static_cast<float>(b.x), static_cast<float>(b.z)) }
				.overwrite(m_roadImage, 1, col);
		}
	}

	m_roadTex = DynamicTexture{ m_roadImage };
}

void MinimapRenderer::updateRoadOverlayAround(const Array<int>& dirtyNodeIds,
                                               const RoadNetwork& network)
{
	m_mapDirty = true;
	// 局所更新では変更ノード周辺の道路だけを消して描き直し、全面再生成を避ける。
	if (m_roadImage.isEmpty()) return;

	// 変更ノードに接続するエッジを収集
	HashSet<int> edgeIds;
	for (const int nid : dirtyNodeIds)
	{
		const RoadNode* node = network.getNode(nid);
		if (!node) continue;
		for (const auto& att : node->attachments)
			edgeIds.insert(att.edgeId);
	}

	// 該当エッジを透明で消去して再描画
	constexpr int kSegments = 32;
	const Color clear{ 0, 0, 0, 0 };

	for (const int eid : edgeIds)
	{
		const RoadEdge* edge = network.getEdge(eid);
		if (!edge) continue;
		const auto bez = network.getBezier(eid);
		if (!bez) continue;

		// まず透明で消去（太めに3pxで消す）
		for (int i = 0; i < kSegments; ++i)
		{
			const Vec3 a = bez->positionAt(bez->totalLength * (static_cast<float>(i) / kSegments));
			const Vec3 b = bez->positionAt(bez->totalLength * (static_cast<float>(i + 1) / kSegments));
			Line{ worldToPixel(static_cast<float>(a.x), static_cast<float>(a.z)),
			      worldToPixel(static_cast<float>(b.x), static_cast<float>(b.z)) }
				.overwrite(m_roadImage, 3, clear);
		}
	}

	for (const int eid : edgeIds)
	{
		const RoadEdge* edge = network.getEdge(eid);
		if (!edge || edge->edgeState == EdgeState::Planned) continue;
		const auto bez = network.getBezier(eid);
		if (!bez) continue;

		const Color col = roadTypeColor(edge->roadType);
		for (int i = 0; i < kSegments; ++i)
		{
			const Vec3 a = bez->positionAt(bez->totalLength * (static_cast<float>(i) / kSegments));
			const Vec3 b = bez->positionAt(bez->totalLength * (static_cast<float>(i + 1) / kSegments));
			Line{ worldToPixel(static_cast<float>(a.x), static_cast<float>(a.z)),
			      worldToPixel(static_cast<float>(b.x), static_cast<float>(b.z)) }
				.overwrite(m_roadImage, 1, col);
		}
	}

	m_roadTex.fill(m_roadImage);
}

// =============================================================================
// 入力処理
// =============================================================================

Optional<Vec2> MinimapRenderer::update(const GameCamera& camera, const RoadNetwork& roads,
	const TrainNetwork& railway, const Array<MapGenerator::Settlement>& settlements)
{
	m_consumedInput = m_map.visible;
	if (m_map.visible) { return m_map.update(Scene::Size()); }
	if (!KeyM.down() && !(m_smallVisible && smallRect().leftClicked())) { return none; }
	m_consumedInput=true;
	openFullScreen(camera,roads,railway,settlements); return none;
}

void MinimapRenderer::openFullScreen(const GameCamera& camera,const RoadNetwork& roads,const TrainNetwork& railway,const Array<MapGenerator::Settlement>& settlements)
{
	m_consumedInput = true;
	if (m_mapDirty)
	{
		m_map.streets.clear(); m_map.labels.clear();
		const auto append = [&](const CubicBezier& curve, double width, int category)
		{
			WorldMapView::Stroke stroke; stroke.width=width; stroke.category=category;
			Vec2 lower{1e9,1e9}, upper{-1e9,-1e9};
			const int count=Max(2, static_cast<int>(curve.totalLength/30)+1);
			for (int index=0;index<=count;++index)
			{
				const Vec3 point=curve.positionAt(curve.totalLength*index/count);
				stroke.points << Vec2{point.x,point.z};
				lower.x=Min(lower.x,point.x); lower.y=Min(lower.y,point.z);
				upper.x=Max(upper.x,point.x); upper.y=Max(upper.y,point.z);
			}
			stroke.bounds=RectF{lower,upper-lower}.stretched(30);
			m_map.streets << std::move(stroke);
		};
		for (const auto& edge : roads.edges())
		{
			if (edge.id<0 || edge.edgeState==EdgeState::Planned) { continue; }
			if (const auto curve=roads.getBezier(edge.id)) { append(*curve,edge.totalWidth(),edge.roadType==RoadType::LocalRoad ? 0 : 1); }
		}
		for (const auto& edge : railway.edges())
		{
			if (edge.id<0) { continue; }
			if (const auto curve=railway.getBezier(edge.id)) { append(*curve,4,2); }
		}
		for (const auto& node : railway.nodes())
		{
			if (node.type==TrackNodeType::Station) { m_map.labels << WorldMapView::Label{{node.position.x,node.position.z},node.name+U"駅",true}; }
		}
		m_map.rivers.clear(); m_map.boundaries.clear();
		if (m_world) for (const auto& reach : m_world->rivers().reaches) { m_map.rivers << WorldMapView::Stroke{{{reach.start.x,reach.start.z},{reach.end.x,reach.end.z}},reach.bounds,reach.halfWidth*2,0}; }
		if (m_districts)
		{
			for (const auto& area : m_districts->areas)
			{
				const String label=area.level==2 && area.urban ? m_districts->areas[area.parent].name+area.name : area.name;
				m_map.labels << WorldMapView::Label{area.center,label,false,area.level==0 ? 1.0 : area.level==1 ? 8.0 : 36.0};
			}
			for (const auto& border : m_districts->boundaries) { m_map.boundaries << WorldMapView::Border{border.a,border.b,border.level}; }
			m_map.addressAt=[this](Vec2 p) { return m_districts->address(p); };
		}
		else { for (const auto& town : settlements) { m_map.labels << WorldMapView::Label{town.center,town.name,false}; } }
		m_mapDirty=false;
	}
	const Vec3 focus=camera.focusPoint();
	m_map.open({focus.x,focus.z});
}

void MinimapRenderer::drawFullScreen(const GameCamera& camera) const
{
	const Vec3 focus=camera.focusPoint();
	m_map.draw(Scene::Size(),m_terrainTex,m_font,{focus.x,focus.z});
}

void MinimapRenderer::render(const GameCamera& camera,
                             const Array<MapGenerator::Settlement>& settlements) const
{
	if (m_terrainTex.isEmpty() || !m_smallVisible) return;

	const RectF rect = smallRect();

	// 背景枠
	rect.stretched(2).draw(ColorF{ 0.0, 0.0, 0.0, 0.7 });
	rect.stretched(2).drawFrame(1.0, ColorF{ 0.6, 0.6, 0.6, 0.8 });

	drawMapContent(rect, camera, settlements, true);
}

// =============================================================================
// 拡大パネル描画
// =============================================================================

void MinimapRenderer::drawExpandedPanel(PanelManager& panels,
                                        const GameCamera& camera,
                                        const Array<MapGenerator::Settlement>& settlements) const
{
	if (m_terrainTex.isEmpty()) return;

	auto content = panels.beginContent(kPanelId);
	if (!content) return;

	// beginContent で座標系がパネルコンテンツ領域ローカルになっている
	// パネルの state から実サイズを取得する代わりに、登録サイズを利用
	// → パネル内のコンテンツ領域は (0, 0) から始まる
	const double side = Min(Scene::Width(), Scene::Height()) - 80.0;
	const double contentH = side - PanelManager::kTitleBarH;
	const RectF mapRect{ 0.0, 0.0, side, contentH };

	drawMapContent(mapRect, camera, settlements, true);

	panels.reportContentHeight(kPanelId, contentH);
}

// =============================================================================
// 共通マップ描画
// =============================================================================

void MinimapRenderer::drawMapContent(const RectF& rect,
                                     const GameCamera& camera,
                                     const Array<MapGenerator::Settlement>& settlements,
                                     bool showLabels) const
{
	// 小マップと拡大マップはこの共通描画へ集約し、背景・道路・ラベル・視点表示を同じ手順で描く。
	// 地形テクスチャ
	m_terrainTex.resized(rect.size).draw(rect.pos);

	// 道路オーバーレイ
	if (!m_roadTex.isEmpty())
	{
		m_roadTex.resized(rect.size).draw(rect.pos);
	}

	// 地名描画（城下町 / 宿場町 のみ）
	if (showLabels)
	{
		const double scale = rect.w / kDisplaySize;  // 拡大率
		for (const auto& s : settlements)
		{
			if (s.name.isEmpty()) continue;
			if (s.kind == MapGenerator::SettlementKind::RuralSettlement) continue;

			const Vec2 sp = worldToScreen(static_cast<float>(s.center.x),
			                              static_cast<float>(s.center.y), rect);
			if (!rect.contains(sp)) continue;

			const double fontSize = (s.kind == MapGenerator::SettlementKind::RegionalCity)
				? 10.0 * scale : 8.0 * scale;
			m_font(s.name).drawAt(TextStyle::Outline(0.3, ColorF{ 0.0, 0.8 }),
			                      fontSize, sp, ColorF{ 1.0, 1.0, 0.9, 0.9 });
		}
	}

	// カメラ位置インジケータ
	{
		const Vec3 focus = camera.focusPoint();
		const Vec2 cp = worldToScreen(static_cast<float>(focus.x),
		                              static_cast<float>(focus.z), rect);

		if (rect.contains(cp))
		{
			const double scale = rect.w / kDisplaySize;
			const float yaw = camera.yaw();
			const double size = 6.0 * scale;
			const Vec2 dir{ Math::Sin(yaw), Math::Cos(yaw) };
			const Vec2 perp{ dir.y, -dir.x };

			const Vec2 tip   = cp - dir * size;
			const Vec2 left  = cp + dir * size * 0.5 - perp * size * 0.6;
			const Vec2 right = cp + dir * size * 0.5 + perp * size * 0.6;

			Triangle{ tip, left, right }.draw(ColorF{ 1.0, 1.0, 0.3, 0.9 });
			Triangle{ tip, left, right }.drawFrame(1.0, ColorF{ 1.0, 1.0, 1.0, 0.9 });
		}
	}
}

// =============================================================================
// private ユーティリティ
// =============================================================================

Vec2 MinimapRenderer::worldToScreen(float wx, float wz, const RectF& rect) const
{
	const float tx = (wx - m_worldMinX) / (m_worldMaxX - m_worldMinX);
	const float tz = (wz - m_worldMinZ) / (m_worldMaxZ - m_worldMinZ);
	return Vec2{
		rect.x + tx * rect.w,
		rect.y + tz * rect.h
	};
}

Point MinimapRenderer::worldToPixel(float wx, float wz) const
{
	const float tx = (wx - m_worldMinX) / (m_worldMaxX - m_worldMinX);
	const float tz = (wz - m_worldMinZ) / (m_worldMaxZ - m_worldMinZ);
	return Point{
		Clamp(static_cast<int>(tx * kMapSize), 0, kMapSize - 1),
		Clamp(static_cast<int>(tz * kMapSize), 0, kMapSize - 1)
	};
}

RectF MinimapRenderer::smallRect() const
{
	if (m_smallBounds) { return *m_smallBounds; }
	return RectF{
		Scene::Width() - kDisplaySize - kMargin,
		kMargin,
		kDisplaySize,
		kDisplaySize
	};
}
