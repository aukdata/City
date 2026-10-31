#pragma once
#include "../road/RoadNetwork.hpp"
#include "../world/World.hpp"
#include "../world/HeightSnapshot.hpp"

/// @brief 山側の擁壁と谷側の防護柵。通行空間を空け、道路と現地盤をつなぐ。
/// @details 高い切土の上は、国道41号の飛騨川沿いのような渓谷の道に多い吹付法面と格子状の法枠で覆う。
namespace MountainRoadGeometry
{
	struct Geometry { MeshData wall, moss, rail, posts, reflectors, shotcrete, frame; };
	[[nodiscard]] Geometry build(const RoadEdge& edge,const CubicBezier& curve,const World& world,float start,float end);
	/// @brief CPU worker overload sharing the same geometry algorithm.
	[[nodiscard]] Geometry build(const RoadEdge& edge,const CubicBezier& curve,const HeightSnapshot& world,float start,float end);
}
