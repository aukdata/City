#include "GameScene.hpp"
#include "../road/RoadConstructionStart.hpp"
#include "../gen/RoadTerrainFit.hpp"

bool GameScene::startRoadConstruction(const Array<int>& edgeIds)
{
	const auto receipt = RoadConstructionStart::start(m_network, edgeIds, m_economy.funds, m_clock.now, m_sandboxActive);
	if (!receipt) { m_soundEffects.play(SoundEffects::Cue::Reject);return false; }
	applyConstructionStart(receipt->cost, receipt->edgeIds, receipt->affectedNodeIds);
	return true;
}

void GameScene::applyConstructionStart(double cost, const Array<int>& edgeIds, const Array<int>& affectedNodeIds)
{
	// 隣接する工事の切盛土が別計画へ影響するため、従来の計画単位と選択順で用地を準備する。
	HashSet<int> preparedPlans;
	for (const int edgeId : edgeIds)
	{
		const RoadEdge* edge = m_network.getEdge(edgeId);
		if (!edge) { continue; }
		if (const RoadPlan* plan = m_network.getPlan(edge->planId))
		{
			if (preparedPlans.insert(plan->id).second) { prepareConstructionSite(plan->edgeIds); }
		}
		else
		{
			prepareConstructionSite({edgeId});
		}
	}
	if (!m_sandboxActive) { m_economy.funds = Max(0.0, m_economy.funds - cost); }
	m_soundEffects.play(SoundEffects::Cue::Construction);
	for (const int nodeId : affectedNodeIds) { m_roadRenderer.invalidateCachesAroundNode(nodeId, m_network); }
	notifyNetworkChanged(affectedNodeIds);
}

void GameScene::prepareConstructionSite(const Array<int>& edgeIds)
{
	if (edgeIds.isEmpty()) return;
	Stopwatch timer{StartImmediately::Yes};
	for (const int id : edgeIds)
	{
		if (const auto* edge = m_network.getEdge(id); edge && edge->useElevation)
			m_network.generatePiersForEdge(id, m_world);
	}
	const auto affected = RoadConstruction::affectedCells(m_network, edgeIds, m_world,
		[this](const Chunk& chunk, int col, int row) -> Optional<RoadConstruction::Bounds>
		{
			const auto box = m_worldRenderer.buildingHitBox(chunk,m_world,col,row);
			if (!box) return none;
			return RoadConstruction::Bounds{box->center,box->size,chunk.buildingGrid[{col,row}].angle};
		});
	const int removed = m_clearanceLedger.clear(m_world,affected);
	RoadTerrainFit::apply(m_network,m_world,HashSet<int>{edgeIds.begin(),edgeIds.end()});
	m_worldRenderer.invalidateTerrainForEdges(m_network,edgeIds);
	for (const int id : edgeIds) m_roadRenderer.invalidateEdgeCache(id);
	DebugLog::print(U"[Construction] 既設物撤去工: edges={} buildings={} reservedCells={} ms={:.3f}"_fmt(
		edgeIds.size(),removed,affected.size(),timer.msF()));
}

void GameScene::tickConstruction()
{
	// 工事中の道路計画を監視し、工期満了時に所属エッジを同時開通する。
	Array<int> completedPlanIds;
	for (const auto& plan : m_network.plans())
	{
		if (plan.id < 0) continue;
		if (plan.state != PlanState::UnderConstruction) continue;
		if (!plan.completionDate || m_clock.now < *plan.completionDate) continue;
		completedPlanIds << plan.id;
	}
	Array<int> dirtyNodes;
	// Legacy single edges also progress and open; they previously remained under construction forever.
	for (const auto& edge : m_network.edges())
	{
		if (edge.id < 0 || edge.planId >= 0 || edge.edgeState != EdgeState::UnderConstruction) continue;
		if (RoadConstruction::progress(m_network,edge,m_clock.now).stage == RoadConstruction::Stage::Complete)
		{
			m_network.getEdge(edge.id)->edgeState = EdgeState::Open;
			dirtyNodes << edge.nodeA << edge.nodeB;
		}
	}
	for (const int planId : completedPlanIds)
	{
		const RoadPlan* plan = m_network.getPlan(planId);
		if (!plan) continue;
		for (const int eid : plan->edgeIds)
		{
			if (const RoadEdge* edge = m_network.getEdge(eid))
				dirtyNodes << edge->nodeA << edge->nodeB;
		}
		m_network.completePlanConstruction(planId);
	}
	if (!dirtyNodes.isEmpty())
	{
		m_soundEffects.play(SoundEffects::Cue::Complete);
		DBG_LOG(U"[Construction] opened plans={} nodes={}"_fmt(completedPlanIds.size(),dirtyNodes.size()));
		for (const int nid : dirtyNodes)
			m_roadRenderer.invalidateCachesAroundNode(nid, m_network);
		notifyNetworkChanged(dirtyNodes);
	}
}

