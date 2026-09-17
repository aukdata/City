#pragma once
#include <Siv3D.hpp>

/// @brief 視界を広く残した右ハンドル車の運転席表示。
namespace DrivingHud
{
	struct State { double speed=0,steering=0,distance=0;float limit=0;bool paused=false,blocked=false; };
	[[nodiscard]] RectF instrumentBounds(Size size);
	void draw(const Font& font,Size size,const State& state);
}
