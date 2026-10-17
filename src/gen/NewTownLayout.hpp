#pragma once
#include "MapGenerator.hpp"

/// @brief 計画住宅地の完成した街路を、車道と緑道へ分ける。幹線接続の処理後に実行する。
namespace NewTownLayout
{
/// @param world 指定時は、歩行者網から孤立した緑道の断片を撤去する。
void finish(MapGenerator::Settlement& settlement, RoadNetwork& network, const World* world = nullptr);
}
