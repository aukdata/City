#pragma once
#include "RoadNetwork.hpp"

/// @brief 既存の未着工道路を、所属計画単位にまとめて見積もり・着工する。
namespace RoadConstructionStart
{
	struct Receipt
	{
		double cost = 0.0;
		Array<int> edgeIds;
		Array<int> affectedNodeIds;
	};

	[[nodiscard]] bool canAfford(double funds, double cost);
	[[nodiscard]] double estimateCost(const RoadNetwork& network, const Array<int>& selectedEdgeIds);
	/// @brief 資金不足や対象なしでは何も変更しない。支払いと用地・描画更新は成功後に呼び出し側で行う。
	[[nodiscard]] Optional<Receipt> start(RoadNetwork& network, const Array<int>& selectedEdgeIds,
		double funds, GameTime now, bool sandbox = false);
}
