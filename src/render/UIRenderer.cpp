#include "UIRenderer.hpp"
#include "../asset/AssetRegistrar.hpp"

void UIRenderer::updateLayout(bool walking, bool driving, bool hasMode)
{
	m_hud.updateLayout(Scene::Size(),walking,driving,hasMode);
}

CityHud::Action UIRenderer::handleInput(bool enabled)
{
	return m_hud.interact(Cursor::PosF(),MouseL.down(),enabled);
}

void UIRenderer::render(const GameClock& clock, int vehicleCount, StringView modeText,
	const Economy& economy, const CityHudStats& stats, bool walking, bool driving)
{
	updateLayout(walking,driving,!modeText.empty());
	m_hud.draw(clock,vehicleCount,modeText,economy,stats,FontAsset(Asset::Small16));
}
