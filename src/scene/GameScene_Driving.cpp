#include "GameScene.hpp"
#include "../ui/DrivingControls.hpp"

bool GameScene::handleDrivingShortcuts()
{
	if (GameInput::down(KeyC) && !GameInput::pressed(KeyControl) && !GameInput::pressed(KeyAlt))
	{
		if (m_driving.active()) { leaveDriving();return true; }
		prepareVehicleRenderData();
		if (!m_driving.enter(m_camera.focusPoint(),m_world,m_network,m_renderVehicles))
		{
			m_drivingNoticeSeconds=5;
			m_soundEffects.play(SoundEffects::Cue::Reject);
			return true;
		}
		m_soundEffects.play(SoundEffects::Cue::EnterCar);
		m_drivingNoticeSeconds=0;
		m_beforeDrivingSpeed=m_clock.speed;m_clock.speed=TimeSpeed::x1;
		m_mode=EditMode::None;m_trackingVehicle=false;m_zoneManager.showOverlay=false;
		m_drawStartNode=none;m_rectStart=none;m_trainDrawStartNode=none;
		m_sandboxDragNode=none;m_sandboxDragCtrl=none;m_editingRouteId=-1;
		m_cursorGroundPos=none;clearSelection();
		for (const StringView id : {U"edge_info",U"node_info",U"guide_sign_edit",U"signal_edit",U"draw_template",U"route_info",U"vehicle_info",U"building_info",U"land_info",U"zone_palette",U"rail_timetable"}) { m_panelManager.hide(id); }
		const auto& vehicle=m_driving.vehicle();
		m_camera.setDrivingState(vehicle.position,vehicle.heading,vehicle.pitch);
		m_vehicleManager.setDrivenVehicle(vehicle,&m_world);
		return true;
	}
	if (!m_driving.active()) { return false; }
	if (GameInput::down(KeyF)) { leaveDriving(true);return true; }
	if (GameInput::down(KeyP)) { m_clock.speed=m_clock.speed==TimeSpeed::Paused ? TimeSpeed::x1 : TimeSpeed::Paused; }
	return true;
}

void GameScene::leaveDriving(bool overview)
{
	if (!m_driving.active()) { return; }
	const Vehicle vehicle=m_driving.vehicle();
	m_soundEffects.stopDriving();m_soundEffects.play(SoundEffects::Cue::ExitCar);
	m_driving.leave();m_vehicleManager.setDrivenVehicle(none);
	m_clock.speed=m_beforeDrivingSpeed;
	m_camera.setWalkingState(vehicle.position,vehicle.heading);
	if (overview) { m_camera.cycleMode(); }
}

void GameScene::updateDriving(double dt,bool blocked)
{
	m_drivingNoticeSeconds=Max(0.0,m_drivingNoticeSeconds-dt);
	if (!m_driving.active()) { m_soundEffects.stopDriving();return; }
	// 一時停止中、入力欄、地図、アプリ切替、保存コマンド中は車を保持する。
	const bool replaying=!getData().playtestCommands.isEmpty() && Scene::Time()<m_playtestDrivingUntil;
	const bool enabled=!blocked && !m_showPauseMenu && !GameInput::keyboardBlocked()
		&& (Window::GetState().focused || replaying) && !GameInput::pressed(KeyControl) && !GameInput::pressed(KeyAlt)
		&& !GameInput::pressed(KeyF3) && m_clock.speed!=TimeSpeed::Paused;
	if (m_clock.speed!=TimeSpeed::Paused) { m_clock.speed=TimeSpeed::x1; }
	const DrivingInput input=replaying && enabled ? m_playtestDrivingInput : DrivingControls::read(enabled);
	prepareVehicleRenderData();
	m_driving.update(dt,input,m_world,m_network,m_renderVehicles,enabled);
	m_soundEffects.updateDriving(dt,enabled,m_driving.vehicle().speed,Max(input.throttle,input.brakeReverse),m_driving.blocked());
	const auto& vehicle=m_driving.vehicle();
	m_camera.setDrivingState(vehicle.position,vehicle.heading,vehicle.pitch);
	m_vehicleManager.setDrivenVehicle(vehicle,&m_world);
}
