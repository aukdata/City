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
	m_mapDirty=true;m_localTerrain.refresh();m_fullTerrain.refresh();
}
void MinimapRenderer::updateRoadOverlayAround([[maybe_unused]] const Array<int>& nodes,[[maybe_unused]] const RoadNetwork& roads)
{
	m_mapDirty=true;m_localTerrain.refresh();m_fullTerrain.refresh();
}
void MinimapRenderer::refreshWaterStrokes() const
{
	const auto* water=m_world ? m_world->sourceWater() : nullptr;
	if (!water || water->channels().empty()) { return; }
	m_map.rivers.clear();const auto& channels=water->channels();
	std::vector<int> incoming(channels.size(),0);std::vector<bool> used(channels.size(),false);
	for (const auto& channel:channels) { if (channel.downstream>=0) { ++incoming[static_cast<std::size_t>(channel.downstream)]; } }
	for (std::size_t first=0;first<channels.size();++first)
	{
		if (incoming[first]==1 || used[first] || water->channelDepth(first)<=.003) { continue; }
		WorldMapView::Stroke stroke;const auto& begin=channels[first];stroke.width=(begin.width0+begin.width1)*.5;
		stroke.points << Vec2{begin.x0,begin.z0};std::size_t current=first;Vec2 lower{begin.x0,begin.z0},upper=lower;
		while (!used[current] && water->channelDepth(current)>.003)
		{
			used[current]=true;const auto& c=channels[current];const Vec2 end{c.x1,c.z1};
			lower.x=Min(lower.x,end.x);lower.y=Min(lower.y,end.y);upper.x=Max(upper.x,end.x);upper.y=Max(upper.y,end.y);
			const bool finish=c.downstream<0 || incoming[static_cast<std::size_t>(c.downstream)]!=1;
			if (finish || stroke.points.back().distanceFrom(end)>=48) { stroke.points << end; }
			if (finish) { break; }current=static_cast<std::size_t>(c.downstream);
		}
		if (stroke.points.size()>1) { stroke.bounds=RectF{lower,upper-lower}.stretched(stroke.width);m_map.rivers << std::move(stroke); }
	}
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
			if (edge.id<0 || !edge.hasRoadLanes() || edge.edgeState==EdgeState::Planned) { continue; }
			if (const auto curve=roads.getBezier(edge.id)) { append(*curve,edge.totalWidth(),edge.roadType==RoadType::Expressway ? 3 : edge.roadType==RoadType::LocalRoad ? 0 : 1,edge.tunnel); }
		}
		for (const auto& edge : railway.edges())
		{
			if (edge.id<0 || edge.edgeState==EdgeState::Planned) { continue; }
			if (const auto curve=railway.getBezier(edge.id)) { append(*curve,4,2,true); }
		}
		for (const auto& node : railway.nodes())
		{
			if (node.type==TrackNodeType::Station) { m_map.labels << WorldMapView::Label{{node.position.x,node.position.z},node.isPortal() ? node.name : node.name+U"駅",true}; }
		}
		m_map.rivers.clear(); m_map.boundaries.clear();
		refreshWaterStrokes();
		if (m_world && !m_world->sourceWater()) for (const auto& reach : m_world->rivers().reaches) { m_map.rivers << WorldMapView::Stroke{{{reach.start.x,reach.start.z},{reach.end.x,reach.end.z}},reach.bounds,reach.halfWidth*2,0}; }
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
Optional<Vec2> MinimapRenderer::update(const GameCamera& camera,const RoadNetwork& roads,const TrainNetwork& railway,const Array<MapGenerator::Settlement>& settlements, bool pointerBlocked)
{
	refreshMap(roads,railway,settlements);
	const Vec3 focus=camera.focusPoint();
	const Vec3 direction=camera.camera3D().getFocusPosition()-camera.eyePosition();
	m_local.follow({focus.x,focus.z},{direction.x,direction.z});
	return updateInput({focus.x,focus.z}, Cursor::PosF(), MouseL.down(), Mouse::Wheel(), GameInput::down(KeyM), pointerBlocked);
}

Optional<Vec2> MinimapRenderer::updateInput(Vec2 focus, Vec2 cursor, bool clicked, double wheel, bool mapShortcut, bool pointerBlocked)
{
	m_consumedInput=m_map.visible;
	if (m_map.visible) { return m_map.update(Scene::Size(),focus); }
	bool open=mapShortcut;
	if (!pointerBlocked && m_smallVisible && smallRect().contains(cursor))
	{
		m_consumedInput=clicked || wheel!=0;
		open|=m_local.interact(smallRect(),cursor,clicked,wheel)==LocalMapView::Action::OpenFullScreen;
	}
	if (open) { m_map.open(focus); m_consumedInput=true; }
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
	if (const auto* water=m_world->sourceWater(); water && water->surfaceRevision()!=m_waterMapRevision && Scene::Time()-m_waterMapUpdated>10)
	{ m_fullTerrain.refresh();m_localTerrain.refresh();refreshWaterStrokes();m_map.invalidateCartography();m_waterMapRevision=water->surfaceRevision();m_waterMapUpdated=Scene::Time(); }
	const Size size=Scene::Size(); const Vec2 a=m_map.toWorld(m_map.body(size).tl(),size),b=m_map.toWorld(m_map.body(size).br(),size);
	const RectF visible{Vec2{Min(a.x,b.x),Min(a.y,b.y)},Vec2{Abs(b.x-a.x),Abs(b.y-a.y)}};
	const Stopwatch timer{StartImmediately::Yes};
	// 全画面地図の間は 3D を描かないので、1 フレームの多くを陰影図に回す。拡大縮小の後に地形の欠けた範囲が長く残らない。
	if (m_fullTerrain.update(visible,{Min(1536,size.x*3/2),Min(1152,size.y*3/2)},[this](Vec2 point){return terrainHeight(point);},kFullMapBudgetMs,[this](Vec2 point,float height){return m_world->sourceWater() ? m_world->sourceWater()->sample(point.x,point.y,height).depth : Max(0.0,-static_cast<double>(height));})) { m_map.invalidateCartography();DBG_LOG(U"[MapTerrain] full {} px step={:.2f}m build={:.1f}ms"_fmt(m_fullTerrain.texture.size(),m_fullTerrain.metersPerPixel,timer.msF())); }
	const Vec3 focus=camera.focusPoint();m_map.draw(size,m_fullTerrain.texture,m_font,{focus.x,focus.z},m_fullTerrain.bounds);
}
void MinimapRenderer::render([[maybe_unused]] const GameCamera& camera,[[maybe_unused]] const Array<MapGenerator::Settlement>& settlements) const
{
	if (!m_world || !m_smallVisible) { return; }
	if (const auto* water=m_world->sourceWater(); water && water->surfaceRevision()!=m_waterMapRevision && Scene::Time()-m_waterMapUpdated>10)
	{ m_fullTerrain.refresh();m_localTerrain.refresh();refreshWaterStrokes();m_map.invalidateCartography();m_waterMapRevision=water->surfaceRevision();m_waterMapUpdated=Scene::Time(); }
	const RectF area=m_local.body(smallRect());
	const double diameter=m_local.span()*area.size.length()/Min(area.w,area.h);
	const Stopwatch timer{StartImmediately::Yes};
	if (m_localTerrain.update({m_local.center-Vec2{diameter,diameter}*.5,diameter,diameter},{512,512},[this](Vec2 point){return terrainHeight(point);},1.0,[this](Vec2 point,float height){return m_world->sourceWater() ? m_world->sourceWater()->sample(point.x,point.y,height).depth : Max(0.0,-static_cast<double>(height));})) { DBG_LOG(U"[MapTerrain] local {} px step={:.2f}m build={:.1f}ms"_fmt(m_localTerrain.texture.size(),m_localTerrain.metersPerPixel,timer.msF())); }
	m_local.draw(smallRect(),m_localTerrain.texture,m_font,m_map,m_localTerrain.bounds);
}
RectF MinimapRenderer::smallRect() const
{
	return m_smallBounds.value_or(RectF{Scene::Width()-kDisplaySize-kMargin,kMargin,kDisplaySize,kDisplaySize});
}

void MinimapRenderer::preload(const GameCamera& camera,const RoadNetwork& roads,const TrainNetwork& railway,const Array<MapGenerator::Settlement>& settlements)
{
	refreshMap(roads,railway,settlements);
	String glyphs = U"北が上進行方向駅街の地図閉じる移動拡大現在地右クリックここへジャンプWASDHomeEsc0123456789 kmN−＋: /";
	for (const auto& label : m_map.labels) { glyphs += label.text + label.reading; }
	m_font.preload(glyphs);
	const Vec3 focus=camera.focusPoint(),direction=camera.camera3D().getFocusPosition()-camera.eyePosition();
	m_local.follow({focus.x,focus.z},{direction.x,direction.z});
	const auto area=m_local.body(smallRect());const double diameter=m_local.span()*area.size.length()/Min(area.w,area.h);
	(void)m_localTerrain.update({m_local.center-Vec2{diameter,diameter}*.5,diameter,diameter},{512,512},[this](Vec2 point) { return terrainHeight(point); },Math::Inf,[this](Vec2 point,float height) { return m_world->sourceWater() ? m_world->sourceWater()->sample(point.x,point.y,height).depth : Max(0.0,-static_cast<double>(height)); });
}
