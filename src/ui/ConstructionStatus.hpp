#pragma once
#include "../road/RoadConstruction.hpp"
/// @brief Shared, screenshot-tested construction status; coordinates are local to the panel.
namespace ConstructionStatus
{
	/// @brief 標準速度で待つ現実時間を表示する。
	inline String durationLabel(double seconds)
	{
		const int rounded = static_cast<int>(Ceil(Max(0.0, seconds)));
		return rounded < 60 ? U"約{}秒"_fmt(rounded) : U"約{}分"_fmt(static_cast<int>(Ceil(rounded / 60.0)));
	}
	inline void draw(const Font& font, const RectF& area, const RoadConstruction::Progress& progress)
	{
		area.draw(ColorF{0.10,0.12,0.14,.94});
		font(progress.name()).draw(area.x+10,area.y+7,ColorF{1,.84,.48});
		font(U"{:.0f}%"_fmt(progress.total*100)).draw(Arg::topRight=Vec2{area.rightX()-10,area.y+7},ColorF{.9});
		const RectF track{area.x+10,area.y+32,area.w-20,5};
		track.draw(ColorF{.25});
		RectF{track.pos,track.w*progress.total,track.h}.draw(ColorF{.96,.65,.18});
		font(progress.stage == RoadConstruction::Stage::Complete ? U"工事完了・通行できます" : U"工事完了まで通行できません").draw(area.x+10,area.y+44,ColorF{.7});
	}
}
