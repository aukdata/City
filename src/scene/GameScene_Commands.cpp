#include "GameScene.hpp"
#include "../debug/CommandExecution.hpp"

void GameScene::executeCommand(StringView input)
{
	const auto parsed=GameCommands::parse(input);
	if(!parsed.command) { m_commandPalette.report(parsed.message,true);return; }
	if(parsed.command->kind==GameCommands::Kind::CameraGoto || parsed.command->kind==GameCommands::Kind::CameraZoom)
	{
		if (m_driving.active())
		{
			leaveDriving(true);
		}
		m_trackingVehicle = false;
		m_trackingTrain = false;
	}
	const auto result=CommandExecution::execute(*parsed.command,{m_clock,m_network,m_world,m_camera,m_economy.funds,m_frameRateGraph.visible});
	m_commandPalette.report(result.message,!result.success);
	if(!result.success) { return; }
	if(!result.changedEdges.isEmpty())
	{
		Array<int> nodes;
		for(int id:result.changedEdges)
		{
			const auto* edge=m_network.getEdge(id);if(!edge) { continue; }
			if(edge->isRoadbedBuilt() || edge->edgeState==EdgeState::UnderConstruction) { prepareConstructionSite({id}); }
			nodes << edge->nodeA << edge->nodeB;
			m_worldRenderer.invalidateTerrainForEdge(id);m_roadRenderer.invalidateEdgeCache(id);
		}
		notifyNetworkChanged(nodes);m_roadPlanSnapIndex.rebuild(m_network);
	}
	m_hudStatsRefreshCountdown=0;
	DBG_LOG(U"[Command] {}: {}"_fmt(input,result.message));
}
