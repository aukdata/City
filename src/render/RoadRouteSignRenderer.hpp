#pragma once
#include "../road/RoadNetwork.hpp"
#include "../ui/Camera.hpp"

/// @brief 現在地の国道を画面上部に表示する。クリックで路線情報を開く。
class RoadRouteSignRenderer
{
public:
	void render(const RoadNetwork& network,const GameCamera& camera) const;
	Optional<int> hitTest(Vec2 point) const;
	void invalidate() const { m_route.reset(); }
private:
	mutable Optional<int> m_route;
	mutable RectF m_bounds;
};
