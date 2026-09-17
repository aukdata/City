#pragma once
#include "RailwaySite.hpp"
#include "../world/World.hpp"
#include "../road/RoadNetwork.hpp"

namespace RailDepotBuilder
{
	/// @brief 終端駅の横の空地へ引込線・2本の留置線を接続する。失敗時は変更しない。
	bool add(TrainNetwork& network, const World& world, const RoadNetwork& roads, int station, String& error);
	void generate(TrainNetwork& network, const World& world, const RoadNetwork& roads);
}
