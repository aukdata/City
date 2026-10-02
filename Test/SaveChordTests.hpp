#pragma once
#include "TestRunner.hpp"
#include "src/ui/KeyboardActions.hpp"

namespace SaveChordTests
{
	/// @brief Isolate the real input buffer and text-ownership state for each regression.
	class InputScope
	{
	public:
		InputScope()
			: m_buffer{ GameInput::buffer }, m_textInput{ GameInput::textInput },
			m_textOwnedFrame{ GameInput::textOwnedFrame }
		{
			GameInput::buffer = KeyboardActionBuffer{};
			GameInput::textInput = nullptr;
			GameInput::textOwnedFrame = false;
		}
		~InputScope()
		{
			GameInput::buffer = m_buffer;
			GameInput::textInput = m_textInput;
			GameInput::textOwnedFrame = m_textOwnedFrame;
		}
	private:
		KeyboardActionBuffer m_buffer;
		TextEditState* m_textInput;
		bool m_textOwnedFrame;
	};

	/// @brief Exercise the same predicate and S edge used by GameScene::handleGlobalShortcuts.
	[[nodiscard]] inline bool saveRequested(bool pauseMenuVisible = false)
	{
		return GameInput::saveShortcutActive(pauseMenuVisible) && GameInput::down(KeyS);
	}
}

/// @brief Register event-order regressions against the actual game input buffer.
inline void registerSaveChordTests(TestRunner& runner)
{
	runner.add(U"Input.SaveChord.AcrossFrameRelease", [](TestContext& context)
	{
		const SaveChordTests::InputScope input;
		GameInput::buffer.update({{0,0,KeyControl.code(),true,false},
			{1,1,KeyShift.code(),true,false}}, true);
		const Array<KeyEvent> retained{{0,0,KeyControl.code(),true,false},
			{1,1,KeyShift.code(),true,false},{2,2,KeyS.code(),true,false},
			{3,3,KeyS.code(),false,true},{4,4,KeyShift.code(),false,true},
			{5,5,KeyControl.code(),false,true}};
		GameInput::buffer.update(retained, true);
		context.expect(GameInput::down(KeyS), U"The fresh S tap survives the second frame");
		context.expect(!GameInput::buffer.down(KeyControl.code()) && !GameInput::buffer.down(KeyShift.code()),
			U"Prior-frame modifier downs are not ordinary fresh key actions");
		context.expect(SaveChordTests::saveRequested(),
			U"S pressed while prior-frame modifiers are held saves even if all keys release before rendering");
		GameInput::buffer.update(retained, true);
		context.expect(!SaveChordTests::saveRequested(), U"Retained event history cannot save twice");
	});
	runner.add(U"Input.SaveChord.SameBatchTap", [](TestContext& context)
	{
		const SaveChordTests::InputScope input;
		const Array<KeyEvent> chord{{0,0,KeyControl.code(),true,false},
			{1,1,KeyShift.code(),true,false},{2,2,KeyS.code(),true,false},
			{3,3,KeyS.code(),false,true},{4,4,KeyShift.code(),false,true},
			{5,5,KeyControl.code(),false,true}};
		GameInput::buffer.update(chord, true);
		context.expect(SaveChordTests::saveRequested(), U"A complete chord between rendered frames saves once");
		GameInput::buffer.update(chord, true);
		context.expect(!SaveChordTests::saveRequested(), U"A completed same-batch chord cannot replay");
	});
	runner.add(U"Input.SaveChord.HeldAcrossEmptyFrames", [](TestContext& context)
	{
		const SaveChordTests::InputScope input;
		GameInput::buffer.update({{0,0,KeyControl.code(),true,false},
			{1,1,KeyShift.code(),true,false}}, true);
		GameInput::buffer.update({}, true);
		context.expect(!SaveChordTests::saveRequested(), U"Held modifiers alone cannot save");
		GameInput::buffer.update({{2,2,KeyS.code(),true,false},{3,3,KeyS.code(),false,true},
			{4,4,KeyShift.code(),false,true},{5,5,KeyControl.code(),false,true}}, true);
		context.expect(SaveChordTests::saveRequested(), U"Modifier ownership survives frames without new key events");
	});
	runner.add(U"Input.SaveChord.NonOverlappingTaps", [](TestContext& context)
	{
		const SaveChordTests::InputScope input;
		GameInput::buffer.update({{0,0,KeyControl.code(),true,false},{1,1,KeyControl.code(),false,true},
			{2,2,KeyShift.code(),true,false},{3,3,KeyShift.code(),false,true},
			{4,4,KeyS.code(),true,false},{5,5,KeyS.code(),false,true}}, true);
		context.expect(!SaveChordTests::saveRequested(), U"Three separate non-overlapping taps cannot combine into a save");
		context.expect(GameInput::down(KeyControl) && GameInput::down(KeyShift) && GameInput::down(KeyS),
			U"Save recognition does not alter ordinary per-frame buffered key semantics");
	});
	runner.add(U"Input.SaveChord.SBeforeModifiers", [](TestContext& context)
	{
		const SaveChordTests::InputScope input;
		GameInput::buffer.update({{0,0,KeyS.code(),true,false},{1,1,KeyS.code(),false,true},
			{2,2,KeyControl.code(),true,false},{3,3,KeyShift.code(),true,false}}, true);
		context.expect(!SaveChordTests::saveRequested(), U"Modifiers pressed after the S tap cannot retroactively save");
	});
	runner.add(U"Input.SaveChord.FocusReset", [](TestContext& context)
	{
		const SaveChordTests::InputScope input;
		GameInput::buffer.update({{0,0,KeyControl.code(),true,false},{1,1,KeyShift.code(),true,false}}, true);
		GameInput::buffer.update({}, false);
		GameInput::buffer.update({{2,2,KeyS.code(),true,false},{3,3,KeyS.code(),false,true}}, true);
		context.expect(!SaveChordTests::saveRequested(), U"Focus loss clears modifiers even when release events are absent");
		const Array<KeyEvent> unfocused{{4,4,KeyControl.code(),true,false},{5,5,KeyShift.code(),true,false},
			{6,6,KeyS.code(),true,false},{7,7,KeyS.code(),false,true}};
		GameInput::buffer.update(unfocused, false);
		context.expect(!SaveChordTests::saveRequested(), U"An unfocused chord cannot save");
		GameInput::buffer.update(unfocused, true);
		context.expect(!SaveChordTests::saveRequested(), U"Regaining focus cannot replay an unfocused chord");
		GameInput::buffer.update({{8,8,KeyS.code(),true,false},{9,9,KeyS.code(),false,true}}, true);
		context.expect(!SaveChordTests::saveRequested(), U"Unfocused modifier downs cannot arm a later S tap");
	});
	runner.add(U"Input.SaveChord.TextAndPauseOwnership", [](TestContext& context)
	{
		const SaveChordTests::InputScope input;
		const Array<KeyEvent> chord{{0,0,KeyControl.code(),true,false},{1,1,KeyShift.code(),true,false},
			{2,2,KeyS.code(),true,false},{3,3,KeyS.code(),false,true},
			{4,4,KeyShift.code(),false,true},{5,5,KeyControl.code(),false,true}};
		GameInput::buffer.update(chord, true);
		context.expect(!SaveChordTests::saveRequested(true), U"The pause menu retains its own save action");
		TextEditState text;
		GameInput::textInput = &text;
		context.expect(!SaveChordTests::saveRequested(), U"Text input owns the full buffered chord");
		GameInput::releaseTextFocus();
		context.expect(!SaveChordTests::saveRequested(), U"Releasing text focus cannot leak a save in the same frame");
		GameInput::textOwnedFrame = false;
		GameInput::buffer.update(chord, true);
		context.expect(!SaveChordTests::saveRequested(), U"Closing text or pause UI cannot replay a previous chord");
	});
	runner.add(U"Input.SaveChord.LeftAndRightModifiers", [](TestContext& context)
	{
		const SaveChordTests::InputScope input;
		for (const auto control : {KeyLControl, KeyRControl})
		{
			for (const auto shift : {KeyLShift, KeyRShift})
			{
				GameInput::buffer = KeyboardActionBuffer{};
				GameInput::buffer.update({{0,0,control.code(),true,false},{1,1,shift.code(),true,false}}, true);
				GameInput::buffer.update({{2,2,KeyS.code(),true,false},{3,3,KeyS.code(),false,true},
					{4,4,shift.code(),false,true},{5,5,control.code(),false,true}}, true);
				context.expect(SaveChordTests::saveRequested(), U"Either physical Control and Shift side can form a save chord");
			}
		}
		GameInput::buffer = KeyboardActionBuffer{};
		GameInput::buffer.update({{0,0,KeyLControl.code(),true,false},{1,1,KeyControl.code(),true,false},
			{2,2,KeyRControl.code(),true,false},{3,3,KeyControl.code(),true,false},
			{4,4,KeyLShift.code(),true,false},{5,5,KeyShift.code(),true,false},
			{6,6,KeyRShift.code(),true,false},{7,7,KeyShift.code(),true,false}}, true);
		GameInput::buffer.update({{8,8,KeyLControl.code(),false,true},{9,9,KeyControl.code(),false,true},
			{10,10,KeyLShift.code(),false,true},{11,11,KeyShift.code(),false,true},
			{12,12,KeyS.code(),true,false},{13,13,KeyS.code(),false,true}}, true);
		context.expect(SaveChordTests::saveRequested(), U"Releasing left modifiers preserves a chord held with right modifiers");
	});
}
