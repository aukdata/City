#include "PanelLayout.hpp"

PanelBuilder::PanelBuilder(int width, int padding, int gap)
	: m_width(width)
	, m_padding(padding)
	, m_gap(gap)
	, m_x(padding)
	, m_y(0)
	, m_font(FontAsset(Asset::Panel14))
	, m_boldFont(FontAsset(Asset::PanelBold14))
{
}

int PanelBuilder::widgetX() const
{
	return m_inRow ? m_rowX : m_x;
}

int PanelBuilder::widgetW(int explicitW) const
{
	if (explicitW > 0)
	{
		return explicitW;
	}
	if (m_inRow)
	{
		return 60;
	}
	return m_width - m_padding * 2;
}

void PanelBuilder::advance(int w, int h)
{
	// 行内配置と縦積み配置でカーソルの進め方を切り替え、次のウィジェット位置を確定する。
	if (m_inRow)
	{
		if (m_rowItemCount > 0)
		{
			m_rowX += m_rowGap;
		}
		m_rowX += w;
		m_rowMaxH = Max(m_rowMaxH, h);
		++m_rowItemCount;
	}
	else
	{
		if (m_y > 0)
		{
			m_y += m_gap;
		}
		m_y += h;
	}
}

void PanelBuilder::label(StringView text, ColorF color, bool bold)
{
	// ラベルも他のウィジェットと同じレイアウト規則に乗せ、描画後に占有分だけ進める。
	const Font& f = bold ? m_boldFont : m_font;
	const int x = widgetX();
	f(text).draw(Vec2{ x, m_y }, color);

	if (m_inRow)
	{
		const int w = static_cast<int>(f(text).region().w) + 2;
		advance(w, kLineH);
	}
	else
	{
		advance(0, kLineH);
	}
}

bool PanelBuilder::button(StringView lbl, bool active, int width, StringView tooltip)
{
	// クリック系ウィジェットは描画とカーソル更新を一体で扱い、呼び出し側を単純に保つ。
	const int x = widgetX();
	const int w = widgetW(width);
	const bool clicked = PanelWidget::button(m_font, lbl, active,
	                                         x, m_y, w, kLineH, tooltip);
	advance(w, kLineH);
	return clicked;
}

bool PanelBuilder::buttonDanger(StringView lbl, int width, StringView tooltip)
{
	const int x = widgetX();
	const int w = widgetW(width);
	const bool clicked = PanelWidget::buttonDanger(m_font, lbl,
	                                               x, m_y, w, kLineH, tooltip);
	advance(w, kLineH);
	return clicked;
}

bool PanelBuilder::toggle(StringView labelOn, StringView labelOff, bool& value,
                          int width, StringView tooltip)
{
	const int x = widgetX();
	const int w = widgetW(width);
	const bool changed = PanelWidget::toggle(m_font, labelOn, labelOff, value,
	                                         x, m_y, w, kLineH, tooltip);
	advance(w, kLineH);
	return changed;
}

bool PanelBuilder::textInput(TextEditState& state, int width, size_t maxChars)
{
	const int x = widgetX();
	const int w = widgetW(width);
	const int h = kLineH + 2;
	const bool changed = PanelWidget::textInput(m_font, state, x, m_y, w, h, maxChars);
	advance(w, h);
	return changed;
}

bool PanelBuilder::section(StringView title, bool& collapsed, ColorF color)
{
	// セクション見出しだけは前後の余白を明示的に確保し、折りたたみの起点にする。
	const int x = widgetX();
	const int w = m_width - m_padding * 2;
	if (m_y > 0)
	{
		m_y += m_gap;
	}
	const bool open = PanelWidget::section(m_font, title, collapsed,
	                                       x, m_y, w, kLineH, color);
	return open;
}

void PanelBuilder::spacer(int height)
{
	m_y += height;
}

void PanelBuilder::separator()
{
	if (m_y > 0)
	{
		m_y += m_gap;
	}
	const double midY = m_y + 2.5;
	Line{ static_cast<double>(m_x), midY,
	      static_cast<double>(m_width - m_padding), midY }
		.draw(1.0, ColorF{ 0.3 });
	m_y += 5;
}

void PanelBuilder::row(int gap, std::function<void()> content)
{
	// row() の間だけ横並びモードに切り替え、子要素群の最大高さで縦方向の消費量を決める。
	if (m_y > 0)
	{
		m_y += m_gap;
	}

	m_inRow = true;
	m_rowGap = gap;
	m_rowX = m_x;
	m_rowMaxH = 0;
	m_rowItemCount = 0;

	content();

	m_inRow = false;
	m_y += m_rowMaxH;
}

void PanelBuilder::flush()
{
	PanelWidget::flushTooltip();
}
