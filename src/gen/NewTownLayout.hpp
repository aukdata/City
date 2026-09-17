#pragma once
#include "MapGenerator.hpp"

/// @brief 計画住宅地の完成した街路を、車道と緑道へ分ける。幹線接続の処理後に実行する。
namespace NewTownLayout
{
void finish(MapGenerator::Settlement& settlement, RoadNetwork& network);
}
