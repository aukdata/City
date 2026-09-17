#pragma once
#include "../railway/RailwaySite.hpp"
#include "../road/RoadNetwork.hpp"

/// @brief 地面切削とは独立した、鉄道施設と高架道路の植生除外範囲。
namespace TransportLandscape
{
	Array<ParcelGeometry::Quad> footprints(const TrainNetwork& railway,const RoadNetwork& roads,const World& world);
}
