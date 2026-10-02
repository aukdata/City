#pragma once
#include <Siv3D.hpp>

/// @brief Zoning pointer transitions shared by the live scene and hardware-frame replay tests.
namespace ZonePaintInput
{
	/// @brief One sampled frame; no ground point represents a panel or a missed terrain ray.
	struct Frame
	{
		Optional<Vec3> groundPoint;
		bool shift = false;
		bool down = false;
		bool pressed = false;
		bool up = false;
	};

	/// @brief Route a scene frame through the live scene's outer input gates.
	template <class HandleInput>
	void dispatchFrame(bool enabled, bool pressed,
		Optional<Vec3>& rectangleStart, const HandleInput& handleInput)
	{
		if (enabled)
		{
			handleInput();
		}
		// Valid releases paint in update(); any remaining capture missed that input path.
		if (!pressed)
		{
			rectangleStart = none;
		}
	}

	/// @brief Apply the production brush and rectangle transitions to a sampled input frame.
	template <class PaintBrush, class PaintRectangle>
	void update(Optional<Vec3>& rectangleStart, const Frame& frame,
		const PaintBrush& paintBrush, const PaintRectangle& paintRectangle)
	{
		if (!frame.groundPoint)
		{
			return;
		}
		if (frame.shift)
		{
			if (frame.down)
			{
				rectangleStart = frame.groundPoint;
			}
			if (frame.up && rectangleStart)
			{
				paintRectangle(*rectangleStart, *frame.groundPoint);
				rectangleStart = none;
			}
		}
		else
		{
			rectangleStart = none;
			if (frame.pressed)
			{
				paintBrush(*frame.groundPoint);
			}
		}
	}
}
