#pragma once
#include "MapGenerator.hpp"

/// @brief 村間の道路距離を測り、遠回りの生活道路を補う。
namespace VillageConnections
{
	struct Result
	{
		int trials = 0, detours = 0, connected = 0;
	};
	/// @brief 車線の方向を守る距離優先探索。未接続または上限超過は無限大。
	double shortestDistance(const RoadNetwork& roads, int start, int goal, double limit = Math::Inf);
	Result improve(
		uint64 seed, const Array<MapGenerator::Settlement>& settlements, const World& world, RoadNetwork& roads);
} // namespace VillageConnections
