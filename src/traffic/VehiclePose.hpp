#pragma once
#include "Vehicle.hpp"
#include "../road/RoadNetwork.hpp"
class World;

/// @brief 車線・交差点内の車両座標を描画と接触判定で共用する。
/// @brief Edge curves kept between frames. An entry is reused only while its end nodes and control
/// points are unchanged, so it always equals a fresh RoadNetwork::getBezier().
class EdgeCurveCache
{
public:
	const CubicBezier* get(const RoadNetwork& roads,int edgeId);
	void trim(size_t edgeCount) { if (m_curves.size()>edgeCount*2+64) { m_curves.clear(); } }
private:
	HashTable<int,CubicBezier> m_curves;
};

namespace VehiclePose
{
	Optional<Vehicle> resolve(const Vehicle& vehicle,const RoadNetwork& roads,const World* world=nullptr);
	/// @brief Same as resolve(), reusing the vehicle's edge curve from the cache.
	Optional<Vehicle> resolve(const Vehicle& vehicle,const RoadNetwork& roads,const World* world,EdgeCurveCache& curves);
}
