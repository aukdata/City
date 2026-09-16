#pragma once
#include <Siv3D.hpp>
#include "PlainLabel.hpp"

/// @brief 俯瞰で指した場所の地名・読み・道路を左下に小さく表示する。
namespace LocationTooltip
{
	struct Content { String place,reading,road; int nationalNumber=0; };
	inline RectF bounds(Size viewport,const Content& content,double bottomInset=12)
	{
		constexpr double kMargin=12,kWidth=360;
		const double height=20+(content.road.isEmpty() ? 0 : 28);
		return {kMargin,Max(kMargin,viewport.y-bottomInset-height),Max(0.0,Min(kWidth,viewport.x-kMargin*2)),height};
	}
	inline void draw(Size viewport,const Content& content,const Font& font,const Texture& shield,double bottomInset=12)
	{
		if(content.place.isEmpty() && content.road.isEmpty()) { return; }
		const RectF box=bounds(viewport,content,bottomInset);
		const String place=content.place+(content.reading.isEmpty() ? U"" : U"  "+content.reading);
		PlainLabel::fitted(font,place,13,{box.pos,box.w,20},Palette::White,ColorF{.05,.08,.10});
		if(!content.road.isEmpty())
		{
			double inset=0;
			if(content.nationalNumber>0)
			{
				shield.resized(30,27).draw(box.x,box.y+20);
				font(content.nationalNumber).drawAt(10,box.x+15,box.y+33,Palette::White);inset=36;
			}
			PlainLabel::fitted(font,content.road,13,{box.x+inset,box.y+23,box.w-inset,22},Palette::White,ColorF{.05,.08,.10});
		}
	}
}
