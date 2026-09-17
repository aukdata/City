#include "GameScene.hpp"
#include "../ui/ZonePalette.hpp"

void GameScene::setZonePaintMode(bool enabled)
{
	if (enabled)
	{
		leaveDriving(true);
		if (m_mode==EditMode::RoadPlan) { clearDraftRoadPlan(); }
		clearSelection();
		m_panelManager.hide(U"draw_template");
		m_mode=EditMode::ZonePaint;
		m_panelManager.show(U"zone_palette",U"用途と街の開発  [Z]",panelRightPos(U"zone_palette"));
	}
	else
	{
		if (m_mode==EditMode::ZonePaint) { m_mode=EditMode::None; }
		m_panelManager.hide(U"zone_palette");
	}
	m_drawStartNode=none; m_rectStart=none;
	m_zoneManager.showOverlay=enabled;
}
void GameScene::drawZonePalette()
{
	if (m_mode!=EditMode::ZonePaint) { return; }
	auto area=m_panelManager.beginContent(U"zone_palette"); if (!area) { return; }
	ZonePalette::State state;
	state.zone=m_paintZone; state.brushRadius=m_zoneBrushRadius;
	state.demand=calculateZoneDevelopmentDemand(m_economy.population,m_citySnapshot);
	state.development=m_zoneManager.developmentSummary(); state.paused=m_clock.speed==TimeSpeed::Paused;
	const auto action=ZonePalette::draw(FontAsset(Asset::Panel14),FontAsset(Asset::PanelBold14),
		static_cast<int>(m_panelManager.getSize(U"zone_palette").x),state);
	if (!m_showPauseMenu)
	{
		if (action.zone) { m_paintZone=*action.zone; m_soundEffects.play(SoundEffects::Cue::Select); }
		if (action.brushRadius) { m_zoneBrushRadius=*action.brushRadius; m_soundEffects.play(SoundEffects::Cue::Select); }
	}
	m_panelManager.reportContentHeight(U"zone_palette",ZonePalette::kHeight);
}
void GameScene::updateZoneDevelopment(double simulationSeconds)
{
	const auto result=m_zoneManager.updateDevelopment(m_world,m_network,
		calculateZoneDevelopmentDemand(m_economy.population,m_citySnapshot),simulationSeconds,m_clock.now,&m_trainNetwork);
	if (result.buildings==0) { return; }
	m_citySnapshot.housingCapacity+=result.housing;
	m_citySnapshot.residentialBuildings+=result.residential;
	m_citySnapshot.commercialBuildings+=result.commercial;
	m_citySnapshot.industrialBuildings+=result.industrial;
	const int residents=static_cast<int>(Floor(result.housing*.72));
	m_economy.population+=residents;
	m_soundEffects.play(SoundEffects::Cue::Complete);
	DBG_LOG(U"[ZoneDevelopment] buildings={} housing={} newResidents={} population={}"_fmt(
		result.buildings,result.housing,residents,m_economy.population));
}
