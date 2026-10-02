#pragma once
#include <Siv3D.hpp>
#include "KeyboardActions.hpp"

/// @brief Offer an exit only after a failed background load has finished.
namespace LoadingRecovery
{
	/// @brief Keep the recovery action below the loading error text.
	inline RectF button(Size size)
	{
		return {size.x * .5 - 140, size.y * .5 + 132, 280, 44};
	}

	/// @brief Never destroy a scene while its background pipeline still owns it.
	inline bool requested(bool failed, bool workerRunning, Size size, Vec2 cursor, bool clicked, bool escape)
	{
		return failed && !workerRunning && (escape || (clicked && button(size).contains(cursor)));
	}

	/// @brief Draw the failure-only recovery action and read its buffered input.
	inline bool draw(const Font& font, Size size, bool failed, bool workerRunning)
	{
		if (!failed || workerRunning) { return false; }
		const auto area = button(size);
		area.rounded(5).draw(area.mouseOver() ? ColorF{.23, .37, .48} : ColorF{.16, .21, .26});
		font(U"タイトルに戻る (Esc)").drawAt(18, area.center(), ColorF{.94});
		return requested(failed, workerRunning, size, Cursor::PosF(), MouseL.down(), GameInput::down(KeyEscape));
	}
}
