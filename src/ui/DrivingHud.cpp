#include "DrivingHud.hpp"

RectF DrivingHud::instrumentBounds(Size size)
{
	const double scale=Clamp(size.x/1280.0,.625,1.5);
	return {size.x*.59,size.y-106*scale,160*scale,83*scale};
}

void DrivingHud::draw(const Font& font,Size size,const State& state)
{
	const double scale=Clamp(size.x/1280.0,.625,1.5);
	const double width=size.x,height=size.y;
	Polygon{{{0,height-88*scale},{width*.32,height-112*scale},{width*.80,height-101*scale},
		{width,height-76*scale},{width,height},{0,height}}}.draw(ColorF{.055,.063,.071});
	Line{{0,height-88*scale},{width*.32,height-112*scale}}.draw(2*scale,ColorF{.26,.27,.27});
	Line{{width*.32,height-112*scale},{width*.8,height-101*scale}}.draw(2*scale,ColorF{.26,.27,.27});
	const Vec2 wheel{width*.70,height+30*scale};const double radius=143*scale;
	Circle{wheel,radius}.drawFrame(13*scale,ColorF{.025,.029,.033});
	Circle{wheel,radius-9*scale}.drawFrame(1*scale,ColorF{.28,.30,.30});
	for (int index=0;index<3;++index)
	{
		const double angle=state.steering*14+index*Math::TwoPi/3;
		const Vec2 direction{Cos(angle),Sin(angle)};
		Line{wheel+direction*(radius*.23),wheel+direction*(radius-.06*radius)}.draw(11*scale,ColorF{.11,.12,.13});
	}
	const auto instrument=instrumentBounds(size);
	instrument.rounded(12*scale).draw(ColorF{.015,.024,.030});
	const String gear=state.speed<-.05 ? U"R" : U"D";
	font(gear).draw(17*scale,instrument.pos+Vec2{10,12}*scale,ColorF{.50,.95,.71});
	font(U"{:.0f}"_fmt(Abs(state.speed)*3.6)).draw(39*scale,instrument.pos+Vec2{39,2}*scale,ColorF{.94,.98,1});
	font(U"km/h").draw(11*scale,instrument.pos+Vec2{105,30}*scale,ColorF{.65,.76,.83});
	font(U"{:.1f} km"_fmt(state.distance/1000)).draw(12*scale,instrument.pos+Vec2{40,57}*scale,ColorF{.61,.73,.77});
	const double textSize=Max(11.0,13*scale);
	font(U"運転   C：降りる   Esc：メニュー").draw(textSize,Vec2{16,height-79*scale},ColorF{.93});
	font(U"W：アクセル  S：ブレーキ・後退").draw(textSize,Vec2{16,height-56*scale},ColorF{.76,.81,.84});
	font(U"A/D：ハンドル  Space：強ブレーキ  P：停止").draw(textSize,Vec2{16,height-33*scale},ColorF{.76,.81,.84});
	const String status=state.paused ? U"停止中  Pで再開" : state.blocked ? U"路肩・車両に接近  後退して戻れます" : U"";
	if (!status.isEmpty())
	{
		const RectF panel{16,height-155*scale,Min(390.0,width*.5),29};
		panel.rounded(5).draw(ColorF{.04,.06,.08,.86});
		font(status).draw(13,panel.pos+Vec2{9,5},ColorF{1,.84,.49});
	}
	if (state.limit>0)
	{
		const Vec2 center{width-38*scale,height-140*scale};
		Circle{center,22*scale}.draw(ColorF{.96});Circle{center,22*scale}.drawFrame(4*scale,ColorF{.72,.08,.08});
		font(U"{:.0f}"_fmt(state.limit)).drawAt(18*scale,center,ColorF{.10});
	}
}
