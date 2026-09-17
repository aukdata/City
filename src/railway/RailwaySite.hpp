#pragma once
#include "TrainNetwork.hpp"
#include "../gen/ParcelGeometry.hpp"

/// @brief 駅・車庫の敷地と線路方向。描画と建物配置で同じ寸法を使う。
class World;
namespace RailwaySite
{
	inline constexpr double kPlatformLength = 90;
	inline constexpr double kStationMinimumRadius = 500;
	inline constexpr double kDepotApproachMinimumRadius = 150;
	inline constexpr double kSidingMinimumRadius = 300;
	/// @brief ホーム長の全区間を検査し、短い区間間の折れも見落とさない。
	double stationMinimumRadius(const TrainNetwork& network,int station);
	struct Frame
	{
		Vec3 origin, along, right;
		Vec3 point(double x, double y, double z) const { return origin + right*x + Vec3{0,y,0} + along*z; }
	};
	Array<Array<Vec3>> stationPaths(const TrainNetwork& network, int station);
	Optional<Frame> stationFrame(const TrainNetwork& network, int station);
	Optional<Frame> depotFrame(const TrainNetwork& network, const RailDepot& depot);
	ParcelGeometry::Quad rectangle(const Frame& frame, double left, double right, double start, double end);
	Array<ParcelGeometry::Quad> footprints(const TrainNetwork& network);
	/// @brief 地上・高架線路と施設の除草範囲。深いトンネル上の森林は残す。
	Array<ParcelGeometry::Quad> landscapeFootprints(const TrainNetwork& network,const World& world);
}
