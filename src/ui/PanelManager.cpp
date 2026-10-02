#include "PanelManager.hpp"

PanelState* PanelManager::find(StringView id)
{
	if (auto it = m_panels.find(String{ id }); it != m_panels.end())
		return &it->second;
	return nullptr;
}

const PanelState* PanelManager::find(StringView id) const
{
	if (auto it = m_panels.find(String{ id }); it != m_panels.end())
		return &it->second;
	return nullptr;
}

Array<PanelState*> PanelManager::sortedPanels()
{
	// 描画と入力処理で同じ順序を使えるよう、表示中パネルだけを zOrder 順へ並べ替える。
	Array<PanelState*> result;
	for (auto& [k, v] : m_panels)
		if (v.visible) result << &v;
	result.sort_by([](const PanelState* a, const PanelState* b) { return a->zOrder < b->zOrder; });
	return result;
}

void PanelManager::registerPanel(StringView id, Vec2 size, bool movable, bool scrollable, StringView exclusiveGroup)
{
	PanelState p;
	p.id         = String{ id };
	p.exclusiveGroup = String{ exclusiveGroup };
	p.size       = size;
	p.movable    = movable;
	p.scrollable = scrollable;
	m_panels[p.id] = std::move(p);
}

void PanelManager::show(StringView id, StringView title, Vec2 pos)
{
	if (auto* p = find(id))
	{
		if (!p->exclusiveGroup.isEmpty())
		{
			for (auto& [otherId, other] : m_panels)
			{
				if (otherId != id && other.visible && other.exclusiveGroup == p->exclusiveGroup)
				{
					hide(otherId);
				}
			}
		}
		p->title   = String{ title };
		if (!p->visible) p->pos = pos;  // 再表示時のみ位置リセット
		p->visible = true;
		p->zOrder  = m_nextZOrder++;
	}
}

void PanelManager::hide(StringView id)
{
	if (auto* p = find(id))
	{
		p->visible = false;
		p->scrollOffset = 0.0;
		if (m_draggingId == id) { m_draggingId.clear(); }
	}
}

bool PanelManager::isVisible(StringView id) const
{
	const auto* p = find(id);
	return p && p->visible;
}

bool PanelManager::handleInput(Vec2 cursor, bool clicked, bool held, double wheel)
{
	// 入力処理はドラッグ継続、最前面パネル判定、閉じる・前面化・スクロールの順に処理する。
	m_consumedInput = false;
	m_mouseOwner.clear();

	// ドラッグ中の処理（パネル移動）
	if (!m_draggingId.isEmpty())
	{
		if (held)
		{
			if (auto* p = find(m_draggingId))
				p->pos = cursor - m_dragOffset;
			m_consumedInput = true;
			m_mouseOwner = m_draggingId;
			return true;
		}
		m_draggingId.clear();
	}

	// zOrder 降順（手前→奥）でイテレート
	auto panels = sortedPanels();
	panels.reverse();

	for (auto* p : panels)
	{
		const RectF panelRect{ p->pos, p->size };
		if (!panelRect.contains(cursor)) continue;

		// このパネルがカーソルの最前面オーナー
		m_mouseOwner = p->id;

		// 閉じるボタン判定
		const RectF closeRect{ p->pos.x + p->size.x - kTitleBarH, p->pos.y,
		                       static_cast<double>(kTitleBarH), static_cast<double>(kTitleBarH) };
		if (closeRect.contains(cursor) && clicked)
		{
			p->visible = false;
			m_consumedInput = true;
			return true;
		}

		// タイトルバードラッグ開始
		const RectF titleRect{ p->pos.x, p->pos.y, p->size.x - kTitleBarH, static_cast<double>(kTitleBarH) };
		if (p->movable && titleRect.contains(cursor) && clicked)
		{
			m_draggingId = p->id;
			m_dragOffset = cursor - p->pos;
			p->zOrder = m_nextZOrder++;
			return true;
		}

		// クリック → 最前面に移動
		if (clicked)
		{
			p->zOrder = m_nextZOrder++;
			// クリックは消費するが、コンテンツ側のボタン処理は
			// beginContent 内の Transformer2D 経由で行われるため、
			// ここでは return しない（コンテンツクリックを許可）
		}

		// ホイール → スクロール
		if (p->scrollable && wheel != 0.0)
		{
			p->scrollOffset += wheel * 40.0;
			const double viewH = p->size.y - kTitleBarH;
			p->scrollOffset = Clamp(p->scrollOffset, 0.0, Max(0.0, p->contentHeight - viewH));
		}

		// パネル上にマウスがあるので入力消費（ホバーのみでも 3D 空間側へ伝播させない）
		m_consumedInput = true;
		return true;
	}

	return false;
}

void PanelManager::drawBackgrounds()
{
	auto panels = sortedPanels();
	for (const auto* p : panels)
	{
		drawBackground(p->id);
	}
}

void PanelManager::drawBackground(StringView id)
{
	// 背景描画ではタイトルバーとスクロールバーまでまとめて描き、コンテンツ本体は beginContent 側へ分離する。
	const auto* p = find(id);
	if (!p || !p->visible) return;

	// パネル背景
	RectF{ p->pos, p->size }.draw(ColorF{ 0, 0, 0, 0.8 });

	// タイトルバー
	RectF{ p->pos.x, p->pos.y, p->size.x, static_cast<double>(kTitleBarH) }
		.draw(ColorF{ 0.15, 0.15, 0.2 });

	// タイトルテキスト
	m_titleFont(p->title).draw(p->pos + Vec2{ 6, 3 }, Palette::White);

	// 閉じるボタン [x]
	const RectF closeRect{ p->pos.x + p->size.x - kTitleBarH, p->pos.y,
	                       static_cast<double>(kTitleBarH), static_cast<double>(kTitleBarH) };
	if (closeRect.mouseOver())
	{
		closeRect.draw(ColorF{ 0.5, 0.2, 0.2 });
	}
	m_titleFont(U"x").drawAt(closeRect.center(), Palette::White);

	// スクロールバー（コンテンツがはみ出す場合のみ）
	if (p->scrollable)
	{
		const double viewH = p->size.y - kTitleBarH;
		if (p->contentHeight > viewH && viewH > 0)
		{
			const double barX = p->pos.x + p->size.x - 4;
			const double barY = p->pos.y + kTitleBarH;
			const double barH = viewH;
			const double thumbH = Max(20.0, barH * viewH / p->contentHeight);
			const double thumbY = barY + (barH - thumbH) * (p->scrollOffset / (p->contentHeight - viewH));

			RectF{ barX, barY, 4.0, barH }.draw(ColorF{ 0.1, 0.1, 0.1, 0.5 });
			RectF{ barX, thumbY, 4.0, thumbH }.draw(ColorF{ 0.5, 0.5, 0.5, 0.7 });
		}
	}
}

Array<String> PanelManager::sortedPanelIds() const
{
	Array<std::pair<String, int>> items;
	for (const auto& [k, v] : m_panels)
	{
		if (v.visible) items << std::pair{ k, v.zOrder };
	}
	items.sort_by([](const auto& a, const auto& b) { return a.second < b.second; });

	Array<String> result;
	for (auto& [id, z] : items)
	{
		result << std::move(id);
	}
	return result;
}

Optional<ScopedContentArea> PanelManager::beginContent(StringView id)
{
	// コンテンツ描画はシザーと座標変換を RAII でまとめ、各パネル実装側をローカル座標前提にする。
	auto* p = find(id);
	if (!p || !p->visible) return none;

	const int cx = static_cast<int>(p->pos.x);
	const int cy = static_cast<int>(p->pos.y) + kTitleBarH;
	const int cw = static_cast<int>(p->size.x);
	const int ch = static_cast<int>(p->size.y) - kTitleBarH;

	if (cw <= 0 || ch <= 0) return none;

	const Rect clipRect{ cx, cy, cw, ch };

	Graphics2D::SetScissorRect(clipRect);

	const Mat3x2 drawMat = Mat3x2::Translate(static_cast<double>(cx), static_cast<double>(cy) - p->scrollOffset);

	// カーソルがこのパネル上にあり、かつ別のパネルが最前面の場合はカーソル変換を無効化
	const bool interactive = m_mouseOwner.isEmpty() || m_mouseOwner == p->id;

	return ScopedContentArea{
		ScopedRenderStates2D{ RasterizerState{ FillMode::Solid, CullMode::Back, true } },
		Transformer2D{
			drawMat,
			interactive ? TransformCursor::Yes : TransformCursor::No
		}
	};
}

void PanelManager::reportContentHeight(StringView id, double height)
{
	if (auto* p = find(id))
	{
		p->contentHeight = height;
		const double viewH = p->size.y - kTitleBarH;
		p->scrollOffset = Clamp(p->scrollOffset, 0.0, Max(0.0, height - viewH));
	}
}

bool PanelManager::isMouseOnAnyPanel(Vec2 cursor) const
{
	for (const auto& [k, v] : m_panels)
	{
		if (v.visible && RectF{ v.pos, v.size }.contains(cursor))
			return true;
	}
	return false;
}
