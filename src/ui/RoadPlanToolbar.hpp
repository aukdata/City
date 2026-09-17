#pragma once
#include <Siv3D.hpp>

/// @brief 道路計画パネルの状態と入力。Test と本体で同じ表示を使う。
namespace RoadPlanToolbar
{
	enum class Action : uint8 { None, Local, Collector, Arterial, OneWay, RailBallast, RailSlab, Tram, Generate, Snap, Undo, Redo, Construct, Clear };
	struct State
	{
		int preset = 0;
		size_t points = 0;
		bool valid = false, canUndo = false, canRedo = false;
		bool generated = false, snapping = true;
		double elevation=0;
		double width = 7.14, length = 0, cost = 0, funds = 0;
		double constructionSeconds = 0;
		String message;
		bool error = false;
	};
	inline constexpr int kHeight = 426;
	Action draw(const Font& font, const Font& bold, int width, const State& state);
}
