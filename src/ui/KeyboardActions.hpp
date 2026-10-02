#pragma once
#include <Siv3D.hpp>
#if SIV3D_PLATFORM(LINUX)
namespace s3d::Platform::Linux::Keyboard { Array<KeyEvent> GetEvents(); }
#endif

/// @brief Preserve short key taps that begin and end between two rendered frames.
class KeyboardActionBuffer
{
public:
	void update(const Array<KeyEvent>& events, bool focused)
	{
		m_down.fill(false);
		m_editPresses.fill(0);
		m_saveShortcutDown = false;
		if (!focused)
		{
			m_controlHeld.fill(false);
			m_shiftHeld.fill(false);
		}
		for (const auto& event : events)
		{
			if (m_lastEvent && static_cast<int32>(event.eventIndex-*m_lastEvent)<=0) { continue; }
			m_lastEvent=event.eventIndex;
			if (focused) { updateSaveShortcut(event); }
			if (focused && event.down)
			{
				m_down[event.code] = true;
				if (event.code == KeyBackspace.code()) { ++m_editPresses[0]; }
				if (event.code == KeyDelete.code()) { ++m_editPresses[1]; }
			}
		}
	}
	[[nodiscard]] bool down(uint8 code) const { return m_down[code]; }
	/// @brief A fresh S press that occurred while Control and Shift were held, even across frame boundaries.
	[[nodiscard]] bool saveShortcutDown() const { return m_saveShortcutDown; }
	/// @brief Count fresh edit-key taps; gameplay actions keep their existing one-per-frame semantics.
	[[nodiscard]] size_t editPressCount(uint8 code) const
	{
		if (code == KeyBackspace.code()) { return m_editPresses[0]; }
		if (code == KeyDelete.code()) { return m_editPresses[1]; }
		return 0;
	}
private:
	/// @brief Track generic and physical modifier events without changing ordinary buffered key actions.
	static void updateModifierHeld(const KeyEvent& event, const Input& generic, const Input& left,
		const Input& right, std::array<bool,3>& held)
	{
		const std::array<uint8,3> codes{ generic.code(), left.code(), right.code() };
		for (size_t i = 0; i < codes.size(); ++i)
		{
			if (event.code != codes[i]) { continue; }
			if (event.down) { held[i] = true; }
			if (event.up) { held[i] = false; }
		}
	}
	void updateSaveShortcut(const KeyEvent& event)
	{
		updateModifierHeld(event, KeyControl, KeyLControl, KeyRControl, m_controlHeld);
		updateModifierHeld(event, KeyShift, KeyLShift, KeyRShift, m_shiftHeld);
		if (event.code == KeyS.code() && event.down
			&& (m_controlHeld[0] || m_controlHeld[1] || m_controlHeld[2])
			&& (m_shiftHeld[0] || m_shiftHeld[1] || m_shiftHeld[2]))
		{
			m_saveShortcutDown = true;
		}
	}
	std::array<bool,256> m_down{};
	std::array<size_t,2> m_editPresses{};
	std::array<bool,3> m_controlHeld{};
	std::array<bool,3> m_shiftHeld{};
	bool m_saveShortcutDown = false;
	Optional<uint32> m_lastEvent;
};

/// @brief Frame-local action inputs. Text entry continues to use Siv3D/IME input.
namespace GameInput
{
	inline KeyboardActionBuffer buffer;
	inline TextEditState* textInput = nullptr;
	inline bool textOwnedFrame = false;
	inline bool keyboardBlocked() { return textInput != nullptr || textOwnedFrame; }
	inline void releaseTextFocus()
	{
		if (textInput) { textInput->active = false; }
		textInput = nullptr;
		textOwnedFrame = true;
	}
	inline void beginFrame()
	{
		textOwnedFrame = textInput != nullptr || !TextInput::GetEditingText().isEmpty();
		#if SIV3D_PLATFORM(WINDOWS)
		buffer.update(Platform::Windows::Keyboard::GetEvents(),Window::GetState().focused);
#else
		buffer.update(Platform::Linux::Keyboard::GetEvents(),Window::GetState().focused);
#endif
	}
	inline bool down(const Input& key) { return !keyboardBlocked() && (key.down() || buffer.down(key.code())); }
	inline bool pressed(const Input& key) { return !keyboardBlocked() && (key.pressed() || buffer.down(key.code())); }
	inline bool down(const InputGroup& keys) { return !keyboardBlocked() && keys.down(); }
	inline bool pressed(const InputGroup& keys) { return !keyboardBlocked() && keys.pressed(); }
	/// @brief Saving is available in every view while text entry and the pause menu retain focus.
	inline bool saveShortcutActive(bool pauseMenuVisible)
	{
		if (pauseMenuVisible || keyboardBlocked()) { return false; }
		// A fresh buffered S owns its event-time modifier decision. Native held state
		// must not turn earlier, unrelated taps into a chord later in the same frame.
		return buffer.saveShortcutDown() || (!buffer.down(KeyS.code())
			&& KeyControl.pressed() && KeyShift.pressed() && KeyS.pressed());
	}
}
