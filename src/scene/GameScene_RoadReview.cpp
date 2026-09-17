#include "GameScene.hpp"
#include "../railway/TrainConsist.hpp"
#include "../road/LocationSigns.hpp"
#include "../road/RoadSign.hpp"

/// @brief 読み込んだ街で計画・Undo・Redo・保存の実画面を再現する常設の検証モード。
void GameScene::updateRoadPlanReview()
{
	m_clock.speed = TimeSpeed::Paused;
	m_clock.hour = 13.0f;
	m_camera.setBlockInput(true);
	static Vec3 start, bend, end;
	if (m_captureFrame == 0)
	{
		const Vec3 focus = captureStreetCornerPoint(captureFocusPoint());
		const auto hit = m_network.findEdgeNearDetailed(focus,200.0f);
		if (!hit) { DBG_LOG(U"[RoadPlanReview] no fixture road"); System::Exit(); return; }
		const auto curve = m_network.getBezier(hit->first);
		if (!curve) { System::Exit(); return; }
		start = curve->positionAt(curve->totalLength*0.5f);
		const Vec3 tangent = curve->tangentAt(curve->totalLength*0.5f);
		bend = start + tangentToRight(tangent)*45;
		end = bend + tangent*60;
		bend.y = m_world.sampleHeight(static_cast<float>(bend.x),static_cast<float>(bend.z));
		end.y = m_world.sampleHeight(static_cast<float>(end.x),static_cast<float>(end.z));
		m_camera.setCaptureState((start+bend+end)/3.0,160.0f,static_cast<float>(-40.0_deg),static_cast<float>(63.0_deg));
		m_mode = EditMode::RoadPlan;
		m_draftRoadPlan.preset = 1;
		m_drawTemplate = RoadPlanDraft::makeRoadTemplate(1);
		m_roadPlanSnapIndex.rebuild(m_network);
		m_panelManager.show(U"draw_template",U"道路計画  [R]",panelRightPos(U"draw_template"));
		FileSystem::CreateDirectories(U"Screenshot");
	}
	if (m_captureFrame == 40)
	{
		m_draftRoadPlan.editor.place(start);
		m_draftRoadPlan.editor.place(end);
		rebuildDraftRoadPlan();
		m_roadPlanCursor = RoadPlanSnapIndex::Hit{end};
	}
	if (m_captureFrame == 90) { generateDraftRoadPlan(); m_roadPlanCursor = none; }
	if (m_captureFrame == 110)
	{
		Array<Vec3> edited=m_draftRoadPlan.editor.points();
		edited.insert(edited.begin()+1,bend);
		m_draftRoadPlan.editor.revise(std::move(edited)); rebuildDraftRoadPlan();
	}
	if (m_captureFrame == 130) { m_draftRoadPlan.editor.undo(); rebuildDraftRoadPlan(); }
	if (m_captureFrame == 170) { m_draftRoadPlan.editor.redo(); rebuildDraftRoadPlan(); }
	if (m_captureFrame == 210)
	{
		const double expectedLength=m_draftRoadPlan.editor.length();
		const double funds=m_economy.funds;
		const int nextEdge=m_network.nextEdgeId(),nextNode=m_network.nextNodeId();
		m_economy.funds=0;
		const bool rejected=!commitDraftRoadPlan();
		const bool unchanged=rejected && m_network.nextEdgeId()==nextEdge && m_network.nextNodeId()==nextNode && m_draftRoadPlan.editor.valid();
		m_economy.funds=funds;
		const bool saved=commitDraftRoadPlan();
		const auto* plan=selectedRoadPlanId() ? m_network.getPlan(*selectedRoadPlanId()) : nullptr;
		const bool lengthMatches=plan && Abs(plan->totalLength-expectedLength) < 0.5;
		DBG_LOG(U"[RoadPlanReview] saved={} lengthMatches={} expected={:.3f} actual={:.3f}"_fmt(saved,lengthMatches,expectedLength,plan ? plan->totalLength : 0.0f));
		JSON result;
		result[U"saved"]=saved;
		result[U"insufficientFundsPreservesCity"]=unchanged;
		result[U"constructionStarted"]=plan && plan->state==PlanState::UnderConstruction;
		result[U"lengthMatches"]=lengthMatches;
		result[U"previewLength"]=expectedLength;
		result[U"savedLength"]=plan ? plan->totalLength : 0.0f;
		result.save(U"road_plan_review.json");
	}
	m_world.update(m_camera.focusPoint());
	renderWorld();
	if (m_captureFrame == 20 || m_captureFrame == 70 || m_captureFrame == 110 || m_captureFrame == 150 || m_captureFrame == 190 || m_captureFrame == 230)
	{
		ScreenCapture::SaveCurrentFrame(U"road_ux_{:03}.png"_fmt(m_captureFrame));
	}
	if (++m_captureFrame >= 250) { System::Exit(); }
}

/// @brief 新規生成された街の編成・市町村界・道路通称名を実描画で確認する。
void GameScene::updateTransportObjectsReview()
{
	m_clock.speed=TimeSpeed::Paused; m_clock.hour=13;
	if (m_captureIndex==0 && m_captureFrame==0) { m_trainManager.update(6,m_clock.now); }
	if (m_captureCameraDirty)
	{
		bool found=false;
		if (m_captureIndex<2)
		{
			for (const auto& train:m_trainManager.trains())
			{
				if ((train.type==TrainType::Local)!=(m_captureIndex==0)) { continue; }
				const auto center=TrainConsist::behind(train,m_trainNetwork,TrainConsist::length(train.type)*.5f);
				if (!center || center->y<m_world.sampleHeight(static_cast<float>(center->x),static_cast<float>(center->z))) { continue; }
				m_camera.setCaptureState(*center,m_captureIndex==0 ? 75.0f : 120.0f,train.heading+static_cast<float>(-55_deg),static_cast<float>(24_deg));
				DBG_LOG(U"[TransportObjects] train={} cars={} speed={:.2f}"_fmt(train.id,TrainConsist::profile(train.type).cars,train.speed));
				found=true;break;
			}
		}
		else
		{
			const auto name=[&](Vec2 point) { const int id=m_districtHierarchy.at(point,0);return id>=0 ? m_districtHierarchy.areas[id].name : U""; };
			for (const auto& edge:m_network.edges())
			{
				if (edge.id<0 || edge.tunnel || edge.length<90) { continue; }
				const auto curve=m_network.getBezier(edge.id);float arc=-1;String label;
				if (m_captureIndex==2)
				{
					const auto boundaries=LocationSigns::boundaries(*curve,name);
					if (boundaries.isEmpty()) { continue; } arc=boundaries.front().arc;label=boundaries.front().after;
				}
				else
				{
					for (int routeId:edge.routeIds)
					{
						const auto* route=m_network.getRoute(routeId);if (!route || route->name.isEmpty()) { continue; }
						for (const auto& anchor:m_network.routeSignAnchors(*route)) { if (anchor.first==edge.id) { arc=anchor.second+(route->number>0 ? 8 : 0);label=route->name;break; } }
						if (arc>=0) { break; }
					}
				}
				if (arc<0) { continue; }
				const Vec3 tangent=curve->tangentAt(arc);Vec3 point=curve->positionAt(arc);
				if (!edge.usesDesignHeight()) { point.y=m_world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z)); }
				const auto ext=RoadSign::roadbedExtentsOf(edge);
				point+=tangentToRight(tangent)*(ext.left-(m_captureIndex==2 ? 1.4 : 1.5))+Vec3{0,2.1,0};
				m_camera.setCaptureState(point,11,static_cast<float>(Atan2(-tangent.x,-tangent.z)),static_cast<float>(8_deg));
				DBG_LOG(U"[TransportObjects] signView={} edge={} text={}"_fmt(m_captureIndex,edge.id,label));
				found=true;break;
			}
		}
		if (!found) { DBG_LOG(U"[TransportObjects] fixtureMissing={}"_fmt(m_captureIndex));System::Exit();return; }
		m_captureCameraDirty=false;
	}
	m_trainManager.update(1.0/30,m_clock.now);
	m_world.update(m_camera.focusPoint());renderWorld();
	if (m_captureFrame==65 && m_worldRenderer.pendingTerrainJobs()>0) { return; }
	if (++m_captureFrame==70) { ScreenCapture::SaveCurrentFrame(U"transport_objects_{}.png"_fmt(m_captureIndex));DBG_LOG(U"[TransportObjects] captured={} trains={}"_fmt(m_captureIndex,m_trainManager.trains().size())); }
	if (m_captureFrame>78)
	{
		if (++m_captureIndex==4) { System::Exit();return; }
		m_captureFrame=0;m_captureCameraDirty=true;
	}
}
