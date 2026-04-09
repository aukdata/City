#include "PanelLayout.hpp"
#include "PanelWidget.hpp"
#include "../asset/AssetRegistrar.hpp"

namespace
{
	Font panelFont()
	{
		return FontAsset(Asset::Panel14);
	}

	Font panelBoldFont()
	{
		return FontAsset(Asset::PanelBold14);
	}

	int getWidthHint(const UIElement& elem)
	{
		return std::visit([](const auto& widget) -> int
		{
			using T = std::decay_t<decltype(widget)>;
			if constexpr (requires { widget.width; })
			{
				return widget.width;
			}
			return 0;
		}, elem.widget);
	}

	int estimateContentWidth(const UIElement& elem, const Font& font)
	{
		return std::visit([&](const auto& widget) -> int
		{
			using T = std::decay_t<decltype(widget)>;
			if constexpr (std::is_same_v<T, PanelLabel>)
			{
				const Font& f = widget.bold ? panelBoldFont() : font;
				if (widget.source)
				{
					return static_cast<int>(f(widget.source()).region().w) + 4;
				}
				return static_cast<int>(f(widget.text).region().w) + 4;
			}
			else if constexpr (std::is_same_v<T, PanelButton>)
			{
				return static_cast<int>(font(widget.label).region().w) + 6;
			}
			else if constexpr (std::is_same_v<T, PanelToggle>)
			{
				const int wOn = static_cast<int>(font(widget.labelOn).region().w);
				const int wOff = static_cast<int>(font(widget.labelOff).region().w);
				return Max(wOn, wOff) + 6;
			}
			else if constexpr (std::is_same_v<T, PanelSpin>)
			{
				return 54;
			}
			else if constexpr (std::is_same_v<T, PanelCycle>)
			{
				int maxW = 0;
				for (const auto& opt : widget.options)
				{
					maxW = Max(maxW, static_cast<int>(font(opt).region().w));
				}
				return maxW + 6;
			}
			else if constexpr (std::is_same_v<T, PanelSpacer>)
			{
				return widget.height;
			}
			else
			{
				return 60;
			}
		}, elem.widget);
	}
}

// =============================================================================
// 構築
// =============================================================================

Array<UIElementPtr>& PanelLayout::currentTarget()
{
	if (m_buildStack.isEmpty())
	{
		return m_root;
	}
	return *m_buildStack.back();
}

void PanelLayout::addElement(UIVariant w)
{
	currentTarget().push_back(std::make_unique<UIElement>(std::move(w)));
	m_dirty = true;
}

void PanelLayout::label(StringView key, StringView text, std::function<String()> source,
                        ColorF color, bool bold)
{
	PanelLabel lbl;
	lbl.key = String{ key };
	lbl.text = String{ text };
	lbl.source = std::move(source);
	lbl.bold = bold;
	lbl.color = color;
	addElement(std::move(lbl));
}

void PanelLayout::label(StringView key, StringView text, String& ref,
                        ColorF color, bool bold)
{
	label(key, text, [&ref] { return ref; }, color, bold);
}

void PanelLayout::label(StringView key, StringView text, float& ref,
                        ColorF color, bool bold)
{
	label(key, text, [&ref] { return U"{}"_fmt(ref); }, color, bold);
}

void PanelLayout::label(StringView key, StringView text, int& ref,
                        ColorF color, bool bold)
{
	label(key, text, [&ref] { return U"{}"_fmt(ref); }, color, bold);
}

void PanelLayout::label(StringView key, StringView text, ColorF color, bool bold)
{
	PanelLabel lbl;
	lbl.key = String{ key };
	lbl.text = String{ text };
	lbl.bold = bold;
	lbl.color = color;
	addElement(std::move(lbl));
}

void PanelLayout::button(StringView key, StringView lbl, std::function<bool()> active,
                         int width, StringView tooltip)
{
	PanelButton btn;
	btn.key = String{ key };
	btn.label = String{ lbl };
	btn.active = std::move(active);
	btn.tooltip = String{ tooltip };
	btn.width = width;
	addElement(std::move(btn));
}

void PanelLayout::button(StringView key, StringView lbl, bool& activeRef,
                         int width, StringView tooltip)
{
	button(key, lbl, [&activeRef] { return activeRef; }, width, tooltip);
}

void PanelLayout::toggle(StringView key, StringView labelOn, StringView labelOff, bool& ref,
                         int width, StringView tooltip)
{
	PanelToggle t;
	t.key = String{ key };
	t.labelOn = String{ labelOn };
	t.labelOff = String{ labelOff };
	t.ref = &ref;
	t.tooltip = String{ tooltip };
	t.width = width;
	addElement(std::move(t));
}

void PanelLayout::spin(StringView key, float& ref, float step, float lo, float hi,
                       StringView format, int width)
{
	PanelSpin s;
	s.key = String{ key };
	s.ref = &ref;
	s.step = step;
	s.lo = lo;
	s.hi = hi;
	s.format = String{ format };
	s.width = width;
	addElement(std::move(s));
}

void PanelLayout::spacer(int height)
{
	PanelSpacer sp;
	sp.height = height;
	addElement(std::move(sp));
}

void PanelLayout::separator()
{
	addElement(PanelSeparator{});
}

void PanelLayout::custom(StringView key, std::function<int(int, int, int)> onDraw, int height)
{
	PanelCustom c;
	c.key = String{ key };
	c.height = height;
	c.onDraw = std::move(onDraw);
	addElement(std::move(c));
}

void PanelLayout::beginVStack(int gap, int padding)
{
	PanelVStack vs;
	vs.gap = gap;
	vs.padding = padding;
	auto elem = std::make_unique<UIElement>(std::move(vs));
	auto& ref = currentTarget().emplace_back(std::move(elem));
	m_buildStack.push_back(&std::get<PanelVStack>(ref->widget).children);
	m_dirty = true;
}

void PanelLayout::beginHStack(int gap)
{
	PanelHStack hs;
	hs.gap = gap;
	auto elem = std::make_unique<UIElement>(std::move(hs));
	auto& ref = currentTarget().emplace_back(std::move(elem));
	m_buildStack.push_back(&std::get<PanelHStack>(ref->widget).children);
	m_dirty = true;
}

void PanelLayout::end()
{
	if (!m_buildStack.isEmpty())
	{
		m_buildStack.pop_back();
	}
}

void PanelLayout::beginSection(StringView key, StringView title, bool collapsed, ColorF color)
{
	PanelSection sec;
	sec.key = String{ key };
	sec.title = String{ title };
	sec.collapsed = collapsed;
	sec.color = color;
	auto elem = std::make_unique<UIElement>(std::move(sec));
	auto& ref = currentTarget().emplace_back(std::move(elem));
	m_buildStack.push_back(&std::get<PanelSection>(ref->widget).children);
	m_dirty = true;
}

void PanelLayout::endSection()
{
	end();
}

// =============================================================================
// レイアウト計算
// =============================================================================

int PanelLayout::measureHeight(UIElement& elem, int w)
{
	if (!elem.visible)
	{
		return 0;
	}

	return std::visit([&](auto& widget) -> int
	{
		using T = std::decay_t<decltype(widget)>;

		if constexpr (std::is_same_v<T, PanelSpacer>)
		{
			return widget.height;
		}
		else if constexpr (std::is_same_v<T, PanelSeparator>)
		{
			return 5;
		}
		else if constexpr (std::is_same_v<T, PanelSection>)
		{
			int h = kDefaultHeight;
			if (!widget.collapsed)
			{
				for (auto& child : widget.children)
				{
					if (child)
					{
						h += measureHeight(*child, w);
					}
				}
			}
			return h;
		}
		else if constexpr (std::is_same_v<T, PanelCustom>)
		{
			return widget.height > 0 ? widget.height : 100;
		}
		else if constexpr (std::is_same_v<T, PanelVStack>)
		{
			const int innerW = w - widget.padding * 2;
			int h = widget.padding;
			int visCount = 0;
			for (auto& child : widget.children)
			{
				if (child && child->visible)
				{
					if (visCount > 0)
					{
						h += widget.gap;
					}
					h += measureHeight(*child, innerW);
					++visCount;
				}
			}
			h += widget.padding;
			return h;
		}
		else if constexpr (std::is_same_v<T, PanelHStack>)
		{
			int maxH = 0;
			for (auto& child : widget.children)
			{
				if (child && child->visible)
				{
					maxH = Max(maxH, measureHeight(*child, w));
				}
			}
			return maxH;
		}
		else
		{
			return kDefaultHeight;
		}
	}, elem.widget);
}

void PanelLayout::layoutElement(UIElement& elem, int x, int y, int w)
{
	if (!elem.visible)
	{
		elem.cx = x; elem.cy = y; elem.cw = 0; elem.ch = 0;
		return;
	}

	elem.cx = x;
	elem.cy = y;
	elem.cw = w;
	elem.ch = measureHeight(elem, w);

	std::visit([&](auto& widget)
	{
		using T = std::decay_t<decltype(widget)>;

		if constexpr (std::is_same_v<T, PanelVStack>)
		{
			const int innerX = x + widget.padding;
			const int innerW = w - widget.padding * 2;
			int curY = y + widget.padding;
			int visCount = 0;
			for (auto& child : widget.children)
			{
				if (child && child->visible)
				{
					if (visCount > 0)
					{
						curY += widget.gap;
					}
					layoutElement(*child, innerX, curY, innerW);
					curY += child->ch;
					++visCount;
				}
			}
		}
		else if constexpr (std::is_same_v<T, PanelHStack>)
		{
			const Font& font = panelFont();
			int curX = x;
			int visCount = 0;
			for (auto& child : widget.children)
			{
				if (child && child->visible)
				{
					if (visCount > 0)
					{
						curX += widget.gap;
					}
					int hint = getWidthHint(*child);
					int childW = hint > 0 ? hint : estimateContentWidth(*child, font);
					layoutElement(*child, curX, y, childW);
					curX += child->cw;
					++visCount;
				}
			}
		}
		else if constexpr (std::is_same_v<T, PanelSection>)
		{
			if (!widget.collapsed)
			{
				int curY = y + kDefaultHeight;
				for (auto& child : widget.children)
				{
					if (child && child->visible)
					{
						layoutElement(*child, x, curY, w);
						curY += child->ch;
					}
				}
			}
		}
	}, elem.widget);
}

void PanelLayout::doLayout(int availableWidth)
{
	m_availableWidth = availableWidth;
	int y = 0;
	for (auto& elem : m_root)
	{
		if (elem && elem->visible)
		{
			layoutElement(*elem, 0, y, availableWidth);
			y += elem->ch;
		}
	}
	m_contentHeight = y;
	m_dirty = false;
}

// =============================================================================
// 入力処理
// =============================================================================

void PanelLayout::updateElement(UIElement& elem)
{
	if (!elem.visible)
	{
		return;
	}

	const Font& font = panelFont();

	std::visit([&](auto& widget)
	{
		using T = std::decay_t<decltype(widget)>;

		if constexpr (std::is_same_v<T, PanelButton>)
		{
			auto hit = PanelWidget::hitTest(font, elem.cx, elem.cy, elem.cw, elem.ch, widget.tooltip);
			if (hit.clickL)
			{
				m_clickedKeys.insert(widget.key);
			}
		}
		else if constexpr (std::is_same_v<T, PanelToggle>)
		{
			const int w = widget.width > 0 ? widget.width : elem.cw;
			auto hit = PanelWidget::hitTest(font, elem.cx, elem.cy, w, elem.ch, widget.tooltip);
			if (hit.clickL && widget.ref)
			{
				*widget.ref = !(*widget.ref);
				m_changedKeys.insert(widget.key);
			}
		}
		else if constexpr (std::is_same_v<T, PanelSpin>)
		{
			const int w = widget.width > 0 ? widget.width : elem.cw;
			auto hit = PanelWidget::hitTest(font, elem.cx, elem.cy, w, elem.ch);
			if (hit.wheel != 0 && widget.ref)
			{
				*widget.ref = Clamp(*widget.ref - hit.wheel * widget.step, widget.lo, widget.hi);
				m_changedKeys.insert(widget.key);
			}
		}
		else if constexpr (std::is_same_v<T, PanelCycle>)
		{
			const int w = widget.width > 0 ? widget.width : elem.cw;
			auto hit = PanelWidget::hitTest(font, elem.cx, elem.cy, w, elem.ch, widget.tooltip);
			const int dir = hit.clickL ? 1 : (hit.clickR ? -1 : 0);
			if (dir != 0 && widget.ref && widget.count > 0)
			{
				*widget.ref = (*widget.ref + widget.count + dir) % widget.count;
				m_changedKeys.insert(widget.key);
			}
		}
		else if constexpr (std::is_same_v<T, PanelSection>)
		{
			auto hit = PanelWidget::hitTest(font, elem.cx, elem.cy, elem.cw, kDefaultHeight);
			if (hit.clickL)
			{
				widget.collapsed = !widget.collapsed;
				m_dirty = true;
			}
			if (!widget.collapsed)
			{
				for (auto& child : widget.children)
				{
					if (child) { updateElement(*child); }
				}
			}
		}
		else if constexpr (std::is_same_v<T, PanelVStack> || std::is_same_v<T, PanelHStack>)
		{
			for (auto& child : widget.children)
			{
				if (child) { updateElement(*child); }
			}
		}
	}, elem.widget);
}

void PanelLayout::update(int availableWidth)
{
	m_clickedKeys.clear();
	m_changedKeys.clear();

	if (m_dirty || availableWidth != m_availableWidth)
	{
		doLayout(availableWidth);
	}

	for (auto& elem : m_root)
	{
		if (elem) { updateElement(*elem); }
	}

	if (m_dirty)
	{
		doLayout(availableWidth);
	}
}

// =============================================================================
// 描画
// =============================================================================

void PanelLayout::drawElement(const UIElement& elem)
{
	if (!elem.visible)
	{
		return;
	}

	const Font& font = panelFont();
	const Font& boldFont = panelBoldFont();

	std::visit([&](const auto& widget)
	{
		using T = std::decay_t<decltype(widget)>;

		if constexpr (std::is_same_v<T, PanelLabel>)
		{
			const Font& f = widget.bold ? boldFont : font;
			String displayText = widget.text;
			if (widget.source)
			{
				const String val = widget.source();
				displayText = displayText.replaced(U"{}", val);
			}
			f(displayText).draw(Vec2{ elem.cx, elem.cy }, widget.color);
		}
		else if constexpr (std::is_same_v<T, PanelButton>)
		{
			const bool active = widget.active ? widget.active() : false;
			const int w = widget.width > 0 ? widget.width : elem.cw;
			PanelWidget::button(font, widget.label, active,
			                    elem.cx, elem.cy, w, elem.ch, widget.tooltip);
		}
		else if constexpr (std::is_same_v<T, PanelToggle>)
		{
			const bool val = widget.ref ? *widget.ref : false;
			const int w = widget.width > 0 ? widget.width : elem.cw;
			const auto hit = PanelWidget::hitTest(font, elem.cx, elem.cy, w, elem.ch, widget.tooltip);
			RectF{ static_cast<double>(elem.cx), static_cast<double>(elem.cy),
			       static_cast<double>(w), static_cast<double>(elem.ch) }
				.draw(val ? ColorF{ 0.3, 0.5, 0.8 }
				      : (hit.hover ? ColorF{ 0.3, 0.3, 0.4 } : ColorF{ 0.15, 0.15, 0.2 }));
			font(val ? widget.labelOn : widget.labelOff)
				.draw(Vec2{ elem.cx + 2, elem.cy }, val ? ColorF{ 1.0 } : ColorF{ 0.7 });
		}
		else if constexpr (std::is_same_v<T, PanelSpin>)
		{
			const float val = widget.ref ? *widget.ref : 0.0f;
			const int w = widget.width > 0 ? widget.width : elem.cw;
			const auto hit = PanelWidget::hitTest(font, elem.cx, elem.cy, w, elem.ch);
			RectF{ static_cast<double>(elem.cx), static_cast<double>(elem.cy),
			       static_cast<double>(w), static_cast<double>(elem.ch) }
				.draw(hit.hover ? ColorF{ 0.2, 0.2, 0.3 } : ColorF{ 0.12, 0.12, 0.18 });
			font(Fmt(widget.format)(val)).draw(Vec2{ elem.cx + 2, elem.cy }, Palette::White);
		}
		else if constexpr (std::is_same_v<T, PanelCycle>)
		{
			const int val = widget.ref ? *widget.ref : 0;
			const int w = widget.width > 0 ? widget.width : elem.cw;
			const auto hit = PanelWidget::hitTest(font, elem.cx, elem.cy, w, elem.ch, widget.tooltip);
			RectF{ static_cast<double>(elem.cx), static_cast<double>(elem.cy),
			       static_cast<double>(w), static_cast<double>(elem.ch) }
				.draw(hit.hover ? ColorF{ 0.3, 0.3, 0.4 } : ColorF{ 0.15, 0.15, 0.2 });
			if (val >= 0 && val < widget.count)
			{
				font(widget.options[val]).draw(Vec2{ elem.cx + 2, elem.cy }, ColorF{ 0.7 });
			}
		}
		else if constexpr (std::is_same_v<T, PanelSpacer>)
		{
			// no draw
		}
		else if constexpr (std::is_same_v<T, PanelSeparator>)
		{
			const double midY = elem.cy + elem.ch * 0.5;
			Line{ static_cast<double>(elem.cx), midY,
			      static_cast<double>(elem.cx + elem.cw), midY }
				.draw(1.0, ColorF{ 0.3 });
		}
		else if constexpr (std::is_same_v<T, PanelSection>)
		{
			const StringView arrow = widget.collapsed ? U"\u25B6" : U"\u25BC";
			const auto hit = PanelWidget::hitTest(font, elem.cx, elem.cy, elem.cw, kDefaultHeight);
			RectF{ static_cast<double>(elem.cx), static_cast<double>(elem.cy),
			       static_cast<double>(elem.cw), static_cast<double>(kDefaultHeight) }
				.draw(hit.hover ? ColorF{ 0.2, 0.2, 0.3 } : ColorF{ 0.1, 0.1, 0.15 });
			font(arrow).draw(Vec2{ elem.cx + 2, elem.cy }, widget.color);
			font(widget.title).draw(Vec2{ elem.cx + 16, elem.cy }, widget.color);

			if (!widget.collapsed)
			{
				for (const auto& child : widget.children)
				{
					if (child) { drawElement(*child); }
				}
			}
		}
		else if constexpr (std::is_same_v<T, PanelCustom>)
		{
			if (widget.onDraw)
			{
				widget.onDraw(elem.cx, elem.cy, elem.cw);
			}
		}
		else if constexpr (std::is_same_v<T, PanelVStack> || std::is_same_v<T, PanelHStack>)
		{
			for (const auto& child : widget.children)
			{
				if (child) { drawElement(*child); }
			}
		}
	}, elem.widget);
}

void PanelLayout::draw()
{
	for (const auto& elem : m_root)
	{
		if (elem) { drawElement(*elem); }
	}
	PanelWidget::flushTooltip();
}

// =============================================================================
// イベント・検索
// =============================================================================

bool PanelLayout::clicked(StringView key) const
{
	return m_clickedKeys.contains(String{ key });
}

bool PanelLayout::changed(StringView key) const
{
	return m_changedKeys.contains(String{ key });
}

UIElement* PanelLayout::findByKey(StringView key)
{
	std::function<UIElement*(Array<UIElementPtr>&)> search;
	search = [&](Array<UIElementPtr>& elements) -> UIElement*
	{
		for (auto& elem : elements)
		{
			if (!elem) { continue; }

			bool matched = std::visit([&](const auto& widget) -> bool
			{
				using T = std::decay_t<decltype(widget)>;
				if constexpr (requires { widget.key; })
				{
					return widget.key == key;
				}
				return false;
			}, elem->widget);

			if (matched) { return elem.get(); }

			UIElement* found = std::visit([&](auto& widget) -> UIElement*
			{
				using T = std::decay_t<decltype(widget)>;
				if constexpr (std::is_same_v<T, PanelVStack> || std::is_same_v<T, PanelHStack>
				              || std::is_same_v<T, PanelSection>)
				{
					return search(widget.children);
				}
				return static_cast<UIElement*>(nullptr);
			}, elem->widget);

			if (found) { return found; }
		}
		return nullptr;
	};

	return search(m_root);
}

void PanelLayout::setVisible(StringView key, bool vis)
{
	if (auto* elem = findByKey(key))
	{
		if (elem->visible != vis)
		{
			elem->visible = vis;
			m_dirty = true;
		}
	}
}
