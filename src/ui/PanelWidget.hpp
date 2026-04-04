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

	// ── 数値スピン: ホイールで増減。変化したら true ──

	inline bool spin(const Font& font, float& value, float step, float lo, float hi,
	                 int x, int y, int w, int h, StringView fmt = U"{:.1f}")
	{
		const auto hit = hitTest(font, x, y, w, h);
		RectF{ static_cast<double>(x), static_cast<double>(y),
		       static_cast<double>(w), static_cast<double>(h) }
			.draw(hit.hover ? ColorF{ 0.2, 0.2, 0.3 } : ColorF{ 0.12, 0.12, 0.18 });
		font(Fmt(fmt)(value)).draw(Vec2{ x + 2, y }, Palette::White);
		if (hit.wheel != 0)
		{
			value = Clamp(value - hit.wheel * step, lo, hi);
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
