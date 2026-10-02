#pragma once
#include "TestRunner.hpp"
#include "src/ui/ZonePaintInput.hpp"

namespace ZonePaintInputTests
{
	/// @brief Replay live pointer gates and record the production handler's painting effects.
	struct Replay
	{
		Optional<Vec3> rectangleStart;
		Array<Vec3> brushes;
		Array<std::pair<Vec3, Vec3>> rectangles;

		void input(const ZonePaintInput::Frame& frame, bool enabled = true, bool returnEarly = false)
		{
			ZonePaintInput::dispatchFrame(enabled, frame.pressed, rectangleStart, [&]
			{
				if (returnEarly)
				{
					return;
				}
				ZonePaintInput::update(rectangleStart, frame,
					[&](Vec3 point) { brushes << point; },
					[&](Vec3 start, Vec3 end) { rectangles << std::pair{start, end}; });
			});
		}
	};

	/// @brief Register permanent brush/rectangle interruption regressions with the zoning suite.
	inline void registerTests(TestRunner& runner)
	{
		runner.add(U"Zoning.RectangleInput.ValidRectangleAndBrushReplay", [](TestContext& context)
		{
			Replay replay;
			const Vec3 start{504,10,520}, end{552,10,568};
			replay.input({start, true, true, true, false});
			replay.input({end, true, false, true, false});
			context.expect(replay.rectangleStart && *replay.rectangleStart == start && replay.rectangles.isEmpty(),
				U"A held Shift rectangle preserves its first point without painting");
			replay.input({end, true, false, false, true});
			context.expect(replay.rectangles.size() == 1 && replay.rectangles.front() == std::pair{start, end},
				U"A world release paints exactly one rectangle with the original endpoints");
			context.expect(!replay.rectangleStart && replay.brushes.isEmpty(), U"A valid rectangle never leaks an ordinary brush");
			replay.input({start, false, true, true, false});
			replay.input({end, false, false, true, false});
			replay.input({end, false, false, false, true});
			context.expect(replay.brushes == Array<Vec3>{start, end}, U"The ordinary brush paints held frames, but not release");
			replay.input({start, true, true, false, true});
			context.expect(replay.rectangles.size() == 2 && replay.rectangles.back() == std::pair{start, start}
				&& !replay.rectangleStart, U"A same-frame Shift press and release paints a single-cell rectangle without capture");
		});
		runner.add(U"Zoning.RectangleInput.BlockedReleaseCancelsRectangleReplay", [](TestContext& context)
		{
			// 0: panel/no terrain; 1: minimap disables dispatch; 2: HUD/header/text returns early.
			for (int gate = 0; gate < 3; ++gate)
			{
				Replay replay;
				replay.input({Vec3{504,10,520}, true, true, true, false});
				replay.input({none, true, false, false, true}, gate != 1, gate == 2);
				context.expect(!replay.rectangleStart, U"A release blocked by terrain/UI/outer gates clears the rectangle capture");
				replay.input({Vec3{680,10,680}, true, false, false, false});
				context.expect(!replay.rectangleStart && replay.rectangles.isEmpty() && replay.brushes.isEmpty(),
					U"Returning to the world after a blocked release leaves no stale preview or paint");
			}
		});
		runner.add(U"Zoning.RectangleInput.UiPressCannotReuseCancelledRectangleReplay", [](TestContext& context)
		{
			for (int gate = 0; gate < 3; ++gate)
			{
				Replay replay;
				replay.input({Vec3{504,10,520}, true, true, true, false});
				replay.input({none, true, false, false, true}, gate != 1, gate == 2);
				replay.input({none, true, true, true, false}, gate != 1, gate == 2);
				replay.input({Vec3{680,10,680}, true, false, false, true});
				context.expect(replay.rectangles.isEmpty() && replay.brushes.isEmpty(),
					U"A later UI press and world release cannot paint from an earlier cancelled anchor");
				context.expect(!replay.rectangleStart, U"The unowned release leaves no pending rectangle");
			}
		});
		runner.add(U"Zoning.RectangleInput.MissedReleaseEdgeClearsRectangleReplay", [](TestContext& context)
		{
			Replay replay;
			replay.input({Vec3{504,10,520}, true, true, true, false});
			replay.input({Vec3{680,10,680}, true, false, false, false});
			context.expect(!replay.rectangleStart && replay.rectangles.isEmpty(),
				U"An idle pointer without a sampled release edge cancels capture rather than painting");
		});
		runner.add(U"Zoning.RectangleInput.HeldRectangleCrossesUiReplay", [](TestContext& context)
		{
			Replay replay;
			const Vec3 start{504,10,520}, end{552,10,568};
			replay.input({start, true, true, true, false});
			replay.input({none, true, false, true, false});
			replay.input({none, true, false, true, false}, false);
			replay.input({none, true, false, true, false}, true, true);
			context.expect(replay.rectangleStart && *replay.rectangleStart == start,
				U"Crossing UI while still held preserves the world-owned anchor");
			replay.input({end, true, false, true, false});
			replay.input({end, true, false, false, true});
			context.expect(replay.rectangles.size() == 1 && replay.rectangles.front() == std::pair{start, end}
				&& replay.brushes.isEmpty(), U"Returning while held permits a later valid world release");
		});
		runner.add(U"Zoning.RectangleInput.ShiftReleaseAndExplicitResetReplay", [](TestContext& context)
		{
			Replay replay;
			const Vec3 start{504,10,520}, end{552,10,568};
			replay.input({start, true, true, true, false});
			replay.input({end, false, false, true, false});
			context.expect(!replay.rectangleStart && replay.brushes == Array<Vec3>{end} && replay.rectangles.isEmpty(),
				U"Releasing Shift while still held preserves the existing ordinary-brush transition");
			replay.brushes.clear();
			replay.input({start, true, true, true, false});
			// Esc and tool changes already explicitly reset GameScene's rectangle state.
			replay.rectangleStart = none;
			replay.input({end, true, false, true, false});
			replay.input({end, true, false, false, true});
			context.expect(!replay.rectangleStart && replay.rectangles.isEmpty() && replay.brushes.isEmpty(),
				U"Held movement and release after an explicit tool reset cannot recreate a rectangle");
		});
	}
}
