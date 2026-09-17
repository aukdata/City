#include "RoadPlanConstruction.hpp"

namespace RoadPlanConstruction
{
	Result commit(RoadNetwork& network, const World& world, const RoadPlanDraft& draft,
		const RoadEdge& roadTemplate, const Request& request, double funds, GameTime now)
	{
		if (!draft.generated() || !draft.valid()) { return Error::InvalidDraft; }
		if (request.existingRouteId && !network.getRoute(*request.existingRouteId))
		{
			return Error::MissingRoute;
		}

		auto proposal = draft.propose(network, world, roadTemplate);
		if (!proposal) { return Error::ConnectionFailed; }
		auto& proposed = proposal->network;
		const auto& edgeIds = proposal->edgeIds;
		double weightedLength = 0.0;
		for (const int id : edgeIds)
		{
			weightedLength += proposed.getEdge(id)->constructionEquivalentLength();
		}
		const double cost = proposed.estimatePlanCost(roadTemplate.roadType, weightedLength);
		if (!request.sandbox && cost > funds) { return Error::InsufficientFunds; }

		Array<int> removedEdgeIds;
		for (const auto& edge : network.edges())
		{
			if (edge.id >= 0 && !proposed.getEdge(edge.id)) { removedEdgeIds << edge.id; }
		}
		Array<int> affectedNodeIds;
		for (const auto& edge : proposed.edges())
		{
			if (edge.id < network.nextEdgeId()) { continue; }
			proposed.updateEdgeElevation(edge.id, world);
			affectedNodeIds << edge.nodeA << edge.nodeB;
		}

		const int routeId = request.existingRouteId
			? *request.existingRouteId
			: proposed.addRoute(RoadRouteKind::Named, request.routeName, edgeIds, 0);
		if (request.existingRouteId) { proposed.getRoute(routeId)->edgeIds.append(edgeIds); }
		proposed.rebuildEdgeRouteIndex();

		RoadPlan plan;
		plan.name = request.planName.isEmpty() ? U"道路計画 {}"_fmt(proposed.plans().size() + 1) : request.planName;
		plan.routeId = routeId;
		plan.routeName = proposed.getRoute(routeId)->name;
		plan.roadType = roadTemplate.roadType;
		plan.edgeIds = edgeIds;
		const auto& points = draft.points();
		for (size_t index = 1; index + 1 < points.size(); ++index) { plan.viaPoints << points[index]; }
		plan.originName = U"始点";
		plan.destName = U"終点";
		plan.state = PlanState::Planning;
		const int planId = proposed.addPlan(std::move(plan));
		if (!proposed.startPlanConstruction(planId, now)) { return Error::ConnectionFailed; }

		Receipt receipt{ planId, cost, std::move(proposal->edgeIds),
			std::move(removedEdgeIds), std::move(affectedNodeIds) };
		network = std::move(proposed);
		return receipt;
	}
}
