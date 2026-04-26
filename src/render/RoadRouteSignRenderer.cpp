#include "../../stdafx.h"
#include "RoadRouteSignRenderer.hpp"
#include "../asset/AssetRegistrar.hpp"
#include "../world/Chunk.hpp"

namespace
{
	constexpr float  kHideDistance  = 3500.0f;   ///< これより遠い標識は非表示
	constexpr float  kFadeDistance  = 1600.0f;   ///< この距離でフェード開始
	constexpr double kLabelOffsetY  =   18.0;    ///< 標識の高さオフセット [m]
	constexpr double kSignSizeNear  =   72.0;    ///< 近距離時の標識高さ [px]
	constexpr double kSignSizeFar   =   40.0;    ///< 遠距離時の標識高さ [px]
	constexpr double kMinHitAlpha   =    0.5;    ///< この alpha 未満の標識はヒット判定しない

	/// @brief アンカーキャッシュを強制再構築する間隔 [frame]（ネットワーク規模の変化以外の編集を拾う保険）
	constexpr int    kRebuildIntervalFrames = 30;

	/// @brief 国道標識の 2D ビルボード表示位置を交差点先 50m ベースで列挙する
	/// @details 同一ルート内で同じチャンクに複数候補があるときは最初の 1 つだけ採用する
	Array<Vec3> routeAnchors(const RoadRoute& route, const RoadNetwork& network)
	{
		Array<Vec3> result;
		HashSet<uint64> usedChunks;

		for (const auto& [edgeId, arcLen] : network.routeSignAnchors(route))
		{
			const auto bezier = network.getBezier(edgeId);
			if (!bezier) continue;

			const Vec3 pos = bezier->positionAt(arcLen);
			const int32 cx = static_cast<int32>(Math::Floor(pos.x / CHUNK_SIZE));
			const int32 cz = static_cast<int32>(Math::Floor(pos.z / CHUNK_SIZE));
			const uint64 key = (static_cast<uint64>(static_cast<uint32>(cx)) << 32)
			                 | static_cast<uint64>(static_cast<uint32>(cz));
			if (!usedChunks.emplace(key).second) continue;

			result << pos;
		}
		return result;
	}
}

void RoadRouteSignRenderer::render(const RoadNetwork& network, const GameCamera& camera) const
{
	// 路線形状に依存するアンカーはキャッシュし、カメラ依存の投影とフェードだけ毎フレーム更新する。
	m_hits.clear();

	const Texture& tex = TextureAsset(Asset::NationalRoadSign);
	if (not tex) return;

	const Font& fontNum = FontAsset(Asset::Arial24);

	const auto&  cam3D   = camera.camera3D();
	const double camDist = camera.distance();
	if (camDist > kHideDistance) return;

	// --- アンカーキャッシュの再構築判定 ---
	// ノード/エッジ/ルート数が変わった、明示的に invalidate された、
	// または一定フレームを経過した場合に全再計算する。
	// ルート内 edgeIds の並び替えやベジエ形状のドラッグは「件数」では拾えないため、
	// 保険として kRebuildIntervalFrames 周期でも再計算する。
	const size_t nodeCount  = network.nodes().size();
	const size_t edgeCount  = network.edges().size();
	const size_t routeCount = network.routes().size();
	const bool sizeChanged = (nodeCount  != m_lastNodeCount)
	                      || (edgeCount  != m_lastEdgeCount)
	                      || (routeCount != m_lastRouteCount);
	const bool periodic    = (++m_framesSinceRebuild >= kRebuildIntervalFrames);

	if (m_anchorsDirty || sizeChanged || periodic)
	{
		m_anchorCache.clear();
		for (const auto& route : network.routes())
		{
			if (route.id < 0) continue;
			if (route.kind != RoadRouteKind::NationalRoute) continue;
			if (route.number <= 0) continue;

			for (const Vec3& anchor : routeAnchors(route, network))
			{
				m_anchorCache.push_back(CachedAnchor{
					Vec3{ anchor.x, anchor.y + kLabelOffsetY, anchor.z },
					route.id,
					route.number,
				});
			}
		}
		m_lastNodeCount      = nodeCount;
		m_lastEdgeCount      = edgeCount;
		m_lastRouteCount     = routeCount;
		m_framesSinceRebuild = 0;
		m_anchorsDirty       = false;
	}

	// --- 描画: カメラ依存のスクリーン投影・フェードは毎フレーム実行 ---
	for (const auto& item : m_anchorCache)
	{
		const Float3 worldPos{
			static_cast<float>(item.worldPos.x),
			static_cast<float>(item.worldPos.y),
			static_cast<float>(item.worldPos.z)
		};

		const Float3 sp = cam3D.worldToScreenPoint(worldPos);
		if (sp.z <= 0.0f) continue;

		const float dist = camera.horizontalDistanceTo(worldPos);
		if (dist > kHideDistance) continue;

		const double t     = Math::Clamp(static_cast<double>(dist - kFadeDistance) / (kHideDistance - kFadeDistance), 0.0, 1.0);
		const double alpha = 1.0 - t;
		const double size  = Math::Lerp(kSignSizeNear, kSignSizeFar, t);

		const Vec2 screenPos{ sp.x, sp.y };

		tex.resized(size).drawAt(screenPos, ColorF{ 1.0, alpha });

		m_hits << SignHit{ RectF{ Arg::center = screenPos, size, size }, alpha, item.routeId };

		const double numberSize = size * 0.361;  // 0.38 * 0.95
		const Vec2   numberPos{ screenPos.x, screenPos.y - size * 0.02 };
		fontNum(item.routeNumber).drawAt(numberSize, numberPos, ColorF{ 1.0, 1.0, 1.0, alpha });
	}
}

Optional<int> RoadRouteSignRenderer::hitTest(Vec2 p) const
{
	// ヒット判定は見た目の前後関係を優先するため、描画順の逆順で最初に当たった標識を返す。
	// 後から描画された（手前の）標識を優先するため逆順走査
	for (int i = static_cast<int>(m_hits.size()) - 1; i >= 0; --i)
	{
		const auto& hit = m_hits[i];
		if (hit.alpha >= kMinHitAlpha && hit.rect.contains(p))
		{
			return hit.routeId;
		}
	}
	return none;
}
