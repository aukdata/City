#include "../../stdafx.h"
#include "RoadRouteSignRenderer.hpp"
#include "../asset/AssetRegistrar.hpp"

namespace
{
	constexpr float  kHideDistance  = 3500.0f;   ///< これより遠い標識は非表示
	constexpr float  kFadeDistance  = 1600.0f;   ///< この距離でフェード開始
	constexpr double kLabelOffsetY  =   18.0;    ///< 標識の高さオフセット [m]
	constexpr double kSignSizeNear  =   72.0;    ///< 近距離時の標識高さ [px]
	constexpr double kSignSizeFar   =   40.0;    ///< 遠距離時の標識高さ [px]

	/// @brief RoadRoute の代表表示位置を返す（route 全体に沿って等間隔に N 個）
	Array<Vec3> routeAnchors(const RoadRoute& route, const RoadNetwork& network, int count)
	{
		Array<Vec3> result;
		if (route.edgeIds.isEmpty() || count <= 0) return result;

		const int n = static_cast<int>(route.edgeIds.size());
		for (int k = 0; k < count; ++k)
		{
			// 等間隔サンプル: t = (k + 0.5) / count を [0, n) に写像
			const double tGlobal = (k + 0.5) / count;
			const double pos     = tGlobal * n;
			const int    idx     = Clamp(static_cast<int>(pos), 0, n - 1);
			const float  tLocal  = static_cast<float>(pos - idx);

			const auto bezier = network.getBezier(route.edgeIds[idx]);
			if (not bezier) continue;
			result << bezier->evaluate(tLocal);
		}
		return result;
	}
}

void RoadRouteSignRenderer::render(const RoadNetwork& network, const GameCamera& camera) const
{
	const Texture& tex = TextureAsset(Asset::NationalRoadSign);
	if (not tex) return;

	const Font& fontNum = FontAsset(Asset::Arial24);

	const auto&  cam3D   = camera.camera3D();
	const double camDist = camera.distance();
	if (camDist > kHideDistance) return;

	for (const auto& route : network.routes())
	{
		if (route.id < 0) continue;                                // 削除済み
		if (route.kind != RoadRouteKind::NationalRoute) continue;
		if (route.number <= 0) continue;

		// route 全長を 2 等分し、各区間の中央に標識を配置
		for (const Vec3& anchor : routeAnchors(route, network, 2))
		{
			const Float3 worldPos{
				static_cast<float>(anchor.x),
				static_cast<float>(anchor.y + kLabelOffsetY),
				static_cast<float>(anchor.z)
			};

			// 3D → スクリーン投影
			const Float3 sp = cam3D.worldToScreenPoint(worldPos);
			if (sp.z <= 0.0f) continue;

			// カメラとの水平距離でフェード
			const float dist = camera.horizontalDistanceTo(worldPos);
			if (dist > kHideDistance) continue;

			const double t     = Math::Clamp(static_cast<double>(dist - kFadeDistance) / (kHideDistance - kFadeDistance), 0.0, 1.0);
			const double alpha = 1.0 - t;
			const double size  = Math::Lerp(kSignSizeNear, kSignSizeFar, t);

			const Vec2 screenPos{ sp.x, sp.y };

			// 標識本体（お握り型テクスチャ）
			tex.resized(size).drawAt(screenPos, ColorF{ 1.0, alpha });

			// 号数（Arial・フチなし）を標識中央からやや上に配置
			const double numberSize = size * 0.361;  // 0.38 * 0.95
			const Vec2   numberPos{ screenPos.x, screenPos.y - size * 0.02 };
			fontNum(route.number).drawAt(numberSize, numberPos, ColorF{ 1.0, 1.0, 1.0, alpha });
		}
	}
}
