#pragma once
#include "MapGenerator.hpp"
#include "../road/RoadNetwork.hpp"
#include "../world/World.hpp"

/// @brief 送電線（鉄塔と架空線）。地域の中心都市・町の変電所どうしと、地図外の発電所からの引き込みを結ぶ。
/// @details 経路は粗い格子で市街地・水面・急斜面を避けて探索し、折れ点を角度鉄塔、直線部を径間ごとの懸垂鉄塔にする。
///   鉄塔は道路・建物・水面に掛からないよう径間の方向へずらす。数値の正本は assets/generation/powerLines.json。
///   生成済みの地形・集落・道路・建物から決定的に求めるため、セーブには含めずロード時にも同じ結果を作り直す。
namespace PowerLines
{
	/// @brief 鉄塔 1 基。base は脚の中心の地表。
	struct Tower
	{
		Vec3 base;
		Vec2 along{ 1, 0 }; ///< 腕と直交する線路方向（角度鉄塔は前後径間の二等分）
		bool angle = false; ///< 折れ点・端の耐張鉄塔
	};

	/// @brief 1 路線。towers は端から端への順。
	struct Line
	{
		Array<Tower> towers;
		float height = 48;  ///< 鉄塔の高さ [m]
	};

	/// @brief 生成済みの街から送電線を求める。
	Array<Line> generate(uint64 seed, const World& world, const RoadNetwork& roads, const Array<MapGenerator::Settlement>& settlements);
}
