#pragma once
#include <Siv3D.hpp>

/// @brief A compact HUD header keeps its toggle and input bounds when folded.
struct CollapsibleHudPanel
{
	bool collapsed = false;
	RectF expanded{10,10,360,238};
	[[nodiscard]] RectF bounds() const { return {expanded.pos,expanded.w,collapsed ? 32.0 : expanded.h}; }
	[[nodiscard]] RectF toggleBounds() const { return {expanded.x+expanded.w-32,expanded.y,32,32}; }
	bool click(Vec2 point)
	{
		if (!toggleBounds().contains(point)) { return false; }
		collapsed = !collapsed;
		return true;
	}
	void draw(const Font& font, StringView title) const
	{
		bounds().draw(ColorF{0,0,0,.56}).drawFrame(1,ColorF{1,.18});
		font(title).draw(16,expanded.pos+Vec2{8,6},Palette::White);
		const RectF button=toggleBounds();
		if (button.mouseOver()) { button.draw(ColorF{1,.15}); Cursor::RequestStyle(CursorStyle::Hand); }
		font(collapsed ? U"＋" : U"−").drawAt(20,button.center(),Palette::White);
	}
};
