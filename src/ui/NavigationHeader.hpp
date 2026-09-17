#pragma once
#include <Siv3D.hpp>

/// @brief 現在地と国道を画面上部に固定し、道路の見通しを確保する。
namespace NavigationHeader
{
	inline RectF placeBounds(Size size)
	{
		const double width=Clamp(size.x-764.0,156.0,360.0);
		return {size.x*.5-width*.5,10,width,44};
	}
	inline RectF routeBounds(Size size)
	{
		const auto place=placeBounds(size);
		return {place.x,58,place.w,28};
	}
	inline void fitted(const Font& font,StringView text,RectF bounds,double fontSize,ColorF color)
	{
		String label{text};
		while (label.size()>1 && font(label).region(fontSize).w>bounds.w) { label.pop_back(); }
		if (label.size()<text.size() && label.size()>1) { label.back()=U'…'; }
		font(label).drawAt(fontSize,bounds.center(),color);
	}
	inline void drawPlace(Size size,const Font& font,StringView name,StringView reading)
	{
		if (name.isEmpty()) { return; }
		const auto bounds=placeBounds(size);
		bounds.rounded(5).draw(ColorF{.07,.13,.17,.87});
		fitted(font,name,{bounds.x+8,bounds.y+2,bounds.w-16,24},19,Palette::White);
		fitted(font,reading,{bounds.x+8,bounds.y+25,bounds.w-16,15},11,ColorF{.75,.85,.9});
	}
	inline void drawRoute(Size size,const Font& font,int number,StringView name)
	{
		const auto bounds=routeBounds(size);
		bounds.rounded(4).draw(ColorF{.08,.24,.43,.94});
		fitted(font,U"国道 {}号  {}"_fmt(number,name),bounds.stretched(-8,-2),14,Palette::White);
	}
}
