#pragma once
#include "TestRunner.hpp"
#include "src/ui/PanelWidget.hpp"
#include "src/asset/AssetRegistrar.hpp"

namespace PanelTextInputTests
{
	/// @brief Isolate physical input and focus while exercising the production widget.
	class InputScope
	{
	public:
		InputScope()
			: m_buffer{ GameInput::buffer }, m_textInput{ GameInput::textInput },
			m_textOwnedFrame{ GameInput::textOwnedFrame }, m_editing{ PanelWidget::textInputEditing }
		{
			GameInput::buffer = KeyboardActionBuffer{};
			GameInput::textInput = nullptr;
			GameInput::textOwnedFrame = false;
			PanelWidget::textInputEditing = PanelWidget::TextInputEditing{};
		}
		~InputScope()
		{
			GameInput::buffer = m_buffer;
			GameInput::textInput = m_textInput;
			GameInput::textOwnedFrame = m_textOwnedFrame;
			PanelWidget::textInputEditing = m_editing;
		}
	private:
		KeyboardActionBuffer m_buffer;
		TextEditState* m_textInput;
		bool m_textOwnedFrame;
		PanelWidget::TextInputEditing m_editing;
	};

	/// @brief Render and process the actual shared panel text input, rather than a copy of its logic.
	[[nodiscard]] inline bool draw(TextEditState& state)
	{
		const Font& font = FontAsset(Asset::CJK14);
		return PanelWidget::textInput(font, state, 20, 20, 280, 24);
	}

	/// @brief Supply a complete physical key tap that never reaches the native raw-text stream.
	inline void tap(const Input& key, uint32 eventIndex)
	{
		GameInput::buffer.update({{0,eventIndex,key.code(),true,false},
			{1,eventIndex + 1,key.code(),false,true}}, true);
	}

	/// @brief Focus a field before its next independently supplied keyboard frame.
	inline void focus(TextEditState& state, size_t cursor)
	{
		GameInput::releaseTextFocus();
		GameInput::buffer.update({}, true);
		GameInput::textInput = &state;
		state.active = true;
		state.cursorPos = cursor;
		(void)draw(state);
	}
}

/// @brief Register shared panel input regressions for route, timetable, and numeric fields.
inline void registerPanelTextInputTests(TestRunner& runner)
{
	runner.add(U"Input.PanelText.ShortBackspaceAndDelete", [](TestContext& context)
	{
		const PanelTextInputTests::InputScope input;
		RegisterAssets();
		const RenderTexture target{Size{320, 80}, TextureFormat::R8G8B8A8_Unorm};
		{
			const ScopedRenderTarget2D render{target};
			TextEditState route{U"国道224号x"};
			PanelTextInputTests::focus(route, route.text.size());
			PanelTextInputTests::tap(KeyBackspace, 0);
			context.expect(PanelTextInputTests::draw(route), U"The production panel reports a short Backspace edit");
			context.expect(route.text == U"国道224号" && route.cursorPos == 6 && route.textChanged,
				U"A complete between-frame Backspace removes the route-name suffix exactly once");
			GameInput::buffer.update({}, true);
			context.expect(!PanelTextInputTests::draw(route) && route.text == U"国道224号",
				U"A later empty keyboard frame does not repeat the edit");
			route.text = U"国道224号x";
			route.cursorPos = 2;
			route.textChanged = false;
			PanelTextInputTests::tap(KeyDelete, 2);
			context.expect(PanelTextInputTests::draw(route), U"The production panel reports a short forward Delete edit");
			context.expect(route.text == U"国道24号x" && route.cursorPos == 2 && route.textChanged,
				U"Delete removes the codepoint after the caret without moving it");
			GameInput::releaseTextFocus();
		}
		Graphics2D::Flush();
	});
	runner.add(U"Input.PanelText.UnicodeAndBounds", [](TestContext& context)
	{
		const PanelTextInputTests::InputScope input;
		RegisterAssets();
		const RenderTexture target{Size{320, 80}, TextureFormat::R8G8B8A8_Unorm};
		{
			const ScopedRenderTarget2D render{target};
			TextEditState route{U"日\U0001F689本"};
			PanelTextInputTests::focus(route, 2);
			PanelTextInputTests::tap(KeyBackspace, 0);
			(void)PanelTextInputTests::draw(route);
			context.expect(route.text == U"日本" && route.cursorPos == 1,
				U"Panel Backspace removes one non-BMP codepoint before an interior caret");
			route.text = U"日本";
			route.cursorPos = 0;
			PanelTextInputTests::tap(KeyBackspace, 2);
			context.expect(!PanelTextInputTests::draw(route) && route.text == U"日本" && route.cursorPos == 0,
				U"Backspace at the start cannot delete text or underflow the caret");
			route.cursorPos = route.text.size();
			PanelTextInputTests::tap(KeyDelete, 4);
			context.expect(!PanelTextInputTests::draw(route) && route.text == U"日本" && route.cursorPos == 2,
				U"Forward Delete at the end stays bounded");
			GameInput::releaseTextFocus();
		}
		Graphics2D::Flush();
	});
	runner.add(U"Input.PanelText.FocusAndInactiveFields", [](TestContext& context)
	{
		const PanelTextInputTests::InputScope input;
		RegisterAssets();
		const RenderTexture target{Size{320, 80}, TextureFormat::R8G8B8A8_Unorm};
		{
			const ScopedRenderTarget2D render{target};
			TextEditState first{U"国道224号x"};
			TextEditState second{U"駅前通り"};
			PanelTextInputTests::focus(first, first.text.size());
			PanelTextInputTests::tap(KeyBackspace, 0);
			context.expect(!PanelTextInputTests::draw(second) && second.text == U"駅前通り",
				U"Drawing an inactive sibling cannot consume or apply the focused field's key");
			(void)PanelTextInputTests::draw(first);
			context.expect(first.text == U"国道224号", U"The focused producer still receives its buffered edit");
			GameInput::releaseTextFocus();
			context.expect(!first.active && !GameInput::down(KeyBackspace),
				U"Releasing focus clears field activity and prevents same-frame gameplay leakage");
			(void)PanelTextInputTests::draw(first);
			PanelTextInputTests::focus(second, second.text.size());
			context.expect(second.text == U"駅前通り", U"Old field input does not replay when another field gains focus");
			PanelTextInputTests::tap(KeyBackspace, 2);
			(void)PanelTextInputTests::draw(second);
			context.expect(second.text == U"駅前通" && first.text == U"国道224号",
				U"A fresh edit after focus transfer affects only the newly focused field");
			GameInput::releaseTextFocus();
		}
		Graphics2D::Flush();
	});
	runner.add(U"Input.PanelText.NativeControlsAreAuthoritative", [](TestContext& context)
	{
		PanelWidget::TextInputEditing editing;
		// These are the states immediately after Siv3D has applied its raw controls.
		TextEditState route{U"国道224号"};
		route.cursorPos = route.text.size();
		editing.update(route, U"\b", false, {1, true, 0}, {});
		context.expect(route.text == U"国道224号" && route.cursorPos == 6,
			U"The shared post-native producer cannot double-apply a native Backspace");
		editing.update(route, U"", false, {0, true, .4}, {});
		context.expect(route.text == U"国道224号", U"A native-owned hold cannot gain fallback repeats");
		editing.update(route, U"", false, {}, {});
		route.text = U"国道24号x";
		route.cursorPos = 2;
		editing.update(route, U"\x7F", false, {}, {1, false, 0});
		context.expect(route.text == U"国道24号x" && route.cursorPos == 2,
			U"The shared post-native producer cannot double-apply a native forward Delete");
		route.text = U"国道24";
		route.cursorPos = route.text.size();
		editing.update(route, U"\b\b", false, {2, false, 0}, {});
		context.expect(route.text == U"国道24" && route.cursorPos == 4,
			U"Multiple native Backspaces remain authoritative over matching physical presses");
	});
	runner.add(U"Input.PanelText.HeldKeysAndFocusReset", [](TestContext& context)
	{
		PanelWidget::TextInputEditing editing;
		TextEditState first{U"国道224号xy"};
		first.cursorPos = first.text.size();
		editing.update(first, U"", false, {1, true, 0}, {});
		context.expect(first.text == U"国道224号x", U"A fresh physical hold begins with one deletion");
		editing.update(first, U"", false, {0, true, .34}, {});
		context.expect(first.text == U"国道224号", U"The focused producer repeats a held key when native controls are missing");
		TextEditState second{U"駅前通り"};
		second.cursorPos = second.text.size();
		editing.update(second, U"", false, {0, true, .6}, {});
		editing.update(second, U"", false, {0, true, .9}, {});
		context.expect(second.text == U"駅前通り", U"Changing fields never transfers an existing deletion hold");
		editing.releaseInactiveFocus(nullptr);
		editing.update(second, U"", false, {0, true, 1.2}, {});
		context.expect(second.text == U"駅前通り", U"Dismissal and refocus cannot resume a stale held key");
		editing.update(second, U"", false, {}, {});
		editing.update(second, U"", false, {1, false, 0}, {});
		context.expect(second.text == U"駅前通", U"A fresh tap after release reaches the new field");
		editing.update(first, U"", false, {1, true, 0}, {}, true);
		editing.update(first, U"", false, {0, true, .5}, {});
		context.expect(first.text == U"国道224号", U"A focus-acquisition click neither replays a same-frame tap nor inherits its hold");
		editing.update(first, U"", false, {}, {});
		first.cursorPos = 2;
		editing.update(first, U"", false, {}, {1, true, 0});
		editing.update(first, U"", false, {}, {0, true, .34});
		context.expect(first.text == U"国道4号" && first.cursorPos == 2,
			U"Fresh forward Delete and its held repeat remain cursor-aware after refocus");
	});
	runner.add(U"Input.PanelText.ImePreservation", [](TestContext& context)
	{
		PanelWidget::TextInputEditing editing;
		TextEditState route{U"国道224号"};
		route.cursorPos = 2;
		editing.update(route, U"", true, {1, true, 0}, {1, true, 0});
		context.expect(route.text == U"国道224号" && route.cursorPos == 2,
			U"Composing input owns physical Backspace and Delete without editing committed text");
		editing.update(route, U"", false, {0, true, .7}, {0, true, .7});
		context.expect(route.text == U"国道224号", U"Ending composition cannot replay either IME-owned held key");
		editing.update(route, U"", false, {}, {});
		editing.update(route, U"", false, {}, {1, false, 0});
		context.expect(route.text == U"国道24号" && route.cursorPos == 2,
			U"A fresh edit works after IME-owned keys are released");
	});
}
