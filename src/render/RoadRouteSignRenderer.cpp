#include "../../stdafx.h"
#include "RoadRouteSignRenderer.hpp"
#include "../asset/AssetRegistrar.hpp"
#include "../ui/NavigationHeader.hpp"

void RoadRouteSignRenderer::render(const RoadNetwork& network,const GameCamera& camera) const
{
	m_route.reset();
	if(camera.mode()==CameraMode::Overview || camera.mode()==CameraMode::Capture) { return; }
	const auto nearby=network.findEdgeNear(camera.focusPoint(),80);
	if (!nearby) { return; }
	const auto* edge=network.getEdge(*nearby);
	if (!edge || !edge->isRoadbedBuilt()) { return; }
	for (const int id : edge->routeIds)
	{
		const auto* route=network.getRoute(id);
		if (!route || route->kind!=RoadRouteKind::NationalRoute || route->number<=0) { continue; }
		m_route=id;m_bounds=NavigationHeader::routeBounds(Scene::Size());
		const String label=route->name==U"国道{}号"_fmt(route->number) ? U"" : route->name;
		NavigationHeader::drawRoute(Scene::Size(),FontAsset(Asset::CJK14),route->number,label);return;
	}
}
Optional<int> RoadRouteSignRenderer::hitTest(Vec2 point) const
{
	return m_route && m_bounds.contains(point) ? m_route : none;
}
