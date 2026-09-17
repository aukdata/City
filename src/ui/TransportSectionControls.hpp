#pragma once
#include "PanelWidget.hpp"
#include "../road/RoadTypes.hpp"

/// @brief 共通断面エディタの交通種別・路盤材質。Test の表示検証でも同じ部品を使う。
namespace TransportSectionControls
{
	inline constexpr StringView kLaneNames[] = {U"通常",U"追越",U"加速",U"減速",U"左折",U"右折",U"バス",U"駐車",U"待避",U"禁止",U"導流帯",U"軌道"};
	inline bool laneKind(const Font& font, Lane& lane, int x, int y, int height)
	{
		const bool changed = PanelWidget::cycle(font,lane.type,kLaneNames,12,x,y,64,height);
		if (changed)
		{
			lane.bidirectional = false; lane.reservedBy = -1;
			if (lane.type == LaneType::Rail) { lane.canChangeLaneLeft = lane.canChangeLaneRight = false; lane.lineLeft = lane.lineRight = LineType::None; }
		}
		return changed;
	}
	inline bool roadbed(const Font& font, RoadPart& part, int x, int y, int height)
	{
		if (part.type != RoadPartType::Roadbed) { return false; }
		const int index = part.defId == U"roadbed_ballast" ? 1 : part.defId == U"roadbed_slab" ? 2 : 0;
		constexpr StringView names[] = {U"舗装",U"バラスト",U"スラブ"};
		if (!PanelWidget::button(font,names[index],false,x,y,80,height,U"路盤材質を切り替える")) { return false; }
		constexpr StringView definitions[] = {U"roadbed_asphalt",U"roadbed_ballast",U"roadbed_slab"};
		part.defId = definitions[(index+1)%3]; return true;
	}
}
