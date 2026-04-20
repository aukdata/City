#pragma once
#include "../road/RoadNetwork.hpp"
#include "../ui/Camera.hpp"

/// @brief 国道路線標識をビルボードとして 2D 投影表示するレンダラ
/// @details 各 NationalRoute について、構成エッジ列の中央付近にお握り型の国道標識
///          (assets/wip/national_road_sign.png) を描画し、中央に Arial で号数を重ねる。
class RoadRouteSignRenderer
{
public:
	/// @brief 国道路線標識を描画する（Shader::LinearToScreen の後に呼ぶこと）
	void render(const RoadNetwork& network, const GameCamera& camera) const;

	/// @brief スクリーン座標 p にある標識の routeId を返す（alpha < kMinHitAlpha は無視）
	Optional<int> hitTest(Vec2 p) const;

private:
	/// @brief render() でキャッシュされる標識のスクリーン矩形・alpha・routeId
	struct SignHit
	{
		RectF rect;
		double alpha;
		int routeId;
	};

	mutable Array<SignHit> m_hits;
};
