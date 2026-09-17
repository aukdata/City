#pragma once
#include "../railway/RailwaySite.hpp"
#include "../world/World.hpp"

/// @brief 日本の小駅・高架駅と開口付き検修庫を、実寸・閉じたメッシュで構築する。
namespace RailFacilities
{
	enum Material : size_t { Concrete, Roof, Steel, Glass, Tactile, Timber, Light, Ballast, Count };
	struct Sign { Vec3 center, along; String text; double width = 3.2; };
	struct Geometry { std::array<MeshData,Count> parts; Array<Sign> signs; };
	Geometry station(const TrainNetwork& network, const World& world, int stationId);
	Geometry depot(const TrainNetwork& network, const World& world, const RailDepot& depot);
	Array<Train> parkedTrains(const TrainNetwork& network);
	ColorF color(size_t material);
}
