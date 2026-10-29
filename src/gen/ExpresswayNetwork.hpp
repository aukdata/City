#pragma once
#include "MapGenerator.hpp"
#include "../road/RoadNetwork.hpp"
#include "../world/World.hpp"

/// @brief 自動車専用道路（上下線分離の本線・IC・JCT・料金所）を一般道の整備後に生成する。
/// @details 本線は地区を避けた地形回廊を探索し、鉄道と同じ縦断費用で一般道・水面を立体交差する。
///   IC はダイヤモンド型（ランプ 4 本＋料金所＋24m 取付道路）、JCT は Y 型の片方向連絡とする。
///   断面・勾配・料金所・標識の数値は CSL（Cities: Skylines 用の自作 MOD 群）から移植した設定値を使う。
namespace ExpresswayNetwork
{
	/// @brief 生成結果の集計。ログと検証に使う。
	struct Summary
	{
		int lines = 0;
		int interchanges = 0;
		int junctions = 0;
		int services = 0;
		int mainlineEdges = 0;
		int rampEdges = 0;
		int accessEdges = 0;
		int guideSigns = 0;
		int tollGates = 0;
		double lengthKm = 0;
	};

	/// @brief 既存の一般道を保ったまま高速道路を追加する。道路網・地形（土工）を更新する。
	Summary generate(uint64 seed, World& world, RoadNetwork& roads, const Array<MapGenerator::Settlement>& settlements);

	/// @brief 路線名の表示用短縮（CSL 日本道路テーマ: 「自動車道」→「道」、「高速」を省略）。
	String displayName(StringView name);

	/// @brief 地名から路線名の基部を作る（末尾の「市」を外す）。
	String routeBaseName(StringView place);
}
