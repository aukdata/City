#pragma once
#include "MapGenerator.hpp"

/// @brief 国道骨格を保ち、近隣の町村へ地形費用に見合う双方向の補完道路を加える。
namespace VillageConnections
{
	struct Audit
	{
		int settlements = 0, accessNodes = 0;
		int compressedNodes = 0, components = 0, links = 0, cycles = 0;
		int nearbyPairs = 0, reachablePairs = 0, unreachablePairs = 0;
		double maximumDetour = 0, meanDetour = 0;
	};
	struct Result
	{
		int candidates = 0, trials = 0, detours = 0, connected = 0;
		int repairedComponents = 0, failed = 0, budgetDeferred = 0;
		Audit before, after;
	};
	/// @brief 生成中の供用予定道路を含め、車線方向・運用と実在する交差点遷移を守る距離探索。
	/// @details 未接続または距離上限超過は無限大。交差点内長は道路長との二重計上を避けて除外する。
	double shortestDistance(const RoadNetwork& roads, int start, int goal, double limit = Math::Inf);
	/// @brief 道路距離の集落ボロノイ領域へ縮約し、市街地内部の格子を数えず地域接続を測る。
	/// @details 離れた並列回廊を残し、近接する境界の街路束だけを同じ地域リンクへまとめる。
	/// @details nearbyPairs は候補上限内の近隣対。unreachablePairs には有限距離上限超過も含む。
	Audit measure(const Array<MapGenerator::Settlement>& settlements, const World& world, const RoadNetwork& roads);
	Result improve(
		uint64 seed, const Array<MapGenerator::Settlement>& settlements, const World& world, RoadNetwork& roads);
} // namespace VillageConnections
