#pragma once
#include "MapGenerator.hpp"

/// @brief 旧町割の秩序を保ち、外側だけを街道沿いの住宅地・小街区へ接続する。
namespace SettlementFringe
{
	void generate(MapGenerator::Settlement& settlement, const World& world, RoadNetwork& network);
}
