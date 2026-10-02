#pragma once
#include <Siv3D.hpp>
#include "AppSettings.hpp"
#include "KeyboardActions.hpp"
#include "BufferedTextEdit.hpp"

/// @brief Shared title/pause settings editor. Changes remain a draft until Apply succeeds.
class SettingsPanel
{
public:
	enum class Action { None, Apply, Cancel };
	bool visible = false;
	AppSettings draft;
	TextEditState distanceText;
	String error;

	/// @brief Opening always discards any previous uncommitted edit.
	void open(const AppSettings& current)
	{
		draft = current;
		distanceText = TextEditState{};
		m_textEdit = BufferedTextEdit{};
		distanceText.text = Format(current.renderDistance);
		error.clear();
		visible = true;
	}
	void close() { distanceText.active = false; m_textEdit = BufferedTextEdit{}; visible = false; error.clear(); }
	void defaults() { draft = AppSettings{}; distanceText.text = U"0"; distanceText.cursorPos = 1; m_textEdit = BufferedTextEdit{}; error.clear(); }
	[[nodiscard]] Optional<AppSettings> value() const
	{
		const auto distance = RenderDistance::parse(distanceText.text);
		if (!distance) { return none; }
		auto result = draft;
		result.renderDistance = *distance;
		return result.valid() ? Optional<AppSettings>{result} : none;
	}
	static RectF panel(Size size) { return {(size.x - 640) * .5, (size.y - 520) * .5, 640, 520}; }
	/// @brief Stable geometry shared by rendering, pointer interaction and tests.
	static RectF button(Size size, int index)
	{
		const auto p = panel(size);
		if (index < 2) { return {p.x + 190 + index * 180, p.y + 88, 164, 42}; }
		return {p.x + 28 + (index - 2) * 198, p.y + 444, 188, 44};
	}
	Action interact(Size size, Vec2 cursor, bool clicked, bool escape = false)
	{
		if (!visible) { return Action::None; }
		if (escape) { close(); return Action::Cancel; }
		if (!clicked) { return Action::None; }
		for (int index = 0; index < 5; ++index)
		{
			if (!button(size, index).contains(cursor)) { continue; }
			if (index < 2) { draft.lowSpec = index == 1; }
			else if (index == 2) { defaults(); }
			else if (index == 3) { close(); return Action::Cancel; }
			else if (value()) { error.clear(); return Action::Apply; }
			else { error = U"描画距離は 0 または 100〜20000 m を入力してください"; }
		}
		return Action::None;
	}
	Action draw(const Font& font, Size size)
	{
		if (!visible) { return Action::None; }
		const bool escape = KeyEscape.down() || GameInput::buffer.down(KeyEscape.code());
		const auto action = interact(size, Cursor::PosF(), MouseL.down(), escape);
		if (!visible) { return action; }
		RectF{size}.draw(ColorF{0, .65});
		const auto p = panel(size);
		p.rounded(10).draw(ColorF{.10, .13, .17, .99}).drawFrame(1, ColorF{.4, .48, .56});
		font(U"設定").drawAt(28, Vec2{p.center().x, p.y + 35}, ColorF{.95});
		font(U"描画品質").draw(20, Vec2{p.x + 28, p.y + 98}, ColorF{.93});
		const std::array<String, 5> labels{U"標準", U"軽量", U"初期値に戻す", U"キャンセル", U"適用して戻る"};
		for (int index = 0; index < 5; ++index)
		{
			const auto b = button(size, index);
			const bool selected = index < 2 && (draft.lowSpec == (index == 1));
			b.rounded(5).draw(selected || b.mouseOver() ? ColorF{.23, .40, .52} : ColorF{.16, .21, .26});
			if (selected) { b.rounded(5).drawFrame(2, ColorF{.5, .78, .9}); }
			font(labels[index]).drawAt(18, b.center(), ColorF{.95});
		}
		font(U"標準：なめらかな輪郭と影 / 軽量：3D の解像度と影を調整").draw(15, Vec2{p.x + 28, p.y + 148}, ColorF{.72, .8, .85});
		font(U"文字・地図・操作の位置は、どちらも変わりません").draw(15, Vec2{p.x + 28, p.y + 172}, ColorF{.72, .8, .85});
		font(U"描画距離").draw(20, Vec2{p.x + 28, p.y + 214}, ColorF{.93});
		const bool editingDistance = distanceText.active;
		const String rawInput = TextInput::GetRawInput();
		const bool composing = !TextInput::GetEditingText().isEmpty();
		SimpleGUI::TextBox(distanceText, {p.x + 190, p.y + 205}, 240, 12);
		if (editingDistance && distanceText.active)
		{
			const String beforeFallback = distanceText.text;
			const auto editKey = [](const Input& key)
			{
				return BufferedTextEditKey{Max(GameInput::buffer.editPressCount(key.code()), key.down() ? size_t{1} : size_t{0}),
					key.pressed(), key.pressedDuration().count()};
			};
			distanceText.cursorPos = m_textEdit.update(distanceText.text, distanceText.cursorPos, rawInput, composing,
				editKey(KeyBackspace), editKey(KeyDelete));
			if (distanceText.text != beforeFallback)
			{
				distanceText.textChanged = true;
				distanceText.cursorStopwatch.restart();
			}
		}
		else { m_textEdit = BufferedTextEdit{}; }
		font(U"m").draw(18, Vec2{p.x + 444, p.y + 214}, ColorF{.93});
		font(U"建物・木の表示範囲。100〜20000 m / 0：追加制限なし").draw(15, Vec2{p.x + 28, p.y + 261}, ColorF{.72, .8, .85});
		font(U"地形・道路の表示範囲には影響しません").draw(15, Vec2{p.x + 28, p.y + 285}, ColorF{.72, .8, .85});
		font(U"効果音").draw(20, Vec2{p.x + 28, p.y + 333}, ColorF{.93});
		SimpleGUI::Slider(draft.effectVolume, 0.0, 1.0, {p.x + 190, p.y + 327}, 260);
		font(U"{}%"_fmt(Round(draft.effectVolume * 100))).draw(18, Vec2{p.x + 470, p.y + 333}, ColorF{.93});
		font(error.isEmpty() ? U"適用すると保存され、次回起動時も使われます。Esc：キャンセル" : error)
			.draw(15, Vec2{p.x + 28, p.y + 390}, error.isEmpty() ? ColorF{.72, .8, .85} : ColorF{1, .55, .4});
		return action;
	}
private:
	BufferedTextEdit m_textEdit;
};
