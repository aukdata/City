#pragma once
#include <Siv3D.hpp>
#include "NavigationHeader.hpp"

/// @brief 俯瞰で指した場所の地名・読み・道路を、ポインタの横にまとめる。
namespace LocationTooltip
{
	struct Content { String place,reading,road; int nationalNumber=0; };
	inline RectF bounds(Size viewport,Vec2 pointer,const Content& content)
	{
		constexpr double kMargin=8,kWidth=280,kPlaceHeight=52,kRoadHeight=42;
		const double width=Min(kWidth,viewport.x-kMargin*2),height=kPlaceHeight+(content.road.isEmpty() ? 0 : kRoadHeight);
		double x=pointer.x+18,y=pointer.y+20;
		if(x+width>viewport.x-kMargin) { x=pointer.x-width-18; }
		if(y+height>viewport.y-kMargin) { y=pointer.y-height-16; }
		return {Clamp(x,kMargin,Max(kMargin,viewport.x-width-kMargin)),Clamp(y,kMargin,Max(kMargin,viewport.y-height-kMargin)),width,height};
	}
	inline void draw(Size viewport,Vec2 pointer,const Content& content,const Font& font,const Texture& shield)
	{
		if(content.place.isEmpty() && content.road.isEmpty()) { return; }
		const RectF box=bounds(viewport,pointer,content);
		box.rounded(5).draw(ColorF{.055,.1,.14,.95}).drawFrame(1,ColorF{.45,.65,.72});
		NavigationHeader::fitted(font,content.place,{box.x+10,box.y+5,box.w-20,23},18,Palette::White);
		NavigationHeader::fitted(font,content.reading,{box.x+10,box.y+29,box.w-20,16},12,ColorF{.72,.85,.91});
		if(!content.road.isEmpty())
		{
			double inset=10;
			if(content.nationalNumber>0)
			{
				shield.resized(40,36).draw(box.x+10,box.y+54);
				font(content.nationalNumber).drawAt(13,box.x+30,box.y+72,Palette::White);inset=58;
			}
			NavigationHeader::fitted(font,content.road,{box.x+inset,box.y+57,box.w-inset-10,30},15,Palette::White);
		}
	}
}
