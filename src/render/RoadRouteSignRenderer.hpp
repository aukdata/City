#pragma once
#include "../road/RoadNetwork.hpp"
#include "../ui/Camera.hpp"

/// @brief 国道路線標識をビルボードとして 2D 投影表示するレンダラ
/// @details 各 NationalRoute について、構成エッジ列の中央付近にお握り型の国道標識
///          (assets/wip/national_road_sign.png) を描画し、中央に Arial で号数を重ねる。
class RoadRouteSignRenderer
{
public:
	// 国道路線標識は 3D 空間のアンカーから 2D ビルボードとして描き、選択用ヒット矩形も保持する。
	/// @brief 国道路線標識を描画する（Shader::LinearToScreen の後に呼ぶこと）
	void render(const RoadNetwork& network, const GameCamera& camera) const;

	/// @brief スクリーン座標 p にある標識の routeId を返す（alpha < kMinHitAlpha は無視）
	Optional<int> hitTest(Vec2 p) const;

	/// @brief ルート・エッジの形状が変化したことを外部から通知してアンカーを強制再計算させる
	void invalidate() const { m_anchorsDirty = true; }

private:
	/// @brief render() でキャッシュされる標識のスクリーン矩形・alpha・routeId
	struct SignHit
	{
		RectF rect;
		double alpha;
		int routeId;
	};

	/// @brief 形状に依存する固定データ（ネットワーク変更時のみ再計算）
	struct CachedAnchor
	{
		Vec3 worldPos;      ///< ラベルを置くワールド位置（高さオフセット込み）
		int  routeId;
		int  routeNumber;
	};

	mutable Array<SignHit>      m_hits;
	mutable Array<CachedAnchor> m_anchorCache;
	mutable size_t              m_lastNodeCount   = SIZE_MAX;
	mutable size_t              m_lastEdgeCount   = SIZE_MAX;
	mutable size_t              m_lastRouteCount  = SIZE_MAX;
	mutable int                 m_framesSinceRebuild = 0;
	mutable bool                m_anchorsDirty    = true;
};
