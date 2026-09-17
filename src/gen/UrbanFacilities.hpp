#pragma once
#include "MapGenerator.hpp"

/// @brief 都市の規模と拠点に応じて公共・大規模商業敷地を住宅より先に確保する。
namespace UrbanFacilities
{
	struct Result
	{
		int placed = 0;
		int rejected = 0;
	};
	Result generate(World& world, const RoadNetwork& roads, const TrainNetwork& trains,
		const Array<MapGenerator::Settlement>& districts, uint64 seed);
	/// @brief 地下駅は専用地下複線と既定ダイヤを持ち、地上駅とは徒歩で連絡する。
	void generateSubways(World& world, TrainNetwork& trains, const Array<MapGenerator::Settlement>& districts);
} // namespace UrbanFacilities
