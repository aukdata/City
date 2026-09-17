#include "GameScene.hpp"
#include "../railway/RailDepotBuilder.hpp"

void GameScene::setRailTimetableVisible(bool visible)
{
	GameInput::releaseTextFocus();
	if (!visible)
	{
		m_panelManager.hide(U"rail_timetable"); m_railTimetableWasVisible = false;
		return;
	}
	leaveDriving(true);
	if (m_mode == EditMode::RoadPlan) { clearDraftRoadPlan(); }
	setZonePaintMode(false); m_mode = EditMode::None; m_trainDrawStartNode = none;
	m_panelManager.hide(U"draw_template");
	if (!m_trainNetwork.getSchedule(m_trainTimetableEditor.selectedId()) && !m_trainTimetableEditor.dirty())
	{
		if (!m_trainNetwork.schedules().isEmpty()) { m_trainTimetableEditor.select(m_trainNetwork.schedules().front()); }
		else
		{
			TrainSchedule schedule; schedule.name=U"新しい路線";
			m_trainTimetableEditor.select(schedule);
			m_trainTimetableEditor.stops={{-1,TextEditState{U"2"}},{-1,TextEditState{U"2"}}};
		}
	}
	m_panelManager.show(U"rail_timetable",U"鉄道・ダイヤ  [H]",panelRightPos(U"rail_timetable"));
	m_railTimetableWasVisible = true;
}
void GameScene::addRailDepot(int station)
{
	String error;
	if (RailDepotBuilder::add(m_trainNetwork,m_world,m_network,station,error))
	{
		m_trainTimetableEditor.message=U"車庫と留置線を追加しました";
		m_soundEffects.play(SoundEffects::Cue::Complete);
	}
	else { m_trainTimetableEditor.message=error; m_soundEffects.play(SoundEffects::Cue::Reject); }
}
void GameScene::drawRailTimetable()
{
	auto area=m_panelManager.beginContent(U"rail_timetable"); if (!area) { return; }
	const auto action=m_trainTimetableEditor.draw(FontAsset(Asset::Panel14),FontAsset(Asset::PanelBold14),m_trainNetwork,m_trainManager,m_clock.now,!m_showPauseMenu);
	m_panelManager.reportContentHeight(U"rail_timetable",m_trainTimetableEditor.contentHeight());
	if (m_showPauseMenu) { return; }
	if (action.apply)
	{
		const bool applied=m_trainTimetableEditor.apply(m_trainNetwork);
		m_soundEffects.play(applied ? SoundEffects::Cue::Complete : SoundEffects::Cue::Reject);
		DBG_LOG(U"[RailTimetable] applied={} id={} message={}"_fmt(applied,m_trainTimetableEditor.selectedId(),m_trainTimetableEditor.message));
	}
	if (action.locateStation)
	{
		if (const auto* node=m_trainNetwork.getNode(*action.locateStation)) { m_camera.setCaptureState(node->position,180,-.7f,.72f); }
	}
	if (action.depotStation) { addRailDepot(*action.depotStation); }
	if (action.locateDepot)
	{
		const auto& depot=m_trainNetwork.depots()[*action.locateDepot];
		if (const auto site=RailwaySite::depotFrame(m_trainNetwork,depot)) { m_camera.setCaptureState(site->point(3,0,65),270,-.7f,.72f); }
	}
}
