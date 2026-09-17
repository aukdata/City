#include "RoadConstructionStart.hpp"

namespace
{
	struct Order
	{
		RoadConstructionStart::Receipt receipt;
		Array<int> planIds;
		Array<int> standaloneEdgeIds;
	};

	Order makeOrder(const RoadNetwork& network, const Array<int>& selectedEdgeIds)
	{
		Order order;
		HashSet<int> seenEdges, seenNodes, seenPlans;
		const auto appendEdge = [&](const RoadEdge& edge)
		{
			if (!seenEdges.insert(edge.id).second) { return; }
			order.receipt.edgeIds << edge.id;
			for (const int nodeId : { edge.nodeA, edge.nodeB })
			{
				if (seenNodes.insert(nodeId).second) { order.receipt.affectedNodeIds << nodeId; }
			}
		};
		for (const int edgeId : selectedEdgeIds)
		{
			const RoadEdge* edge = network.getEdge(edgeId);
			if (!edge || edge->edgeState != EdgeState::Planned || seenEdges.contains(edgeId)) { continue; }
			if (edge->planId >= 0)
			{
				const RoadPlan* plan = network.getPlan(edge->planId);
				if (!plan || plan->state != PlanState::Planning || !seenPlans.insert(plan->id).second) { continue; }
				bool hasEdges = false;
				for (const int memberId : plan->edgeIds)
				{
					if (const RoadEdge* member = network.getEdge(memberId))
					{
						appendEdge(*member);
						hasEdges = true;
					}
				}
				if (hasEdges)
				{
					order.planIds << plan->id;
					order.receipt.cost += static_cast<double>(plan->totalCost);
				}
			}
			else
			{
				appendEdge(*edge);
				order.standaloneEdgeIds << edgeId;
				// 単独エッジの従来料金を維持する。計画の構造物係数は rebuildPlanStats が計算する。
				order.receipt.cost += network.estimatePlanCost(edge->roadType, edge->length);
			}
		}
		return order;
	}
}

bool RoadConstructionStart::canAfford(double funds, double cost)
{
	constexpr double kCostEpsilon = 1e-6;
	return funds + kCostEpsilon >= cost;
}

double RoadConstructionStart::estimateCost(const RoadNetwork& network, const Array<int>& selectedEdgeIds)
{
	return makeOrder(network, selectedEdgeIds).receipt.cost;
}

Optional<RoadConstructionStart::Receipt> RoadConstructionStart::start(RoadNetwork& network,
	const Array<int>& selectedEdgeIds, double funds, GameTime now, bool sandbox)
{
	// 同じ選別処理を実行直前に再評価する。以降は外部コールバックを挟まず、確認済みの状態だけを変更する。
	Order order = makeOrder(network, selectedEdgeIds);
	if (order.receipt.edgeIds.isEmpty() || (!sandbox && !canAfford(funds, order.receipt.cost))) { return none; }
	for (const int planId : order.planIds) { network.startPlanConstruction(planId, now); }
	for (const int edgeId : order.standaloneEdgeIds)
	{
		RoadEdge* edge = network.getEdge(edgeId);
		edge->edgeState = EdgeState::UnderConstruction;
		edge->constructionStartTime = now;
	}
	return std::move(order.receipt);
}
