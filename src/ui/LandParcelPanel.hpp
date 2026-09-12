#pragma once
#include "../world/LandPlot.hpp"

/// @brief Shared parcel information layout, previewed by the Test application.
namespace LandParcelPanel
{
	inline void draw(const Font& font,const LandPatch& patch,Vec2 origin)
	{
		const s3d::Polygon shape=LandPlot::shape(patch);
		font(LandPlot::name(patch)).draw(18,origin,Palette::White);
		font(U"面積: {:.0f} m²"_fmt(shape.area())).draw(14,origin+Vec2{0,34},Palette::White);
		const Vec2 center=shape.boundingRect().center();
		font(U"位置: {:.0f}, {:.0f}"_fmt(center.x,center.y)).draw(14,origin+Vec2{0,58},Palette::White);
		font(patch.sourceParcelKey>=0 ? U"建物に付属する敷地" : U"農地の区画").draw(14,origin+Vec2{0,82},ColorF{.72,.82,.72});
	}
}
