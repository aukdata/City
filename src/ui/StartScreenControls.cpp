#include "StartScreenControls.hpp"
#include "PlainLabel.hpp"

namespace StartScreenControls
{
	namespace
	{
		const ColorF text{.9, .94, .97};
		void button(RectF area, StringView label, const Font& font, bool enabled = true, bool danger = false)
		{
			area.rounded(4).draw(
				enabled ? (danger ? ColorF{.38, .16, .18} : ColorF{.17, .27, .35}) : ColorF{.12, .15, .17});
			PlainLabel::fitted(font, label, 14, area.stretched(-7, -4), enabled ? text : ColorF{.4}, ColorF{0, 0});
		}
	} // namespace
	RectF optionBounds(Vec2 origin, size_t index)
	{
		return {origin + Vec2{static_cast<double>(index % 2) * 176, static_cast<double>(index / 2) * 30}, 170, 27};
	}
	void drawOptions(Vec2 origin, const GenerationOptions& options, const Font& font)
	{
		for (size_t i = 0; i < GenerationOptions::kCount; ++i)
		{
			const auto element = static_cast<GenerationOptions::Element>(i);
			const auto area = optionBounds(origin, i);
			const bool available = options.available(element), enabled = options.enabled(element);
			const RectF box{area.pos + Vec2{2, 4}, 18, 18};
			box.draw(ColorF{.12, .18, .23}).drawFrame(1, available ? ColorF{.58, .72, .77} : ColorF{.25});
			if (enabled)
			{
				Line{box.pos + Vec2{4, 9}, box.pos + Vec2{8, 13}}.draw(2, ColorF{.4, .9, .77});
				Line{box.pos + Vec2{8, 13}, box.pos + Vec2{15, 4}}.draw(2, ColorF{.4, .9, .77});
			}
			PlainLabel::fitted(font, GenerationOptions::labels[i], 14, {area.x + 27, area.y + 3, area.w - 27, 23},
				available ? text : ColorF{.4}, ColorF{0, 0});
		}
	}
	bool selectOption(Vec2 origin, GenerationOptions& options, Vec2 cursor, bool clicked)
	{
		if (!clicked)
		{
			return false;
		}
		for (size_t i = 0; i < GenerationOptions::kCount; ++i)
		{
			if (options.available(static_cast<GenerationOptions::Element>(i)) &&
				optionBounds(origin, i).contains(cursor))
			{
				options.selected[i] = !options.selected[i];
				return true;
			}
		}
		return false;
	}
	RectF saveRow(Vec2 origin, int row)
	{
		return {origin + Vec2{0, row * 30.0}, 340, 28};
	}
	RectF loadButton(Vec2 origin)
	{
		return {origin + Vec2{0, 220}, 160, 30};
	}
	RectF deleteButton(Vec2 origin)
	{
		return {origin + Vec2{180, 220}, 160, 30};
	}
	RectF confirmationButton(Vec2 origin, bool confirm)
	{
		return {origin + Vec2{confirm ? 0.0 : 180.0, 295}, 160, 30};
	}
	void drawSaves(Vec2 origin, const SaveList& state, const Font& font)
	{
		for (int row = 0; row < kVisibleSaves; ++row)
		{
			const int index = state.first + row;
			if (index >= static_cast<int>(state.names.size()))
			{
				break;
			}
			const auto area = saveRow(origin, row);
			area.rounded(3).draw(index == state.selected ? ColorF{.18, .36, .43} : ColorF{.10, .15, .20});
			PlainLabel::fitted(font, state.names[index], 14, area.stretched(-7, -4), text, ColorF{0, 0});
		}
		if (state.names.isEmpty())
		{
			font(U"セーブデータがありません").draw(14, origin, ColorF{.6});
		}
		const bool selected = state.selected >= 0 && state.selected < static_cast<int>(state.names.size());
		button(loadButton(origin), U"ロード", font, selected && state.confirming.isEmpty());
		button(deleteButton(origin), U"削除", font, selected && state.confirming.isEmpty(), true);
		if (!state.confirming.isEmpty())
		{
			PlainLabel::fitted(font, U"「{}」を削除しますか？"_fmt(state.confirming), 14,
				{origin + Vec2{0, 262}, 340, 26}, text, ColorF{0, 0});
			button(confirmationButton(origin, true), U"削除する", font, true, true);
			button(confirmationButton(origin, false), U"キャンセル", font);
		}
		if (!state.error.isEmpty())
		{
			PlainLabel::fitted(
				font, state.error, 13, {origin + Vec2{0, 335}, 340, 35}, ColorF{1, .65, .6}, ColorF{0, 0});
		}
		if (state.names.size() > kVisibleSaves)
		{
			font(U"ホイールで一覧をスクロール").draw(12, origin + Vec2{0, 376}, ColorF{.6});
		}
	}
	Action interactSaves(Vec2 origin, SaveList& state, Vec2 cursor, bool clicked, double wheel)
	{
		if (!state.confirming.isEmpty())
		{
			if (clicked && confirmationButton(origin, true).contains(cursor))
			{
				return Action::Delete;
			}
			if (clicked && confirmationButton(origin, false).contains(cursor))
			{
				state.confirming.clear();
			}
			return Action::None;
		}
		if (RectF{origin, 340, 210}.contains(cursor))
		{
			state.first = Clamp(
				state.first + static_cast<int>(wheel), 0, Max(0, static_cast<int>(state.names.size()) - kVisibleSaves));
			if (clicked)
			{
				for (int row = 0; row < kVisibleSaves; ++row)
				{
					if (saveRow(origin, row).contains(cursor) &&
						state.first + row < static_cast<int>(state.names.size()))
					{
						state.selected = state.first + row;
					}
				}
			}
		}
		const bool selected = state.selected >= 0 && state.selected < static_cast<int>(state.names.size());
		if (clicked && selected)
		{
			if (loadButton(origin).contains(cursor))
			{
				return Action::Load;
			}
			if (deleteButton(origin).contains(cursor))
			{
				state.confirming = state.names[state.selected];
				state.error.clear();
			}
		}
		return Action::None;
	}
	RectF layerButton(Size size, bool fpsVisible)
	{
		return {12, Max(12, size.y - (fpsVisible ? 218 : 80)), 108, 28};
	}
	void drawLayerButton(Size size, bool underground, const Font& font, bool fpsVisible)
	{
		button(layerButton(size, fpsVisible), underground ? U"地下  U" : U"地上  U", font);
	}
} // namespace StartScreenControls
