#pragma once
#include "PlainLabel.hpp"
#include "WorldMapView.hpp"

/// @brief 視点に追従する周辺地図。全画面地図と道路・鉄道データを共用する。
class LocalMapView
{
public:
	enum class Action { None, OpenFullScreen };
	inline static constexpr std::array<double,5> kSpans{500,1000,2000,4000,8000};
	int zoomIndex=2;
	bool headingUp=false;
	Vec2 center{0,0},forward{0,-1};
	[[nodiscard]] double span() const { return kSpans[zoomIndex]; }
	[[nodiscard]] RectF body(RectF rect) const { return {rect.x,rect.y+28,rect.w,Max(1.0,rect.h-28)}; }
	[[nodiscard]] RectF orientationButton(RectF rect) const { return {rect.x+3,rect.y+2,rect.w-66,24}; }
	[[nodiscard]] RectF zoomButton(RectF rect,bool closer) const { return {rect.x+rect.w-(closer ? 30 : 60),rect.y+2,27,24}; }
	void changeZoom(int steps) { zoomIndex=Clamp(zoomIndex+steps,0,static_cast<int>(kSpans.size())-1); }
	void follow(Vec2 position,Vec2 direction)
	{
		center=position;
		if (direction.lengthSq()>.0001) { forward=direction.normalized(); }
	}
	[[nodiscard]] Vec2 up() const { return headingUp ? forward : Vec2{0,-1}; }
	[[nodiscard]] Vec2 toScreen(Vec2 world,RectF rect) const
	{
		const Vec2 direction=up(),right{direction.y,-direction.x},delta=world-center;
		const RectF area=body(rect); const double scale=Min(area.w,area.h)/span();
		return area.center()+Vec2{delta.dot(right),-delta.dot(direction)}*scale;
	}
	[[nodiscard]] Vec2 toWorld(Vec2 screen,RectF rect) const
	{
		const Vec2 direction=up(),right{direction.y,-direction.x}; const RectF area=body(rect);
		const Vec2 delta=(screen-area.center())*(span()/Min(area.w,area.h));
		return center+right*delta.x-direction*delta.y;
	}
	Action interact(RectF rect,Vec2 cursor,bool clicked,double wheel)
	{
		if (!rect.contains(cursor)) { return Action::None; }
		if (wheel!=0) { changeZoom(wheel<0 ? -1 : 1); }
		if (!clicked) { return Action::None; }
		if (orientationButton(rect).contains(cursor)) { headingUp=!headingUp; }
		else if (zoomButton(rect,true).contains(cursor)) { changeZoom(-1); }
		else if (zoomButton(rect,false).contains(cursor)) { changeZoom(1); }
		else if (body(rect).contains(cursor)) { return Action::OpenFullScreen; }
		return Action::None;
	}
	void draw(RectF rect,const Texture& terrain,const Font& font,const WorldMapView& data,RectF terrainWorld={0,0,WorldMapView::kWorldSize,WorldMapView::kWorldSize}) const
	{
		const ScopedCustomShader2D standard{VertexShader{},PixelShader{}};
		rect.draw(ColorF{.10,.15,.18});
		const RectF area=body(rect); const double scale=Min(area.w,area.h)/span();
		const Rect previous=Graphics2D::GetScissorRect();
		{
			const ScopedRenderStates2D clip{RasterizerState{FillMode::Solid,CullMode::Off,true}};
			Graphics2D::SetScissorRect(Rect{static_cast<int>(area.x),static_cast<int>(area.y),static_cast<int>(area.w),static_cast<int>(area.h)});
			area.draw(ColorF{.72,.79,.68});
			if (!terrain.isEmpty())
			{
				Quad{toScreen(terrainWorld.tl(),rect),toScreen(terrainWorld.tr(),rect),toScreen(terrainWorld.br(),rect),toScreen(terrainWorld.bl(),rect)}(terrain).draw();
			}
			const double radius=span()*area.size.length()/Min(area.w,area.h)*.5;
			const RectF visible{center-Vec2{radius,radius},radius*2,radius*2};
			for (const auto& river : data.rivers)
			{
				if (!river.bounds.intersects(visible) || river.points.size()<2) { continue; }
				MapStroke::draw(toScreen(river.points.front(),rect),toScreen(river.points.back(),rect),area,Max(1.4,river.width*scale),ColorF{.27,.59,.78});
			}
			for (const auto& road : data.streets)
			{
				if (!road.bounds.intersects(visible)) { continue; }
				const double width=Clamp(road.width*scale,road.category==0 ? 1.0 : 1.5,9.0);
				const ColorF color=road.category==2 ? ColorF{.23,.31,.37} : road.category==1 ? ColorF{1,.82,.32} : ColorF{.97};
				double phase=0;
				for (size_t i=1;i<road.points.size();++i)
				{
					const Vec2 a=toScreen(road.points[i-1],rect),b=toScreen(road.points[i],rect);
					MapStroke::draw(a,b,area,width,color,road.tunnel,phase);phase+=a.distanceFrom(b);
				}
			}
			Array<RectF> used;
			for (const auto& label : data.labels)
			{
				if (!label.station && label.minimumZoom>32) { continue; }
				const Vec2 point=toScreen(label.position,rect);
				if (!area.stretched(-14).contains(point)) { continue; }
				if (label.station) { RectF{point-Vec2{3,3},6,6}.draw(ColorF{.1,.3,.52}); }
				const double width=Max(font(label.text).region().w,font(label.reading).region(10).w)+6;
				const RectF bounds{point+Vec2{6,-8},width,label.reading.isEmpty() ? 23.0 : 35.0};
				bool overlap=!area.contains(bounds.br());
				for (const auto& other : used) { overlap|=other.intersects(bounds); }
				if (overlap || used.size()>=4) { continue; }
				used<<bounds;
				PlainLabel::draw(font,label.text,font.fontSize(),bounds.pos+Vec2{3,0},ColorF{.12,.22,.3},ColorF{1,.85});
				if (!label.reading.isEmpty()) { PlainLabel::draw(font,label.reading,10,bounds.pos+Vec2{3,21},ColorF{.3,.38,.41},ColorF{1,.85}); }
			}
			const Vec2 position=area.center(),tip=toScreen(center+forward*(7/scale),rect),right{-(tip-position).y,(tip-position).x};
			Triangle{tip,position-(tip-position)*.6+right*.6,position-(tip-position)*.6-right*.6}.draw(ColorF{.95,.25,.12}).drawFrame(1,Palette::White);
			const Vec2 north=toScreen(center+Vec2{0,-1},rect)-position;
			const Vec2 compass=area.pos+Vec2{16,20};
			Line{compass,compass+north.normalized()*11}.drawArrow(2,{5,5},ColorF{.15,.22,.3});
			font(U"N").draw(compass+Vec2{-5,9},ColorF{.1});
			const double meters=span()*.25,pixels=meters*scale;
			RectF{area.x+5,area.y+area.h-28,pixels+14,25}.draw(ColorF{1,.85});
			Line{area.x+12,area.y+area.h-6,area.x+12+pixels,area.y+area.h-6}.draw(2,ColorF{.2});
			font(meters>=1000 ? U"{} km"_fmt(meters/1000) : U"{} m"_fmt(meters)).draw(area.x+10,area.y+area.h-27,ColorF{.2});
		}
		Graphics2D::SetScissorRect(previous);
		for (const RectF button : {orientationButton(rect),zoomButton(rect,false),zoomButton(rect,true)}) { button.draw(ColorF{.22,.29,.33}); }
		font(headingUp ? U"進行方向が上" : U"北が上").drawAt(orientationButton(rect).center(),Palette::White);
		font(U"−").drawAt(zoomButton(rect,false).center(),Palette::White);
		font(U"＋").drawAt(zoomButton(rect,true).center(),Palette::White);
		rect.drawFrame(1,ColorF{.5,.6,.63});
	}
};
