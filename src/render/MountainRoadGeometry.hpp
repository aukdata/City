#pragma once
#include "../road/RoadNetwork.hpp"
#include "../world/World.hpp"
#include "../world/HeightSnapshot.hpp"

/// @brief 山側の擁壁と谷側の防護柵。通行空間を空け、道路と現地盤をつなぐ。
namespace MountainRoadGeometry
{
	struct Geometry { MeshData wall, moss, rail, posts, reflectors; };
	[[nodiscard]] Geometry build(const RoadEdge& edge,const CubicBezier& curve,const World& world,float start,float end);
	/// @brief CPU worker overload sharing the same geometry algorithm.
	[[nodiscard]] Geometry build(const RoadEdge& edge,const CubicBezier& curve,const HeightSnapshot& world,float start,float end);
}
