#pragma once
#include "../zone/ZoneManager.hpp"

/// @brief 用途・ブラシ・需要・開発状況をまとめる。本体とTestで同じ部品を描画する。
namespace ZonePalette
{
	inline constexpr int kWidth=366, kHeight=470;
	struct State
	{
		ZoneType zone=ZoneType::LowResidential;
		int brushRadius=1;
		ZoneDevelopmentDemand demand;
		ZoneDevelopmentSummary development;
		bool paused=false;
	};
	struct Action { Optional<ZoneType> zone; Optional<int> brushRadius; };
	[[nodiscard]] Rect zoneButton(int index, int width);
	[[nodiscard]] Rect brushButton(int index, int width);
	[[nodiscard]] StringView description(ZoneType zone);
	Action draw(const Font& font, const Font& bold, int width, const State& state);
}
