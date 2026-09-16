#pragma once
#include <Siv3D.hpp>

/// @brief 地図と俯瞰HUDの背景なしラベル。文字の細い縁取りだけで読みやすさを保つ。
namespace PlainLabel
{
	inline void draw(const Font& font,StringView text,double size,Vec2 position,ColorF color,ColorF outline)
	{
		font(text).draw(TextStyle::Outline(0.0,.10,outline),size,position,color);
	}
	inline void fitted(const Font& font,StringView text,double size,RectF bounds,ColorF color,ColorF outline,bool centered=false)
	{
		String label{text};
		while(label.size()>1 && font(label).region(size).w>bounds.w) { label.pop_back(); }
		if(label.size()<text.size() && label.size()>1) { label.back()=U'…'; }
		if(label.isEmpty() || font(label).region(size).w>bounds.w) { return; }
		const auto style=TextStyle::Outline(0.0,.10,outline);
		if(centered) { font(label).drawAt(style,size,bounds.center(),color); }
		else { font(label).draw(style,size,bounds.pos,color); }
	}
}
