#include "GameScene.hpp"

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
		m_draftRoadPlan.editor.place(bend);
		rebuildDraftRoadPlan();
		m_roadPlanCursor = RoadPlanSnapIndex::Hit{end};
	}
	if (m_captureFrame == 90) { m_draftRoadPlan.editor.place(end); rebuildDraftRoadPlan(); m_roadPlanCursor = none; }
	if (m_captureFrame == 130) { m_draftRoadPlan.editor.undo(); rebuildDraftRoadPlan(); }
	if (m_captureFrame == 170) { m_draftRoadPlan.editor.redo(); rebuildDraftRoadPlan(); }
	if (m_captureFrame == 210)
	{
		const double expectedLength=m_draftRoadPlan.editor.length();
		const bool saved=commitDraftRoadPlan();
		const auto* plan=selectedRoadPlanId() ? m_network.getPlan(*selectedRoadPlanId()) : nullptr;
		const bool lengthMatches=plan && Abs(plan->totalLength-expectedLength) < 0.5;
		DBG_LOG(U"[RoadPlanReview] saved={} lengthMatches={} expected={:.3f} actual={:.3f}"_fmt(saved,lengthMatches,expectedLength,plan ? plan->totalLength : 0.0f));
		JSON result;
		result[U"saved"]=saved;
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
