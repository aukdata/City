#include "../../stdafx.h"
#include "PlaceNameRenderer.hpp"
#include "../asset/AssetRegistrar.hpp"
#include "../ui/NavigationHeader.hpp"
#include "../ui/TownBillboards.hpp"
#include "../gen/SettlementNames.hpp"

void PlaceNameRenderer::render(const Array<MapGenerator::Settlement>& settlements,const GameCamera& camera, const World& world, const Array<RectF>& hudBounds) const
{
	m_labels.clear();
	if (camera.mode() == CameraMode::Overview || camera.mode() == CameraMode::Capture)
	{
		Array<TownBillboards::Place> places;
		for (const auto& town : settlements)
		{
			const int importance = town.kind == MapGenerator::SettlementKind::RegionalCity ? 2
				: town.kind == MapGenerator::SettlementKind::LocalTown ? 1 : 0;
			const double ground = world.sampleHeight(static_cast<float>(town.center.x), static_cast<float>(town.center.y));
			places << TownBillboards::Place{{town.center.x, ground + 25 + importance * 30, town.center.y}, SettlementNames::name(town),importance,SettlementNames::reading(town)};
		}
		const Font font = FontAsset(Asset::CJK24);
		m_labels = TownBillboards::layout(places, camera, Scene::Size(), font, hudBounds);
		TownBillboards::draw(m_labels, font);
		return;
	}
	const Vec3 focus=camera.focusPoint();const Vec2 point{focus.x,focus.z};
	const MapGenerator::Settlement* nearest=nullptr;double distance=std::numeric_limits<double>::infinity();
	for (const auto& town : settlements)
	{
		if (town.name.isEmpty()) { continue; }
		const double candidate=town.center.distanceFromSq(point);
		if (candidate<distance) { nearest=&town;distance=candidate; }
	}
	if (nearest) { NavigationHeader::drawPlace(Scene::Size(),FontAsset(Asset::CJK24),SettlementNames::name(*nearest),SettlementNames::reading(*nearest)); }
}
