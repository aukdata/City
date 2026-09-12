#pragma once
#include "RoadTypes.hpp"

/// @brief reference/ks084812_INDEX.md: 202, 323 and 326. Draw once into cached sign textures.
namespace SignArtwork
{
	inline bool dynamic(RoadSignType type) { return type==RoadSignType::SpeedLimit || type==RoadSignType::OneWay || type==RoadSignType::CurveWarning; }
	inline int key(RoadSignType type,int value) { return static_cast<int>(type)*1000+value; }
	inline void draw(RoadSignType type,int value,const Font& font)
	{
		const Color blue{20,62,142},red{202,28,44},yellow{255,199,25};
		if (type==RoadSignType::SpeedLimit)
		{
			Circle{128,128,124}.draw(Palette::White); Circle{128,128,111}.drawFrame(14,0,red);
			const ScopedRenderStates2D textBlend{BlendState::Default2D};
			font(value).drawAt(112,Vec2{128,126},blue);
		}
		else if (type==RoadSignType::OneWay)
		{
			RectF{5,5,246,246}.draw(Palette::White); RectF{13,13,230,230}.draw(blue);
			Line{128,212,128,76}.draw(37,Palette::White);
			Triangle{Vec2{56,102},Vec2{128,36},Vec2{200,102}}.draw(Palette::White);
		}
		else if (type==RoadSignType::CurveWarning)
		{
			const Quad diamond{Vec2{128,3},Vec2{253,128},Vec2{128,253},Vec2{3,128}};
			diamond.draw(yellow); diamond.drawFrame(7,Palette::Black);
			const double direction=value==1 ? -1.0 : 1.0;
			const Vec2 lower{128-direction*23,200},middle{128-direction*23,128},upper{128+direction*30,86};
			Line{lower,middle}.draw(20,Palette::Black); Line{middle,upper}.draw(20,Palette::Black);
			Triangle{upper+Vec2{-29,10},upper+Vec2{0,-39},upper+Vec2{29,10}}.draw(Palette::Black);
		}
	}
}
