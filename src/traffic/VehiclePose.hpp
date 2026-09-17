#pragma once
#include "Vehicle.hpp"
#include "../road/RoadNetwork.hpp"
class World;

/// @brief 車線・交差点内の車両座標を描画と接触判定で共用する。
namespace VehiclePose
{
	Optional<Vehicle> resolve(const Vehicle& vehicle,const RoadNetwork& roads,const World* world=nullptr);
}
