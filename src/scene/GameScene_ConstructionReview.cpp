#include "GameScene.hpp"
#include "../ui/ConstructionStatus.hpp"

/// @brief Deterministic production-scene check, isolated by --capture-construction.
void GameScene::updateConstructionReview()
{
	m_clock.speed=TimeSpeed::Paused;m_clock.hour=13;
	m_camera.setBlockInput(true);
	static Array<int> planIds;
	static Vec3 center;
	static int oldBuildings=0;
	static size_t beforeClearance=0;
	static GameTime started=0;
	if(m_captureFrame==0)
	{
		const Vec3 focus=captureStreetCornerPoint(captureFocusPoint());
		double distance=Math::Inf;Vec3 along{1,0,0};
		for(int y=0;y<WORLD_CHUNKS;++y) for(int x=0;x<WORLD_CHUNKS;++x)
		{
			const auto* chunk=m_world.getChunk({x,y});if(!chunk) continue;
			if(Vec2{x*CHUNK_SIZE+512.0,y*CHUNK_SIZE+512.0}.distanceFrom({focus.x,focus.z})>1800) continue;
			for(int row=0;row<ZONE_CELLS;++row) for(int col=0;col<ZONE_CELLS;++col)
			{
				const auto& building=chunk->buildingGrid[{col,row}];
				if(!isResidentialBuildingType(building.type)) continue;
				Vec3 p{x*CHUNK_SIZE+(col+.5)*16+building.offsetX,0,y*CHUNK_SIZE+(row+.5)*16+building.offsetZ};
				const double d=Vec2{p.x,p.z}.distanceFrom({focus.x,focus.z});
				if(d<distance) {distance=d;center=p;along={Cos(building.angle),0,Sin(building.angle)};}
			}
		}
		if(!std::isfinite(distance)) {DBG_LOG(U"[ConstructionReview] fixture building missing");System::Exit();return;}
		center.y=m_world.sampleHeight(static_cast<float>(center.x),static_cast<float>(center.z));
		const Vec3 right{-along.z,0,along.x};
		for(int elevated=0;elevated<2;++elevated)
		{
			const Vec3 midpoint=center+right*(elevated ? 42 : 0);
			Vec3 a=midpoint-along*65,b=midpoint+along*65;
			a.y=m_world.sampleHeight(static_cast<float>(a.x),static_cast<float>(a.z))+(elevated ? 12 : 0);
			b.y=m_world.sampleHeight(static_cast<float>(b.x),static_cast<float>(b.z))+(elevated ? 12 : 0);
			const int nodeA=m_network.addNode(a),nodeB=m_network.addNode(b);
			const int edgeId=*m_network.addEdge(nodeA,nodeB,a+(b-a)/3,a+(b-a)*2/3,RoadType::Arterial,2);
			m_network.applyEdgeTemplate(edgeId,RoadPlanDraft::makeRoadTemplate(1));
			auto* edge=m_network.getEdge(edgeId);edge->edgeState=EdgeState::Planned;edge->useElevation=elevated!=0;
			RoadPlan plan;plan.name=elevated ? U"高架工事の確認" : U"道路工事の確認";plan.edgeIds={edgeId};
			const int id=m_network.addPlan(plan);m_network.getPlan(id)->constructionDuration=100;planIds<<id;
			m_network.getEdge(edgeId)->planId=id;
			const auto affected=RoadConstruction::affectedCells(m_network,{edgeId},m_world);
			for(const auto& cell:affected) if(cell.building) ++oldBuildings;
		}
		beforeClearance=m_clearanceLedger.size();
		m_selectedRoadPlanId=planIds.front();
		m_mode=EditMode::None;
		m_camera.setCaptureState(center+right*18,205,static_cast<float>(-40_deg),static_cast<float>(48_deg));
	}
	const int phase=m_captureFrame/45;
	if(m_captureFrame==45)
	{
		started=m_clock.now;
		for(const int id:planIds)
		{
			startRoadConstruction(m_network.getPlan(id)->edgeIds);
		}
	}
	if(phase>0)
	{
		const std::array<double,6> times{5,26,48,77,95,100};
		m_clock.now=started+times[Min(phase-1,5)];
		tickConstruction();
	}
	m_world.update(m_camera.focusPoint());renderWorld();
	const Font& font=FontAsset(U"Font_Panel14");
	for(size_t i=0;i<planIds.size() && phase>0;++i)
	{
		const auto* plan=m_network.getPlan(planIds[i]);
		if(const auto* edge=m_network.getEdge(plan->edgeIds.front()))
			ConstructionStatus::draw(font,RectF{20,305+i*80.0,365,72},RoadConstruction::progress(m_network,*edge,m_clock.now));
	}
	if(m_captureFrame%45==30) ScreenCapture::SaveCurrentFrame(U"construction_main_{}.png"_fmt(phase));
	if(++m_captureFrame>=315)
	{
		JSON result;
		result[U"fixtureBuildings"]=oldBuildings;
		result[U"clearedCells"]=m_clearanceLedger.size()-beforeClearance;
		bool open=true;
		for(const int id:planIds) open=open && m_network.getPlan(id)->state==PlanState::Complete;
		result[U"bothPlansOpen"]=open;
		result.save(U"construction_review.json");
		DBG_LOG(U"[ConstructionReview] buildings={} cells={} bothOpen={}"_fmt(oldBuildings,m_clearanceLedger.size()-beforeClearance,open));
		System::Exit();
	}
}
