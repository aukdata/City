#pragma once
#include <Siv3D.hpp>

/// @brief パネル内 UI ウィジェット（即時モード描画）
/// @details 各関数はパネルの beginContent() ～ reportContentHeight() 間で呼ぶ。
///   座標はパネル内ローカル座標。flushTooltip() をパネル末尾で呼ぶとツールチップが最前面に描画される。
namespace PanelWidget
{
	// ── ツールチップ（遅延描画） ──

	inline const Font* tipFont   = nullptr;
	inline String      tipText;
	inline Vec2        tipPos{ 0, 0 };
	inline bool        tipActive = false;

	inline void flushTooltip()
	{
		if (!tipActive || !tipFont) return;
		const auto region = (*tipFont)(tipText).region(tipPos);
		RectF{ region.x - 3, region.y - 1, region.w + 6, region.h + 2 }
			.draw(ColorF{ 0.1, 0.1, 0.1, 0.95 });
		(*tipFont)(tipText).draw(tipPos, Palette::White);
		tipActive = false;
	}

	// ── 共通: hover / click / wheel 判定 + ツールチップ登録 ──

	struct HitResult { bool hover; bool clickL; bool clickR; int wheel; };

	inline HitResult hitTest(const Font& font, int x, int y, int w, int h, StringView tooltip = U"")
	{
		const RectF r{ static_cast<double>(x), static_cast<double>(y),
		               static_cast<double>(w), static_cast<double>(h) };
		const bool hover = r.mouseOver();
		if (hover && !tooltip.isEmpty())
		{
			tipFont   = &font;
			tipText   = String{ tooltip };
			tipPos    = Vec2{ static_cast<double>(x + w + 4), static_cast<double>(y) };
			tipActive = true;
		}
		return { hover, hover && MouseL.down(), hover && MouseR.down(),
		         hover ? static_cast<int>(Mouse::Wheel()) : 0 };
	}

	// ── ボタン: 左クリックで true ──

	inline bool button(const Font& font, StringView label, bool active,
	                   int x, int y, int w, int h, StringView tooltip = U"")
	{
		const auto hit = hitTest(font, x, y, w, h, tooltip);
		RectF{ static_cast<double>(x), static_cast<double>(y),
		       static_cast<double>(w), static_cast<double>(h) }
			.draw(active ? ColorF{ 0.3, 0.5, 0.8 }
			      : (hit.hover ? ColorF{ 0.3, 0.3, 0.4 } : ColorF{ 0.15, 0.15, 0.2 }));
		font(label).draw(Vec2{ x + 2, y }, active ? ColorF{ 1.0 } : ColorF{ 0.7 });
		return hit.clickL;
	}

	// ── 危険操作ボタン（削除など）: 赤系配色 ──

	inline bool buttonDanger(const Font& font, StringView label,
	                         int x, int y, int w, int h, StringView tooltip = U"")
	{
		const auto hit = hitTest(font, x, y, w, h, tooltip);
		RectF{ static_cast<double>(x), static_cast<double>(y),
		       static_cast<double>(w), static_cast<double>(h) }
			.draw(hit.hover ? ColorF{ 0.6, 0.15, 0.15 } : ColorF{ 0.4, 0.1, 0.1 });
		font(label).draw(Vec2{ x + 2, y }, ColorF{ 1.0, 0.7, 0.7 });
		return hit.clickL;
	}

	// ── トグル: クリックで bool 反転。変化したら true ──

	inline bool toggle(const Font& font, StringView labelOn, StringView labelOff, bool& value,
	                   int x, int y, int w, int h, StringView tooltip = U"")
	{
		if (button(font, value ? labelOn : labelOff, value, x, y, w, h, tooltip))
		{
			value = !value;
			return true;
		}
		return false;
	}

	// ── 列挙サイクル: L クリック→次、R クリック→前。変化したら true ──

	template <typename E>
	bool cycle(const Font& font, E& value, const StringView* names, int count,
	           int x, int y, int w, int h, StringView tooltip = U"")
	{
		const auto hit = hitTest(font, x, y, w, h, tooltip);
		RectF{ static_cast<double>(x), static_cast<double>(y),
		       static_cast<double>(w), static_cast<double>(h) }
			.draw(hit.hover ? ColorF{ 0.3, 0.3, 0.4 } : ColorF{ 0.15, 0.15, 0.2 });
		font(names[static_cast<int>(value)]).draw(Vec2{ x + 2, y }, ColorF{ 0.7 });
		const int dir = hit.clickL ? 1 : (hit.clickR ? -1 : 0);
		if (dir != 0)
		{
			value = static_cast<E>((static_cast<int>(value) + count + dir) % count);
			return true;
		}
		return false;
	}

	// ── テキスト入力（コンパクト・IME 対応） ──
	// SimpleGUI::TextBox は大きすぎるため独自実装。
	// activeId: 現在フォーカス中のウィジェットを追跡するグローバルポインタ。
	// クリックでフォーカス取得、外部クリックでフォーカス喪失。

	inline TextEditState* activeTextInput = nullptr;

	inline bool textInput(const Font& font, TextEditState& state,
	                       int x, int y, int w, int h, size_t maxChars = 32)
	{
		const RectF rect{ static_cast<double>(x), static_cast<double>(y),
		                  static_cast<double>(w), static_cast<double>(h) };
		const bool isActive = (activeTextInput == &state);
		const bool hover    = rect.mouseOver();
		bool changed = false;

		// 背景
		rect.draw(isActive ? ColorF{ 0.15, 0.15, 0.25 } : (hover ? ColorF{ 0.18, 0.18, 0.24 } : ColorF{ 0.10, 0.10, 0.16 }));
		rect.drawFrame(1.0, 0.0, isActive ? ColorF{ 0.5, 0.6, 1.0 } : ColorF{ 0.25 });

		// クリックでフォーカス
		if (hover && MouseL.down())
		{
			activeTextInput = &state;
			state.active = true;
			state.cursorPos = state.text.size();
		}
		// 外部クリックでフォーカス喪失
		if (isActive && MouseL.down() && !hover)
		{
			activeTextInput = nullptr;
			state.active = false;
			state.textChanged = true;
		}

		// テキスト入力処理（フォーカス中のみ）
		if (isActive)
		{
			const size_t prevLen = state.text.size();
			state.cursorPos = TextInput::UpdateText(state.text, state.cursorPos);
			if (maxChars > 0 && state.text.size() > maxChars)
				state.text = state.text.substr(0, maxChars);
			if (state.text.size() != prevLen) { changed = true; state.textChanged = true; }

			// Enter で確定
			if (KeyEnter.down())
			{
				activeTextInput = nullptr;
				state.active = false;
				state.textChanged = true;
			}
		}

		// テキスト描画
		const double textX = x + 3.0;
		const double textY = y + 1.0;
		font(state.text).draw(Vec2{ textX, textY }, ColorF{ 0.9 });

		// IME 変換中テキスト（下線付きで表示）
		if (isActive)
		{
			const String editing = TextInput::GetEditingText();
			if (!editing.isEmpty())
			{
				const double curX = textX + font(state.text.substr(0, state.cursorPos)).region().w;
				font(editing).draw(Vec2{ curX, textY }, ColorF{ 0.7, 0.9, 1.0 });
				const double editW = font(editing).region().w;
				Line{ curX, textY + h - 2.0, curX + editW, textY + h - 2.0 }
					.draw(1.0, ColorF{ 0.7, 0.9, 1.0 });
			}

			// カーソル描画（点滅）
			if (static_cast<int>(Scene::Time() * 2) % 2 == 0)
			{
				const double curX = textX + font(state.text.substr(0, state.cursorPos)).region().w;
				Line{ curX, textY + 1.0, curX, textY + h - 3.0 }
					.draw(1.0, ColorF{ 1.0, 1.0, 1.0, 0.8 });
			}
		}

		return changed;
	}

	// ── 数値入力: ホイールで増減 + クリックでテキスト編集。変化したら true ──
	// 状態は &value をキーにした内部キャッシュで自動管理される。
	// hover + ホイール: step 単位で増減 / クリック: テキスト編集モードへ移行
	// Enter または外部クリックで確定（パース失敗時は元の値へ戻す）。

	inline HashTable<uint64, TextEditState> numberInputStates;

	/// @tparam T float または double
	template <class T>
	inline bool numberInput(const Font& font, T& value, T step, T lo, T hi,
	                        int x, int y, int w, int h, StringView fmt = U"{:.1f}")
	{
		static_assert(std::is_arithmetic_v<T>, "numberInput requires arithmetic T");

		const uint64 id = static_cast<uint64>(reinterpret_cast<uintptr_t>(&value));
		TextEditState& state = numberInputStates[id];
		const bool wasActive = (activeTextInput == &state);

		// 非編集中は value → text を同期（外部から value が変わっても追従）
		if (!wasActive && !state.textChanged)
		{
			state.text = Fmt(fmt)(value);
		}

		bool changed = false;

		// 非編集中のみホイール増減を受け付ける
		if (!wasActive)
		{
			const RectF rect{ static_cast<double>(x), static_cast<double>(y),
			                  static_cast<double>(w), static_cast<double>(h) };
			if (rect.mouseOver())
			{
				const int wheel = static_cast<int>(Mouse::Wheel());
				if (wheel != 0)
				{
					value = static_cast<T>(Clamp(static_cast<double>(value) - wheel * static_cast<double>(step),
					                             static_cast<double>(lo), static_cast<double>(hi)));
					state.text = Fmt(fmt)(value);
					changed = true;
				}
			}
		}

		textInput(font, state, x, y, w, h, 12);

		// 編集確定（Enter / 外部クリックで textChanged が立つ）
		const bool nowActive = (activeTextInput == &state);
		if (!nowActive && state.textChanged)
		{
			if (const auto parsed = ParseOpt<double>(state.text))
			{
				const T nv = static_cast<T>(Clamp(*parsed, static_cast<double>(lo), static_cast<double>(hi)));
				if (nv != value) { value = nv; changed = true; }
			}
			state.text = Fmt(fmt)(value);
			state.textChanged = false;
		}

		return changed;
	}

	// ── ラベル: 読み取り専用テキスト ──

	inline void label(const Font& font, StringView text, int x, int y, ColorF color = ColorF{ 0.5 })
	{
		font(text).draw(Vec2{ x, y }, color);
	}

	// ── 折りたたみセクション ──
	// クリックで開閉。開いていれば true を返す。
	// collapsed 状態は呼び出し側が bool& で管理する。

	inline bool section(const Font& font, StringView title, bool& collapsed,
	                    int x, int& y, int w, int h, ColorF color = ColorF{ 1.0, 1.0, 0.4 })
	{
		const StringView arrow = collapsed ? U"▶" : U"▼";
		const auto hit = hitTest(font, x, y, w, h);

		// ヘッダ背景（ホバー時にハイライト）
		RectF{ static_cast<double>(x), static_cast<double>(y),
		       static_cast<double>(w), static_cast<double>(h) }
			.draw(hit.hover ? ColorF{ 0.2, 0.2, 0.3 } : ColorF{ 0.1, 0.1, 0.15 });

		font(arrow).draw(Vec2{ x + 2, y }, color);
		font(title).draw(Vec2{ x + 16, y }, color);

		if (hit.clickL)
			collapsed = !collapsed;

		y += h;
		return !collapsed;
	}
}
