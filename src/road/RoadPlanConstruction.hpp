#pragma once
#include <variant>
#include "RoadPlanDraft.hpp"

/// @brief 道路候補の確定。画面・描画・用地撤去から独立した道路網のトランザクション。
namespace RoadPlanConstruction
{
	struct Request
	{
		String planName;
		String routeName;
		Optional<int> existingRouteId;
		bool sandbox = false; ///< 資金制限なしで着工できる。
	};

	enum class Error : uint8
	{
		InvalidDraft,
		MissingRoute,
		ConnectionFailed,
		InsufficientFunds,
	};

	/// @brief 確定した工事と、その反映に必要な差分。
	struct Receipt
	{
		int planId;
		double cost;
		Array<int> edgeIds;
		Array<int> removedEdgeIds;
		Array<int> affectedNodeIds;
	};
	using Result = std::variant<Receipt, Error>;

	/// @brief 接続・資金・路線登録・着工が成功した場合だけ道路網を置き換える。
	/// @details 資金の支払い、用地撤去、描画の更新は成功後に呼び出し側が行う。
	[[nodiscard]] Result commit(RoadNetwork& network, const World& world, const RoadPlanDraft& draft,
		const RoadEdge& roadTemplate, const Request& request, double funds, GameTime now);
}
