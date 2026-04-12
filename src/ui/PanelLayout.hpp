#pragma once
#include <Siv3D.hpp>
#include <functional>
#include "PanelWidget.hpp"
#include "../asset/AssetRegistrar.hpp"

/// @brief パネル内 UI の即時モード自動レイアウト
/// @details 毎フレーム構築・描画する。y 座標の手動管理を排除する。
///
/// 使用例:
/// @code
/// PanelBuilder ui(contentWidth);
/// ui.label(U"Type: {}"_fmt(type));
/// ui.row(8, [&] {
///     ui.label(U"Speed", ColorF{0.6});
///     ui.spin(speed, 10.f, 10.f, 200.f, U"{:.0f}", 44);
/// });
/// if (ui.button(U"Track", tracking, 120))
///     tracking = !tracking;
/// reportContentHeight(ui.height());
/// @endcode
class PanelBuilder
{
public:
	static constexpr int kLineH = 17;

	/// @param width 利用可能なコンテンツ幅
	/// @param padding 左右の余白
	/// @param gap 要素間の縦スペース
	PanelBuilder(int width, int padding = 6, int gap = 2);

	// ── ウィジェット ──

	/// @brief ラベル（読み取り専用テキスト）
	void label(StringView text, ColorF color = ColorF{ 0.7 }, bool bold = false);

	/// @brief ボタン。クリックされたら true
	bool button(StringView lbl, bool active = false, int width = 0, StringView tooltip = U"");

	/// @brief ON/OFF トグル。変化したら true
	bool toggle(StringView labelOn, StringView labelOff, bool& value,
	            int width = 0, StringView tooltip = U"");

	/// @brief 数値スピナー。変化したら true
	bool spin(float& value, float step, float lo, float hi,
	          StringView fmt = U"{:.1f}", int width = 0);

	/// @brief 列挙サイクル。変化したら true
	template <typename E>
	bool cycle(E& value, const StringView* names, int count,
	           int width = 0, StringView tooltip = U"");

	/// @brief 折りたたみセクション。開いていれば true
	bool section(StringView title, bool& collapsed,
	             ColorF color = ColorF{ 1.0, 1.0, 0.4 });

	/// @brief スペーサー
	void spacer(int height = 4);

	/// @brief 区切り線
	void separator();

	// ── レイアウト ──

	/// @brief 横並びレイアウト。ラムダ内のウィジェットは横に並ぶ
	void row(int gap, std::function<void()> content);

	/// @brief ツールチップを描画（最後に呼ぶ）
	void flush();

	/// @brief コンテンツ全体の高さ
	[[nodiscard]] int height() const { return m_y; }

private:
	int m_width;
	int m_padding;
	int m_gap;
	int m_x;
	int m_y;

	// row 状態
	bool m_inRow = false;
	int  m_rowGap = 0;
	int  m_rowX = 0;
	int  m_rowMaxH = 0;
	int  m_rowItemCount = 0;

	Font m_font;
	Font m_boldFont;

	int widgetX() const;
	int widgetW(int explicitW) const;
	void advance(int w, int h);
};

// ── テンプレート実装 ──

template <typename E>
bool PanelBuilder::cycle(E& value, const StringView* names, int count,
                         int width, StringView tooltip)
{
	const int x = widgetX();
	const int w = widgetW(width);
	const bool changed = PanelWidget::cycle(m_font, value, names, count,
	                                        x, m_y, w, kLineH, tooltip);
	advance(w, kLineH);
	return changed;
}
