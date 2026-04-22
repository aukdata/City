#pragma once
#include "../world/World.hpp"
#include "../road/RoadNetwork.hpp"

/// @brief スタート/ゴール指定による自動敷設ユーティリティ
/// @details RoadPathfinder + MapGenerator::buildRoadSegment と同じアルゴリズムを使い、
///          ゲームプレイ中にユーザが指定した 2 点間の道路を Planned 状態で敷設する。
namespace RoadAutoPlace
{
	/// @brief 2 点間の経路探索を実行し、Planned エッジを道路ネットワークに追加する。
	/// @param roads          対象の RoadNetwork
	/// @param world          高さ計算に使用するワールド
	/// @param startWorld     スタートワールド座標
	/// @param goalWorld      ゴールワールド座標
	/// @param routeIds       新規エッジに紐付けるルート ID リスト（空可）
	/// @param templateEdge   生成エッジに適用する道路テンプレート（parts・lanes・roadType を反映）
	/// @return 追加されたエッジ ID 一覧。経路探索失敗時は空配列
	Array<int> buildPlanned(
		RoadNetwork& roads,
		const World& world,
		Vec3 startWorld, Vec3 goalWorld,
		const Array<int>& routeIds,
		const RoadEdge& templateEdge);
}
