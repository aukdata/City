#pragma once
#include <Siv3D.hpp>

/// @brief A frame's physical edit-key state, independent of text/IME input.
struct BufferedTextEditKey
{
	size_t presses = 0;  ///< Fresh physical presses, including complete between-frame taps.
	bool held = false;
	double heldSeconds = 0;
};

/// @brief Fill missing Backspace/Delete text controls without duplicating native text edits.
/// @details Call after TextInput::UpdateText. Native raw controls, including repeats, remain authoritative.
class BufferedTextEdit
{
public:
	/// @brief Apply only edit controls absent from the native raw input for this frame.
	[[nodiscard]] size_t update(String& text, size_t cursor, StringView raw, bool composing,
		const BufferedTextEditKey& backspace, const BufferedTextEditKey& forwardDelete)
	{
		cursor = Min(cursor, text.size());
		const size_t eraseBefore = m_backspace.count(backspace, contains(raw, U'\b'), composing);
		const size_t eraseAfter = m_forwardDelete.count(forwardDelete, contains(raw, U'\x7F'), composing);
		const size_t before = Min(eraseBefore, cursor);
		if (before > 0)
		{
			text.erase(cursor - before, before);
			cursor -= before;
		}
		const size_t after = Min(eraseAfter, text.size() - cursor);
		if (after > 0) { text.erase(cursor, after); }
		return cursor;
	}

private:
	/// @brief Retain held-key repeat timing without replaying keys used by an IME.
	struct RepeatState
	{
		double lastRepeatSeconds = 0;
		bool imeOwned = false;
		bool nativeHold = false;

		[[nodiscard]] size_t count(const BufferedTextEditKey& key, bool nativeControl, bool composing)
		{
			if (!key.held || key.presses > 0)
			{
				lastRepeatSeconds = 0;
				imeOwned = false;
				nativeHold = false;
			}
			if (composing)
			{
				lastRepeatSeconds = key.heldSeconds;
				imeOwned = key.held;
				return 0;
			}
			if (imeOwned) { return 0; }
			if (nativeControl)
			{
				nativeHold = key.held;
				lastRepeatSeconds = key.heldSeconds;
				return 0;
			}
			if (nativeHold) { return 0; }
			if (key.presses > 0)
			{
				lastRepeatSeconds = key.heldSeconds;
				return key.presses;
			}
			constexpr double kRepeatDelaySeconds = 0.33;
			constexpr double kRepeatIntervalSeconds = 0.06;
			if (key.held && key.heldSeconds > kRepeatDelaySeconds
				&& key.heldSeconds - lastRepeatSeconds > kRepeatIntervalSeconds)
			{
				lastRepeatSeconds = key.heldSeconds;
				return 1;
			}
			return 0;
		}
	};

	[[nodiscard]] static bool contains(StringView raw, char32 control)
	{
		for (const char32 character : raw)
		{
			if (character == control) { return true; }
		}
		return false;
	}

	RepeatState m_backspace, m_forwardDelete;
};
