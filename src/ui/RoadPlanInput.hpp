#pragma once
#include "../road/RoadPlanDraft.hpp"

/// @brief Road-plan pointer/history transitions shared by the live scene and input replay tests.
namespace RoadPlanInput
{
	/// @brief Keyboard and right-click commands take precedence over pointer movement.
	enum class History : uint8 { None, Undo, Redo };
	/// @brief Effects the scene must apply after processing an input frame.
	enum class Result : uint8 { None, Placed, PlacementRejected, Changed };

	/// @brief One sampled input frame; an absent ground point means a UI surface or no ground hit.
	struct Frame
	{
		Optional<Vec3> groundPoint; ///< Elevation and connection snapping are already applied.
		Vec2 screenCursor{0,0}; ///< Cursor position in the same coordinates as project().
		bool down = false; ///< Fresh left-button press.
		bool pressed = false; ///< Left button is still held in this frame.
		History history = History::None;
	};

	/// @brief Route a scene frame through the same input gates used by the live game.
	template <class HandleInput>
	void dispatchFrame(bool enabled, bool pressed,
		Optional<size_t>& draggedPoint, Array<Vec3>& dragPoints,
		const HandleInput& handleInput)
	{
		if (enabled)
		{
			handleInput();
		}
		// Normal releases commit in update(); a capture left behind was blocked by an outer gate.
		if (!pressed && draggedPoint)
		{
			draggedPoint = none;
			dragPoints.clear();
		}
	}

	/// @brief Replay the actual scene transitions without requiring a window or hardware input.
	/// @param project World-to-screen projection, including the scene's handle-height offset.
	template <class Project>
	Result update(RoadPlanDraft& draft, Optional<size_t>& draggedPoint, Array<Vec3>& dragPoints,
		const Frame& frame, const Project& project)
	{
		if (frame.history != History::None)
		{
			draggedPoint = none;
			dragPoints.clear();
			const bool changed = frame.history == History::Redo ? draft.redo() : draft.undo();
			return changed ? Result::Changed : Result::None;
		}
		if (draggedPoint)
		{
			if (frame.groundPoint)
			{
				dragPoints[*draggedPoint] = *frame.groundPoint;
			}
			if (!frame.pressed)
			{
				const bool changed = frame.groundPoint && draft.revise(std::move(dragPoints));
				draggedPoint = none;
				dragPoints.clear();
				return changed ? Result::Changed : Result::None;
			}
			return Result::None;
		}
		if (!frame.groundPoint || !frame.down)
		{
			return Result::None;
		}

		const auto& points = draft.points();
		constexpr double kHandleRadius = 12;
		Optional<size_t> selected;
		double nearest = kHandleRadius;
		for (size_t index = 0; index < points.size(); ++index)
		{
			const double distance = project(points[index]).distanceFrom(frame.screenCursor);
			if (distance < nearest)
			{
				selected = index;
				nearest = distance;
			}
		}
		Array<Vec3> edited = points;
		if (!selected && draft.generated())
		{
			// Pick the curve itself and insert a new adjustment point.
			constexpr int kCurveSamples = 32;
			size_t index = 0;
			for (const auto& edge : draft.preview().edges())
			{
				if (edge.id < 0)
				{
					continue;
				}
				const auto curve = draft.preview().getBezier(edge.id);
				++index;
				if (!curve)
				{
					continue;
				}
				for (int sample = 1; sample < kCurveSamples; ++sample)
				{
					const Vec3 point = curve->evaluate(static_cast<float>(sample) / kCurveSamples);
					const double distance = project(point).distanceFrom(frame.screenCursor);
					if (distance < nearest)
					{
						selected = index;
						nearest = distance;
					}
				}
			}
			if (selected)
			{
				edited.insert(edited.begin() + *selected, *frame.groundPoint);
			}
		}
		if (selected)
		{
			draggedPoint = selected;
			dragPoints = std::move(edited);
			return Result::None;
		}
		constexpr size_t kEndpointCount = 2;
		if (points.size() < kEndpointCount)
		{
			return draft.place(*frame.groundPoint) ? Result::Placed : Result::PlacementRejected;
		}
		return Result::None;
	}
}
