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
	m_worldMinX = 0.0f;
	m_worldMinZ = 0.0f;
	m_worldMaxX = static_cast<float>(WORLD_CHUNKS * CHUNK_SIZE);
	m_worldMaxZ = static_cast<float>(WORLD_CHUNKS * CHUNK_SIZE);

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

void MinimapRenderer::update(PanelManager& panels)
{
	if (m_terrainTex.isEmpty()) return;

	// 小さいミニマップをクリック → 拡大パネルを表示（パネル上のクリックは無視）
	if (!panels.isVisible(kPanelId) && !panels.isMouseOnAnyPanel()
		&& !panels.consumedInput() && smallRect().leftClicked())
	{
		const double side = Min(Scene::Width(), Scene::Height()) - 80.0;
		const Vec2 pos{
			(Scene::Width() - side) * 0.5,
			(Scene::Height() - side) * 0.5
		};
		panels.show(kPanelId, U"全体マップ", pos);
	}
}

// =============================================================================
// 右上の小さいミニマップ描画
// =============================================================================

void MinimapRenderer::render(const GameCamera& camera,
                             const Array<MapGenerator::Settlement>& settlements) const
{
	if (m_terrainTex.isEmpty()) return;

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
	// 地形テクスチャ
	m_terrainTex.resized(rect.size).draw(rect.pos);

	// 道路オーバーレイ
	if (!m_roadTex.isEmpty())
	{
		m_roadTex.resized(rect.size).draw(rect.pos);
	}

	// 地名描画（Urban / Suburbs のみ）
	if (showLabels)
	{
		const double scale = rect.w / kDisplaySize;  // 拡大率
		for (const auto& s : settlements)
		{
			if (s.name.isEmpty()) continue;
			if (s.type == MapGenerator::SettlementType::Rural) continue;

			const Vec2 sp = worldToScreen(static_cast<float>(s.center.x),
			                              static_cast<float>(s.center.y), rect);
			if (!rect.contains(sp)) continue;

			const double fontSize = (s.type == MapGenerator::SettlementType::Urban)
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
	return RectF{
		Scene::Width() - kDisplaySize - kMargin,
		kMargin,
		kDisplaySize,
		kDisplaySize
	};
}
