#include "../../stdafx.h"
#include "MinimapRenderer.hpp"
#include "../debug/DebugLog.hpp"
#include "../road/RoadEnvironment.hpp"

void MinimapRenderer::buildTerrainTexture(const World& world)
{
	m_world=&world;m_localTerrain.invalidate();m_fullTerrain.invalidate();m_map.invalidateCartography();
}

float MinimapRenderer::terrainHeight(Vec2 point) const
{
	const float x=static_cast<float>(point.x),z=static_cast<float>(point.y);
	const auto* chunk=m_world->getChunk({static_cast<int>(x/CHUNK_SIZE),static_cast<int>(z/CHUNK_SIZE)});
	return chunk && !chunk->heightMap.isEmpty() ? m_world->sampleHeight(x,z) : m_world->computeHeight(x,z);
}

void MinimapRenderer::updateRoadOverlay([[maybe_unused]] const RoadNetwork& roads,[[maybe_unused]] const World& world)
{
	m_mapDirty=true;m_localTerrain.invalidate();m_fullTerrain.invalidate();
}
void MinimapRenderer::updateRoadOverlayAround([[maybe_unused]] const Array<int>& nodes,[[maybe_unused]] const RoadNetwork& roads)
{
	m_mapDirty=true;m_localTerrain.invalidate();m_fullTerrain.invalidate();
}
void MinimapRenderer::refreshMap(const RoadNetwork& roads,const TrainNetwork& railway,const Array<MapGenerator::Settlement>& settlements)
{
	if (m_railEdges!=railway.edges().size() || m_railNodes!=railway.nodes().size())
	{
		m_railEdges=railway.edges().size(); m_railNodes=railway.nodes().size(); m_mapDirty=true;
	}
	if (m_mapDirty)
	{
		m_map.streets.clear(); m_map.labels.clear();
		const auto append = [&](const CubicBezier& curve, double width, int category, bool candidate)
		{
			WorldMapView::Stroke stroke;stroke.width=width;stroke.category=category;
			const auto finish=[&]()
			{
				if(stroke.points.size()<2) { return; }
				Vec2 lower{Math::Inf,Math::Inf},upper{-Math::Inf,-Math::Inf};
				for(const Vec2 point:stroke.points)
				{
					lower.x=Min(lower.x,point.x);lower.y=Min(lower.y,point.y);upper.x=Max(upper.x,point.x);upper.y=Max(upper.y,point.y);
				}
				stroke.bounds=RectF{lower,upper-lower}.stretched(30);m_map.streets << stroke;
			};
			const int count=Max(2,static_cast<int>(Ceil(curve.totalLength/(candidate ? 6 : 30))));
			stroke.points << Vec2{curve.p0.x,curve.p0.z};
			for(int index=1;index<=count;++index)
			{
				const Vec3 point=curve.positionAt(curve.totalLength*index/count);
				const bool tunnel=m_world && RoadEnvironment::coveredAt(*m_world,curve.positionAt(curve.totalLength*(index-.5f)/count),candidate,
					category==2 ? RoadEnvironment::kRailTunnelCrown : RoadEnvironment::kRoadTunnelCrown);
				if(stroke.points.size()>1 && tunnel!=stroke.tunnel)
				{
					finish();const Vec2 previous=stroke.points.back();stroke.points={previous};
				}
				stroke.tunnel=tunnel;stroke.points << Vec2{point.x,point.z};
			}
			finish();
		};
		for (const auto& edge : roads.edges())
		{
			if (edge.id<0 || edge.edgeState==EdgeState::Planned) { continue; }
			if (const auto curve=roads.getBezier(edge.id)) { append(*curve,edge.totalWidth(),edge.roadType==RoadType::LocalRoad ? 0 : 1,edge.tunnel); }
		}
		for (const auto& edge : railway.edges())
		{
			if (edge.id<0) { continue; }
			if (const auto curve=railway.getBezier(edge.id)) { append(*curve,4,2,true); }
		}
		for (const auto& node : railway.nodes())
		{
			if (node.type==TrackNodeType::Station) { m_map.labels << WorldMapView::Label{{node.position.x,node.position.z},node.name+U"駅",true}; }
		}
		m_map.rivers.clear(); m_map.boundaries.clear();
		if (m_world) for (const auto& reach : m_world->rivers().reaches) { m_map.rivers << WorldMapView::Stroke{{{reach.start.x,reach.start.z},{reach.end.x,reach.end.z}},reach.bounds,reach.halfWidth*2,0}; }
		if (m_districts)
		{
			for (const auto& area : m_districts->areas)
			{
				const String label=area.level==2 && area.urban ? m_districts->areas[area.parent].name+area.name : area.name;
				m_map.labels << WorldMapView::Label{area.center,label,false,area.level==0 ? 1.0 : area.level==1 ? 8.0 : 36.0,area.level==2 && area.urban ? m_districts->areas[area.parent].reading+U" "+area.reading : area.reading};
			}
			for (const auto& border : m_districts->boundaries) { m_map.boundaries << WorldMapView::Border{border.a,border.b,border.level}; }
			m_map.addressAt=[this](Vec2 p) { return m_districts->address(p); };
		}
		else { for (const auto& town : settlements) { m_map.labels << WorldMapView::Label{town.center,SettlementNames::name(town),false,1,SettlementNames::reading(town)}; } }
		m_map.invalidateCartography();
		m_mapDirty=false;
	}
}
Optional<Vec2> MinimapRenderer::update(const GameCamera& camera,const RoadNetwork& roads,const TrainNetwork& railway,const Array<MapGenerator::Settlement>& settlements)
{
	refreshMap(roads,railway,settlements);
	const Vec3 focus=camera.focusPoint();
	const Vec3 direction=camera.camera3D().getFocusPosition()-camera.eyePosition();
	m_local.follow({focus.x,focus.z},{direction.x,direction.z});
	m_consumedInput=m_map.visible;
	if (m_map.visible) { return m_map.update(Scene::Size(),Vec2{focus.x,focus.z}); }
	bool open=GameInput::down(KeyM);
	if (m_smallVisible && smallRect().contains(Cursor::PosF()))
	{
		m_consumedInput=MouseL.down() || Mouse::Wheel()!=0;
		open|=m_local.interact(smallRect(),Cursor::PosF(),MouseL.down(),Mouse::Wheel())==LocalMapView::Action::OpenFullScreen;
	}
	if (open) { openFullScreen(camera,roads,railway,settlements); }
	return none;
}
void MinimapRenderer::openFullScreen(const GameCamera& camera,const RoadNetwork& roads,const TrainNetwork& railway,const Array<MapGenerator::Settlement>& settlements)
{
	refreshMap(roads,railway,settlements);m_consumedInput=true;
	const Vec3 focus=camera.focusPoint();m_map.open({focus.x,focus.z});
}
void MinimapRenderer::drawFullScreen(const GameCamera& camera) const
{
	if (!m_world) { return; }
	const Size size=Scene::Size(); const Vec2 a=m_map.toWorld(m_map.body(size).tl(),size),b=m_map.toWorld(m_map.body(size).br(),size);
	const RectF visible{Vec2{Min(a.x,b.x),Min(a.y,b.y)},Vec2{Abs(b.x-a.x),Abs(b.y-a.y)}};
	const Stopwatch timer{StartImmediately::Yes};
	if (m_fullTerrain.update(visible,{Min(1536,size.x*3/2),Min(1152,size.y*3/2)},[this](Vec2 point){return terrainHeight(point);})) { m_map.invalidateCartography();DBG_LOG(U"[MapTerrain] full {} px step={:.2f}m build={:.1f}ms"_fmt(m_fullTerrain.texture.size(),m_fullTerrain.metersPerPixel,timer.msF())); }
	const Vec3 focus=camera.focusPoint();m_map.draw(size,m_fullTerrain.texture,m_font,{focus.x,focus.z},m_fullTerrain.bounds);
}
void MinimapRenderer::render([[maybe_unused]] const GameCamera& camera,[[maybe_unused]] const Array<MapGenerator::Settlement>& settlements) const
{
	if (!m_world || !m_smallVisible) { return; }
	const RectF area=m_local.body(smallRect());
	const double diameter=m_local.span()*area.size.length()/Min(area.w,area.h);
	const Stopwatch timer{StartImmediately::Yes};
	if (m_localTerrain.update({m_local.center-Vec2{diameter,diameter}*.5,diameter,diameter},{512,512},[this](Vec2 point){return terrainHeight(point);})) { DBG_LOG(U"[MapTerrain] local {} px step={:.2f}m build={:.1f}ms"_fmt(m_localTerrain.texture.size(),m_localTerrain.metersPerPixel,timer.msF())); }
	m_local.draw(smallRect(),m_localTerrain.texture,m_font,m_map,m_localTerrain.bounds);
}
RectF MinimapRenderer::smallRect() const
{
	return m_smallBounds.value_or(RectF{Scene::Width()-kDisplaySize-kMargin,kMargin,kDisplaySize,kDisplaySize});
}
