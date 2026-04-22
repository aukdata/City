#pragma once
#include "MapGenerator.hpp"

class World;
class RoadNetwork;

/// @brief 集落タイプ別の地区内道路生成
/// @details 城下町（格子＋街道クランク＋城）／宿場町（はしご状）／農村（櫛状）の
///     3 類型に分けて、初期マップ生成時の生活道路を敷設する。
///     すべて MapGenerator::generateGlobalRoads() で幹線が敷設された後に呼ばれる。
namespace DistrictRoads
{
	/// @brief 集落を通る街道（格子・裏通り・支線の基軸となる既存道路）
	/// @details 幹線生成済みの RoadNetwork から、集落中心付近を通る
	///     Arterial/LocalRoad の連続エッジ列として抽出する。
	struct KaidoSegment
	{
		Array<int> edgeIds;              ///< 連続エッジ列（先頭ノード → 末尾ノードの順）
		Array<int> nodeIds;              ///< edgeIds に対応するノード列（edgeIds.size()+1 個）
		Vec2       dirAtCenter{ 1, 0 }; ///< 集落中心付近での街道方向（XZ 単位ベクトル）
		int        centerNodeId = -1;    ///< 集落中心に最も近い街道上のノード ID
		bool       passesThrough = false;///< 集落内を通る街道が見つかったか
	};

	/// @brief 集落中心付近の街道エッジ列を抽出する
	/// @param searchRadius 集落中心から街道上のノードを探す最大半径 [m]
	/// @return 貫通する街道が無ければ passesThrough=false で空のセグメントを返す
	KaidoSegment extractKaido(
		const MapGenerator::Settlement& settlement,
		const RoadNetwork& network,
		float searchRadius);

	/// @brief 城下町（CastleTown）の道路を生成する
	/// @details 街道クランク＋町人地／武家地の二層格子＋城広場＋辺間引き
	void generateCastleTown(
		uint64 seed, int settlementIndex,
		const MapGenerator::Settlement& settlement,
		const KaidoSegment& kaido,
		const World& world,
		RoadNetwork& network);

	/// @brief 宿場町（PostTown）の道路を生成する（はしご状）
	void generatePostTown(
		uint64 seed, int settlementIndex,
		const MapGenerator::Settlement& settlement,
		const KaidoSegment& kaido,
		const World& world,
		RoadNetwork& network);

	/// @brief 農村（Village）の道路を生成する（櫛状・短い支線）
	void generateVillage(
		uint64 seed, int settlementIndex,
		const MapGenerator::Settlement& settlement,
		const KaidoSegment& kaido,
		const World& world,
		RoadNetwork& network);
}
