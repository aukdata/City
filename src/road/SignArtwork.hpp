#pragma once
#include "RoadTypes.hpp"

/// @brief reference/ks084812_INDEX.md: 202, 323 and 326. Draw once into cached sign textures.
namespace SignArtwork
{
	inline bool wide(RoadSignType type) { return type==RoadSignType::RoadName || type==RoadSignType::Municipality; }
	inline bool dynamic(RoadSignType type)
	{
		return type==RoadSignType::SpeedLimit || type==RoadSignType::OneWay || type==RoadSignType::CurveWarning
			|| type==RoadSignType::DirectionalRestriction || type==RoadSignType::NationalRoute
			|| type==RoadSignType::PrefectureRoute || type==RoadSignType::SteepGrade || type==RoadSignType::NarrowRoad || wide(type);
	}
	inline String textureKey(RoadSignType type,int value,StringView label) { return U"{}:{}:{}"_fmt(static_cast<int>(type),value,label); }
	inline int key(RoadSignType type,int value) { return static_cast<int>(type)*1000+value; }
	inline void draw(RoadSignType type,int value,const Font& font,StringView label=U"")
	{
		const Color blue{20,62,142},red{202,28,44},yellow{255,199,25};
		if (wide(type))
		{
			RectF{4,4,760,248}.draw(Palette::White);
			RectF{12,12,744,232}.drawFrame(5,blue);
			const double size=Min(80.0,700.0/Max(1.0,font(label).region().w)*font.fontSize());
			const ScopedRenderStates2D textBlend{BlendState::Default2D};
			font(label).drawAt(size,Vec2{384,140},blue);
			font(type==RoadSignType::Municipality ? U"市町村界" : U"通りの名称").drawAt(26,Vec2{384,52},blue);
		}
		else if (type==RoadSignType::NationalRoute || type==RoadSignType::PrefectureRoute)
		{
			const Polygon board=type==RoadSignType::NationalRoute
				? Polygon{Array<Vec2>{{36,22},{220,22},{244,75},{181,218},{128,246},{75,218},{12,75}}}
				: Polygon{Array<Vec2>{{69,10},{187,10},{247,128},{187,246},{69,246},{9,128}}};
			board.draw(blue).drawFrame(7,Palette::White);
			const ScopedRenderStates2D textBlend{BlendState::Default2D};
			font(type==RoadSignType::NationalRoute ? U"国道" : U"県道").drawAt(27,Vec2{128,51},Palette::White);
			font(value).drawAt(90,Vec2{128,130},Palette::White);
		}
		else if (type==RoadSignType::DirectionalRestriction)
		{
			Circle{128,128,123}.draw(blue).drawFrame(5,Palette::White);
			Line{128,209,128,119}.draw(20,Palette::White);
			if (value&1) { Line{128,140,128,64}.drawArrow(20,Vec2{42,38},Palette::White); }
			if (value&2) { Line{128,126,48,126}.drawArrow(20,Vec2{42,38},Palette::White); }
			if (value&4) { Line{128,126,208,126}.drawArrow(20,Vec2{42,38},Palette::White); }
			if (value&8) { Line{128,109,188,109}.draw(16,Palette::White); Line{188,109,188,192}.drawArrow(16,Vec2{30,30},Palette::White); }
		}
		else if (type==RoadSignType::SteepGrade || type==RoadSignType::NarrowRoad)
		{
			Quad{Vec2{128,3},Vec2{253,128},Vec2{128,253},Vec2{3,128}}.draw(yellow).drawFrame(7,Palette::Black);
			if (type==RoadSignType::SteepGrade)
			{
				Triangle{Vec2{61,170},Vec2{197,170},Vec2{value>0 ? 197.0 : 61.0,108}}.draw(Palette::Black);
				const ScopedRenderStates2D textBlend{BlendState::Default2D};
				font(U"{}%"_fmt(Abs(value))).drawAt(46,Vec2{128,95},Palette::Black);
			}
			else
			{
				for (int side:{-1,1}) { Line{128+side*48.0,193,128+side*48.0,143}.draw(15,Palette::Black); Line{128+side*48.0,143,128+side*22.0,103}.draw(15,Palette::Black); Line{128+side*22.0,103,128+side*22.0,61}.draw(15,Palette::Black); }
			}
		}
		else if (type==RoadSignType::SpeedLimit)
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
			// reference/06 ... (202): 曲線の先端へ向いた矢印。左は右図形の鏡像。
			const auto point=[&](double x,double y) { return Vec2{128+direction*(x-128),y}; };
			Bezier3{point(98,191),point(98,130),point(107,113),point(165,87)}.draw(LineStyle::RoundCap,20,Palette::Black);
			Triangle{point(184,80),point(151,68),point(164,112)}.draw(Palette::Black);
		}
	}
}
