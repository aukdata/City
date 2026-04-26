#include "../../stdafx.h"
#include "PlaceNameRenderer.hpp"
#include "../asset/AssetRegistrar.hpp"

namespace
{
	constexpr float  kHideDistance  = 4000.0f;  ///< これより遠い地区は非表示
	constexpr float  kSmallDistance = 1600.0f;  ///< この距離でフェード開始
	constexpr double kLabelOffsetY  =   30.0;   ///< 地名表示の高さオフセット [m]
	constexpr double kRomajiScale   =    0.55;  ///< 漢字サイズに対するローマ字の比率
}

void PlaceNameRenderer::render(const Array<MapGenerator::Settlement>& settlements,
                               const GameCamera& camera,
                               const World& world) const
{
	// 地名は地区中心を 2D 投影し、距離に応じた縮小・フェード付きビルボードとして重ねる。
	const auto& m_font = FontAsset(Asset::CJK24);
	const auto& cam3D    = camera.camera3D();
	const double camDist = camera.distance();

	if (camDist > kHideDistance) return;

	for (const auto& s : settlements)
	{
		if (s.name.isEmpty()) continue;

		// 地名の表示位置：地区中心の地形高さ + オフセット
		const float groundY = world.sampleHeight(
			static_cast<float>(s.center.x),
			static_cast<float>(s.center.y));

		const Float3 worldPos{
			static_cast<float>(s.center.x),
			groundY + static_cast<float>(kLabelOffsetY),
			static_cast<float>(s.center.y)
		};

		// 3D → スクリーン座標変換
		const Float3 sp = cam3D.worldToScreenPoint(worldPos);
		if (sp.z <= 0.0f) continue;

		// カメラとの水平距離でフェード
		const float dist = camera.horizontalDistanceTo(worldPos);
		if (dist > kHideDistance) continue;

		const double t     = Math::Clamp(static_cast<double>(dist - kSmallDistance) / (kHideDistance - kSmallDistance), 0.0, 1.0);
		const double alpha = 1.0 - t;
		const double scale = Math::Lerp(1.0, 0.65, t);

		const Vec2 screenPos{ sp.x, sp.y };
		const double kanjiSize  = m_font.fontSize() * scale;
		const double romajiSize = kanjiSize * kRomajiScale;

		// 背景付きの 2 段ラベルにするため、漢字とローマ字の領域を先にまとめて確定する。
		const auto kanjiRegion  = m_font(s.name).regionAt(kanjiSize, screenPos);
		const RectF romajiRegion = s.reading.isEmpty()
			? RectF{}
			: m_font(s.reading).regionAt(romajiSize, Vec2{ screenPos.x, kanjiRegion.bottomY() + romajiSize * 0.15 });

		const RectF bgRect = s.reading.isEmpty()
			? kanjiRegion.stretched(6, 3)
			: RectF{ Arg::topLeft(
				Min(kanjiRegion.x, romajiRegion.x) - 6,
				kanjiRegion.y - 3),
				Max(kanjiRegion.w, romajiRegion.w) + 12,
				kanjiRegion.h + romajiRegion.h + 6 };

		bgRect.rounded(4).draw(ColorF{ 0.1, 0.1, 0.1, alpha * 0.55 });

		// 漢字地名（黒縁 + 白文字）
		m_font(s.name)
			.drawAt(TextStyle::Outline(0.2, ColorF{ 0.0, alpha * 0.85 }),
			        kanjiSize,
			        screenPos,
			        ColorF{ 1.0, 1.0, 0.9, alpha });

		// ローマ字読み（漢字の直下に小さく表示）
		if (!s.reading.isEmpty())
		{
			const Vec2 romajiPos{ screenPos.x, kanjiRegion.bottomY() + romajiSize * 0.15 };
			m_font(s.reading)
				.drawAt(TextStyle::Outline(0.2, ColorF{ 0.0, alpha * 0.7 }),
				        romajiSize,
				        romajiPos,
				        ColorF{ 0.95, 0.95, 1.0, alpha * 0.9 });
		}
	}
}
