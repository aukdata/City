#include "../../stdafx.h"
#include "PlaceNameRenderer.hpp"

// カメラ距離でフォントサイズを決める閾値
namespace
{
	constexpr float kHideDistance  = 2000.0f;  ///< これより遠い集落は非表示
	constexpr float kSmallDistance =  800.0f;  ///< これより近いと大きく表示
	constexpr double kLabelOffsetY = 30.0;     ///< 地名表示の高さオフセット [m]
}

PlaceNameRenderer::PlaceNameRenderer()
	: m_font{ FontMethod::MSDF, 24, Typeface::CJK_Regular_JP }
{
}

void PlaceNameRenderer::render(const Array<MapGenerator::Settlement>& settlements,
                               const GameCamera& camera,
                               const World& world) const
{
	const auto& cam3D = camera.camera3D();
	const double camDist = camera.distance();

	// カメラが非常に近いときは地名表示不要
	if (camDist > kHideDistance) return;

	for (const auto& s : settlements)
	{
		if (s.name.isEmpty()) continue;

		// 地名の表示位置：集落中心の地形高さ + オフセット
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
		if (sp.z <= 0.0f) continue;  // カメラ後方は非表示

		// 集落とカメラの水平距離でサイズ・透明度を調整
		const float dx = worldPos.x - static_cast<float>(camera.eyePosition().x);
		const float dz = worldPos.z - static_cast<float>(camera.eyePosition().z);
		const float dist = Math::Sqrt(dx * dx + dz * dz);
		if (dist > kHideDistance) continue;

		const double t     = Clamp(static_cast<double>(dist - kSmallDistance) / (kHideDistance - kSmallDistance), 0.0, 1.0);
		const double alpha = 1.0 - t;
		const double scale = Math::Lerp(1.0, 0.65, t);

		const Vec2 screenPos{ sp.x, sp.y };

		// 縁取りテキスト（黒縁 + 白文字）
		m_font(s.name)
			.drawAt(TextStyle::Outline(0.2, ColorF{ 0.0, alpha * 0.85 }),
			        static_cast<double>(m_font.fontSize()) * scale,
			        screenPos,
			        ColorF{ 1.0, 1.0, 0.9, alpha });
	}
}
