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
		notifyNetworkChanged();
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

void GameScene::selectTrain(int id)
{
	clearSelection();
	m_selectedVehicleId.reset();
	m_trackingVehicle = false;
	m_selection = {SelectionKind::Train, id};
	for (const auto key : {U"vehicle_info", U"building_info", U"edge_info", U"node_info"})
	{
		m_panelManager.hide(key);
	}
	m_panelManager.show(U"rail_info", U"電車 #{}"_fmt(id), panelRightPos(U"rail_info"));
}
void GameScene::selectStation(int id)
{
	const auto* station = m_trainNetwork.getNode(id);
	if (!station || station->type != TrackNodeType::Station)
	{
		return;
	}
	clearSelection();
	m_selectedVehicleId.reset();
	m_trackingVehicle = false;
	m_selection = {SelectionKind::Station, id};
	for (const auto& data : SubsurfaceView::stationGeometry(m_world, m_trainNetwork, id, m_underground))
	{
		m_stationSelectionMeshes << Mesh{data};
	}
	for (const auto key : {U"vehicle_info", U"building_info", U"edge_info", U"node_info"})
	{
		m_panelManager.hide(key);
	}
	m_panelManager.show(U"rail_info", RailInfoPanel::station(id, m_trainNetwork).title, panelRightPos(U"rail_info"));
}
void GameScene::drawRailInfoPanel()
{
	RailInfoPanel::Summary summary;
	Optional<int> schedule;
	if (m_selection.kind == SelectionKind::Train)
	{
		for (const auto& train : m_trainManager.trains())
		{
			if (train.id == m_selection.id)
			{
				summary = RailInfoPanel::train(train, m_trainNetwork);
				schedule = train.scheduleId;
				break;
			}
		}
	}
	else if (m_selection.kind == SelectionKind::Station)
	{
		summary = RailInfoPanel::station(m_selection.id, m_trainNetwork);
	}
	if (summary.title.isEmpty())
	{
		m_panelManager.hide(U"rail_info");
		return;
	}
	auto area = m_panelManager.beginContent(U"rail_info");
	if (!area)
	{
		return;
	}
	RailInfoPanel::draw(summary, m_trackingTrain, FontAsset(Asset::Panel14));
	m_panelManager.reportContentHeight(U"rail_info", RailInfoPanel::height(summary));
	if (m_showPauseMenu || !MouseL.down())
	{
		return;
	}
	if (summary.canFollow && RailInfoPanel::followButton(summary).mouseOver())
	{
		m_trackingTrain = !m_trackingTrain;
		leaveDriving(true);
	}
	if (RailInfoPanel::timetableButton(summary).mouseOver())
	{
		if (schedule)
		{
			if (const auto* service = m_trainNetwork.getSchedule(*schedule))
			{
				m_trainTimetableEditor.select(*service);
			}
		}
		setRailTimetableVisible(true);
	}
}

void GameScene::toggleUnderground()
{
	leaveDriving(true);
	clearSelection();
	m_selectedVehicleId.reset();
	m_trackingVehicle = false;
	for (const auto key :
		{U"vehicle_info", U"building_info", U"edge_info", U"node_info", U"signal_edit", U"guide_sign_edit"})
	{
		m_panelManager.hide(key);
	}
	m_underground = !m_underground;
	m_subsurface.invalidate();
	m_camera.setIgnoreTerrain(m_underground);
	Vec3 focus = m_camera.focusPoint();
	focus.y = m_world.sampleHeight(static_cast<float>(focus.x), static_cast<float>(focus.z)) - (m_underground ? 20 : 0);
	m_camera.setOverviewState(focus, m_camera.distance(), m_camera.yaw(), m_camera.pitch());
}
Optional<int> GameScene::visibleNodeNear(Vec3 position, float radius) const
{
	Optional<int> best;
	double distance = radius * radius;
	for (const auto& node : m_network.nodes())
	{
		if (node.id < 0 || (m_underground && !SubsurfaceView::below(node.position, m_world)))
		{
			continue;
		}
		const double next = node.position.distanceFromSq(position);
		if (next < distance)
		{
			best = node.id;
			distance = next;
		}
	}
	return best;
}
