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
		for (const auto& event : events)
		{
			if (m_lastEvent && static_cast<int32>(event.eventIndex-*m_lastEvent)<=0) { continue; }
			m_lastEvent=event.eventIndex;
			if (focused && event.down) { m_down[event.code]=true; }
		}
	}
	[[nodiscard]] bool down(uint8 code) const { return m_down[code]; }
private:
	std::array<bool,256> m_down{};
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
}
