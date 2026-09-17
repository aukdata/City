#include "RoadNetwork.hpp"

namespace
{
	constexpr double kMinimumConstructionSeconds = 5.0;

	/// @brief 道路種別ごとの概算建設費 [億円/km]
	double constructionCostPerKm(RoadType roadType)
	{
		switch (roadType)
		{
		case RoadType::LocalRoad:  return 0.5;   // 生活道路
		case RoadType::Arterial:   return 1.5;   // 県道級の幹線
		case RoadType::Expressway: return 8.0;   // バイパス・自動車専用道級
		case RoadType::Highway:    return 20.0;  // 高速道路級
		}
		return 0.5;
	}

	/// @brief 道路種別ごとの建設期間 [標準速度での秒/km]。昼夜の長さと独立させる。
	double constructionSecondsPerKm(RoadType roadType)
	{
		switch (roadType)
		{
		case RoadType::LocalRoad:  return 60.0;
		case RoadType::Arterial:   return 120.0;
		case RoadType::Expressway: return 360.0;
		case RoadType::Highway:    return 720.0;
		}
		return 60.0;
	}

}

int RoadNetwork::addPlan(RoadPlan plan)
{
	plan.id = m_nextPlanId++;
	if (plan.routeId >= 0)
	{
		if (const RoadRoute* route = getRoute(plan.routeId))
		{
			plan.routeName = route->name;
		}
	}

	int idx;
	if (!m_freePlanSlots.isEmpty())
	{
		idx = m_freePlanSlots.back();
		m_freePlanSlots.pop_back();
		m_plans[idx] = std::move(plan);
	}
	else
	{
		idx = static_cast<int>(m_plans.size());
		m_plans << std::move(plan);
	}
	m_planIdToIdx[m_plans[idx].id] = idx;
	rebuildPlanStats(m_plans[idx].id);
	rebuildPlanEdgeLinks();
	return m_plans[idx].id;
}

void RoadNetwork::addPlanRaw(const RoadPlan& plan)
{
	if (plan.id < 0 || m_planIdToIdx.contains(plan.id)) return;

	int idx;
	if (!m_freePlanSlots.isEmpty())
	{
		idx = m_freePlanSlots.back();
		m_freePlanSlots.pop_back();
		m_plans[idx] = plan;
	}
	else
	{
		idx = static_cast<int>(m_plans.size());
		m_plans << plan;
	}
	m_planIdToIdx[plan.id] = idx;
	m_nextPlanId = Max(m_nextPlanId, plan.id + 1);
}

void RoadNetwork::removePlan(int planId)
{
	const int idx = planIndex(planId);
	if (idx < 0) return;
	for (const int eid : m_plans[idx].edgeIds)
	{
		if (RoadEdge* edge = getEdge(eid))
		{
			if (edge->planId == planId)
				edge->planId = -1;
		}
	}
	m_plans[idx].id = -1;
	m_plans[idx].edgeIds.clear();
	m_plans[idx].viaPoints.clear();
	m_planIdToIdx.erase(planId);
	m_freePlanSlots << idx;
}

RoadPlan* RoadNetwork::getPlan(int id)
{
	const int idx = planIndex(id);
	return (idx >= 0) ? &m_plans[idx] : nullptr;
}

const RoadPlan* RoadNetwork::getPlan(int id) const
{
	const int idx = planIndex(id);
	return (idx >= 0) ? &m_plans[idx] : nullptr;
}

void RoadNetwork::rebuildPlanEdgeLinks()
{
	for (auto& edge : m_edges)
	{
		if (edge.id >= 0 && edge.planId >= 0 && !getPlan(edge.planId))
			edge.planId = -1;
	}

	for (auto& plan : m_plans)
	{
		if (plan.id < 0) continue;
		Array<int> valid;
		for (const int eid : plan.edgeIds)
		{
			if (RoadEdge* edge = getEdge(eid))
			{
				edge->planId = plan.id;
				valid << eid;
			}
		}
		plan.edgeIds = std::move(valid);
	}
}

void RoadNetwork::rebuildPlanStats(int planId)
{
	RoadPlan* plan = getPlan(planId);
	if (!plan) return;

	double totalMeters = 0.0,weightedMeters=0;
	for (const int eid : plan->edgeIds)
	{
		if (const RoadEdge* edge = getEdge(eid))
			{ totalMeters += edge->length; weightedMeters += edge->constructionEquivalentLength(); }
	}
	plan->totalLength = static_cast<float>(totalMeters);
	plan->totalCost = static_cast<float>(estimatePlanCost(plan->roadType, weightedMeters));
	plan->constructionDuration = estimatePlanConstructionDuration(plan->roadType, weightedMeters);
	if (plan->routeId >= 0)
	{
		if (const RoadRoute* route = getRoute(plan->routeId))
			plan->routeName = route->name;
	}
}

void RoadNetwork::rebuildAllPlanStats()
{
	for (const auto& plan : m_plans)
	{
		if (plan.id >= 0)
			rebuildPlanStats(plan.id);
	}
}

bool RoadNetwork::startPlanConstruction(int planId, GameTime startTime)
{
	RoadPlan* plan = getPlan(planId);
	if (!plan || plan->state != PlanState::Planning || plan->edgeIds.isEmpty()) return false;

	bool changed = false;
	for (const int eid : plan->edgeIds)
	{
		RoadEdge* edge = getEdge(eid);
		if (!edge) continue;
		edge->planId = planId;
		edge->edgeState = EdgeState::UnderConstruction;
		edge->constructionStartTime = startTime;
		changed = true;
	}
	if (!changed) return false;

	plan->state = PlanState::UnderConstruction;
	plan->constructionStart = startTime;
	plan->completionDate = startTime + plan->constructionDuration;
	return true;
}

bool RoadNetwork::completePlanConstruction(int planId)
{
	RoadPlan* plan = getPlan(planId);
	if (!plan) return false;

	bool changed = false;
	for (const int eid : plan->edgeIds)
	{
		if (RoadEdge* edge = getEdge(eid))
		{
			edge->edgeState = EdgeState::Open;
			changed = true;
		}
	}
	if (!changed) return false;

	plan->state = PlanState::Complete;
	if (plan->constructionStart)
		plan->completionDate = *plan->constructionStart + plan->constructionDuration;
	return true;
}

double RoadNetwork::estimatePlanConstructionDuration(RoadType roadType, double totalLengthMeters) const
{
	const double lengthKm = totalLengthMeters / 1000.0;
	const double duration = lengthKm * constructionSecondsPerKm(roadType);
	return Max(kMinimumConstructionSeconds, duration);
}

double RoadNetwork::estimatePlanCost(RoadType roadType, double totalLengthMeters) const
{
	const double lengthKm = totalLengthMeters / 1000.0;
	return lengthKm * constructionCostPerKm(roadType);
}

void RoadNetwork::onEdgeRemovedFromPlans(int edgeId)
{
	Array<int> emptyPlans;
	for (auto& plan : m_plans)
	{
		if (plan.id < 0) continue;
		const bool removed = plan.edgeIds.contains(edgeId);
		if (removed)
			plan.edgeIds.remove(edgeId);
		if (!removed) continue;
		rebuildPlanStats(plan.id);
		if (plan.edgeIds.isEmpty())
			emptyPlans << plan.id;
	}

	for (const int planId : emptyPlans)
		removePlan(planId);
}
