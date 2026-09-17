#include "ZonePalette.hpp"

namespace ZonePalette
{
	Rect zoneButton(int index, int width)
	{
		const int half=(width-24)/2;
		return {8+(index%2)*(half+8),32+(index/2)*35,half,29};
	}
	Rect brushButton(int index, int width)
	{
		const int third=(width-32)/3;
		return {8+index*(third+8),229,third,29};
	}
	StringView description(ZoneType zone)
	{
		switch (zone)
		{
		case ZoneType::LowResidential: return U"道路沿いの空地に戸建て住宅が建ちます";
		case ZoneType::Residential: return U"住宅や低層マンションが建ちます";
		case ZoneType::Commercial: return U"道路沿いに店やオフィスが建ちます";
		case ZoneType::Industrial: return U"道路沿いに工場が建ちます";
		case ZoneType::Agriculture: return U"道に接した平らな土地を畑にします";
		case ZoneType::UrbanControl: return U"新しい開発を抑え、今の土地を保ちます";
		default: return U"用途を解除します。既存の建物は残ります";
		}
	}
	Action draw(const Font& font, const Font& bold, int width, const State& state)
	{
		Action action;
		const ColorF text{.91}, muted{.67,.74,.78}, accent{.65,.88,.92};
		bold(U"用途を選んで、道路の隣を塗る").draw(8,4,accent);
		const ZoneType order[]={ZoneType::LowResidential,ZoneType::Residential,ZoneType::Commercial,ZoneType::Industrial,ZoneType::Agriculture,ZoneType::UrbanControl,ZoneType::Unzoned};
		const StringView labels[]={U"2  低層住宅",U"3  住宅",U"4  商業",U"5  工業",U"6  農地",U"1  市街化調整",U"0  用途を解除"};
		for (int i=0;i<7;++i)
		{
			const Rect rect=zoneButton(i,width);
			const bool selected=state.zone==order[i];
			rect.draw(selected ? ColorF{.18,.35,.39} : rect.mouseOver() ? ColorF{.23,.28,.32} : ColorF{.13,.18,.22});
			Rect{rect.x,rect.y,4,rect.h}.draw(order[i]==ZoneType::Unzoned ? ColorF{.6} : zoneColor(order[i]).withAlpha(1));
			if (selected) { rect.drawFrame(1,accent); }
			font(labels[i]).draw(rect.pos+Point{12,4},text);
			if (rect.leftClicked()) { action.zone=order[i]; }
		}
		font(description(state.zone)).draw(8,179,muted);
		bold(U"塗る範囲").draw(8,207,accent);
		const StringView brushes[]={U"1区画",U"小ブラシ",U"大ブラシ"};
		for (int i=0;i<3;++i)
		{
			const Rect rect=brushButton(i,width);
			rect.draw(state.brushRadius==i ? ColorF{.18,.35,.39} : ColorF{.13,.18,.22});
			font(brushes[i]).draw(rect.pos+Point{9,4},text);
			if (rect.leftClicked()) { action.brushRadius=i; }
		}
		font(U"左ドラッグ: 塗る   Shift+ドラッグ: 矩形").draw(8,266,muted);
		bold(U"街の需要").draw(8,296,accent);
		const double demands[]={state.demand.residential,state.demand.commercial,state.demand.industrial};
		const StringView uses[]={U"住宅",U"商業",U"工業"};
		const ColorF colors[]={zoneColor(ZoneType::Residential).withAlpha(1),zoneColor(ZoneType::Commercial).withAlpha(1),zoneColor(ZoneType::Industrial).withAlpha(1)};
		const int third=(width-32)/3;
		for (int i=0;i<3;++i)
		{
			const int x=8+i*(third+8);
			font(U"{} {}%"_fmt(uses[i],static_cast<int>(Round(Clamp(demands[i],0.0,1.0)*100)))).draw(x,319,text);
			Rect{x,342,third,5}.draw(ColorF{.18,.23,.27});
			RectF{x,342,third*Clamp(demands[i],0.0,1.0),5}.draw(colors[i]);
		}
		const auto& status=state.development;
		bold(state.paused ? U"一時停止中 — 再開すると開発が進みます" : U"開発中 {}区画  /  完成 {}件"_fmt(status.developing,status.completed)).draw(8,365,accent);
		font(U"道路待ち {}  /  敷地不足 {}  /  地形不適 {}"_fmt(status.needsRoad,status.needsSpace,status.unsuitableTerrain)).draw(8,392,muted);
		font(status.checking>0 ? U"{}区画の建築条件を確認中"_fmt(status.checking) : U"需要に応じ約10〜34秒で建ち始めます").draw(8,416,muted);
		font(U"Z / Esc: 終了  ・  既存の建物は残ります").draw(8,445,muted);
		return action;
	}
}
