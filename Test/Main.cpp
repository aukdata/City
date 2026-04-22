// UI レビュー用テスト: 敷設パネル / エッジ編集パネル / PanelWidget プリミティブ を 1 画面にモック配置
// ・左: 敷設パネル風（スタート/ゴール指定モード、所属ルート）
// ・右上: エッジ編集パネル風（部品バー Quad、車線バー 4 隅マーカー、4 隅数値入力）
// ・右下: PanelWidget 単体（長ラベルボタン、ツールチップ複数行）

# include <Siv3D.hpp>
# include "src/ui/PanelWidget.hpp"
# include "src/asset/AssetRegistrar.hpp"

namespace
{
	// ── モック用の軽量データ構造（本体 RoadEdge を使うと膨大な依存が出るため最小に）──
	struct MockPart
	{
		float aL, aR, bL, bR;
		int   type;  // 0..9 の部品種別
	};
	struct MockLane
	{
		float aL, aR, bL, bR;
		int   dir; // 0=forward, 1=backward
	};

	enum class RouteKind { Expressway, National, Prefectural, City, Named };

	struct MockRoute
	{
		int       id;
		RouteKind kind;
		int       number;
		String    name;
		ColorF    color;
	};

	struct NewRouteState
	{
		bool          expanded = false;
		RouteKind     kind = RouteKind::National;
		int           number = 0;
		TextEditState nameEdit;
	};

	ColorF PartTypeColor(int type)
	{
		switch (type)
		{
		case 0: return ColorF{ 0.3, 0.3, 0.35 };   // 路盤
		case 1: return ColorF{ 0.55, 0.5, 0.35 };  // 路肩
		case 2: return ColorF{ 0.75, 0.75, 0.25 }; // 中央帯
		case 3: return ColorF{ 0.55, 0.55, 0.60 }; // 歩道
		default: return ColorF{ 0.4, 0.4, 0.5 };
		}
	}

	// ========================================================================
	// 左: 敷設パネル モック
	// ========================================================================
	void DrawDrawTemplatePanelMock(const Font& pFont, const Font& pBold,
		int px, int py, int panelW,
		bool& autoPlaceMode, Optional<Vec2>& autoPlaceStart,
		Array<MockRoute>& routes, HashSet<int>& pendingRouteIds,
		NewRouteState& newRouteState)
	{
		constexpr int kPad = 6;
		constexpr int kLH = 17;

		// パネル枠
		RectF{ px - 2, py - 2, panelW + 4, 340 }.draw(ColorF{ 0.06, 0.06, 0.09, 0.95 });
		RectF{ px - 2, py - 2, panelW + 4, 340 }.drawFrame(1.0, 0.0, ColorF{ 0.3 });

		// タイトル
		pBold(U"敷設テンプレ").draw(Vec2{ px + kPad, py - 18 }, ColorF{ 1.0, 0.9, 0.5 });

		int y = py;

		// ── Section 0: スタート/ゴール指定モード ──
		const bool toggled = PanelWidget::button(
			pFont, U"スタート/ゴール指定モード", autoPlaceMode,
			px + kPad, y, panelW - kPad * 2, kLH,
			U"2 点クリックで自動経路探索・敷設");
		if (toggled)
		{
			autoPlaceMode = !autoPlaceMode;
			if (!autoPlaceMode) autoPlaceStart = none;
		}
		y += kLH + 2;

		if (autoPlaceMode)
		{
			const String statusText = autoPlaceStart
				? U"スタート: ({:.0f}, {:.0f}, {:.0f})"_fmt(autoPlaceStart->x, 0.0, autoPlaceStart->y)
				: U"スタート未指定 — クリックで地点を選択";
			PanelWidget::label(pFont, statusText, px + kPad, y, ColorF{ 0.7, 1.0, 0.7 });
			y += kLH + 2;
		}

		// 区切り
		y += 6;

		// ── Section 4: 所属ルート ──
		static bool routeCollapsed = false;
		int sectY = y;
		PanelWidget::section(pBold, U"所属ルート", routeCollapsed, px, sectY, panelW, kLH,
			ColorF{ 0.5, 1.0, 0.7 });
		y = sectY;

		if (!routeCollapsed)
		{
			constexpr StringView kindNames[] = { U"高速道路", U"国道", U"都道府県道", U"市区町村道", U"名称路線" };

			for (const auto& route : routes)
			{
				const bool selected = pendingRouteIds.contains(route.id);
				const String lbl = U"[{}] {}"_fmt(kindNames[static_cast<int>(route.kind)], route.name);
				if (PanelWidget::button(pFont, lbl, selected, px + kPad, y, panelW - kPad * 2, kLH,
					U"このルートに含める/外す"))
				{
					if (selected) pendingRouteIds.erase(route.id);
					else pendingRouteIds.emplace(route.id);
				}
				// 色スウォッチ（ボタン右端に小さく）
				RectF{ px + panelW - kPad - 14.0, y + 2.0, 10, kLH - 4 }.draw(route.color);
				y += kLH + 2;
			}

			// 新規ルート展開ボタン
			if (PanelWidget::button(pFont,
				newRouteState.expanded ? U"▲ 新規ルート" : U"＋ 新規ルート",
				newRouteState.expanded,
				px + kPad, y, panelW - kPad * 2, kLH, U"新規ルートを作成"))
			{
				newRouteState.expanded = !newRouteState.expanded;
			}
			y += kLH + 2;

			if (newRouteState.expanded)
			{
				// 種別 cycle
				{
					const int xLbl = px + kPad;
					const int xCtrl = xLbl + 50;
					PanelWidget::label(pFont, U"種別", xLbl, y, ColorF{ 0.6 });
					PanelWidget::cycle(pFont, newRouteState.kind, kindNames, 5, xCtrl, y,
						panelW + px - kPad - xCtrl, kLH);
					y += kLH + 2;
				}
				// 番号
				{
					const int xLbl = px + kPad;
					const int xCtrl = xLbl + 50;
					PanelWidget::label(pFont, U"番号", xLbl, y, ColorF{ 0.6 });
					PanelWidget::numberInput(pFont, newRouteState.number, 1, 0, 400,
						xCtrl, y, 60, kLH, U"{}");
					y += kLH + 2;
				}
				// 名前
				{
					const int xLbl = px + kPad;
					const int xCtrl = xLbl + 50;
					PanelWidget::label(pFont, U"名前", xLbl, y, ColorF{ 0.6 });
					PanelWidget::textInput(pFont, newRouteState.nameEdit, xCtrl, y,
						panelW + px - kPad - xCtrl, kLH, 32);
					y += kLH + 2;
				}
				// 色スウォッチ（仮: 固定色を並べて選択）
				{
					const int xLbl = px + kPad;
					int xCtrl = xLbl + 50;
					PanelWidget::label(pFont, U"色", xLbl, y, ColorF{ 0.6 });
					const ColorF swatches[] = {
						ColorF{0.9,0.3,0.3}, ColorF{0.95,0.6,0.2}, ColorF{0.95,0.85,0.25},
						ColorF{0.3,0.8,0.4}, ColorF{0.3,0.6,0.95}, ColorF{0.7,0.4,0.9}
					};
					for (const auto& c : swatches)
					{
						RectF{ xCtrl, y + 1.0, 18, kLH - 2 }.draw(c);
						RectF{ xCtrl, y + 1.0, 18, kLH - 2 }.drawFrame(1.0, ColorF{ 0.2 });
						xCtrl += 20;
					}
					y += kLH + 2;
				}
				// 作成ボタン
				PanelWidget::button(pFont, U"作成", false, px + kPad, y, 60, kLH,
					U"ルートを作成して選択に追加");
				y += kLH + 2;
			}
		}
	}

	// ========================================================================
	// 右上: エッジ編集パネル モック（部品バー・車線バー・4 隅マーカー・4 隅数値入力）
	// ========================================================================
	void DrawEdgePanelMock(const Font& pFont, const Font& pBold,
		int px, int py,
		Array<MockPart>& parts, Array<MockLane>& lanes,
		int& selectedLane)
	{
		constexpr int kBarX = 6;
		constexpr int kBarW = 356;
		constexpr int kPartBarH = 36;
		constexpr int kLaneBarH = 28;
		constexpr int kLH = 17;
		const int panelW = 380;

		RectF{ px - 2, py - 2, panelW + 4, 210 }.draw(ColorF{ 0.06, 0.06, 0.09, 0.95 });
		RectF{ px - 2, py - 2, panelW + 4, 210 }.drawFrame(1.0, 0.0, ColorF{ 0.3 });

		pBold(U"エッジ編集").draw(Vec2{ px + 6, py - 18 }, ColorF{ 1.0, 0.9, 0.5 });

		// スケール計算
		float mn = 1e9f, mx = -1e9f;
		for (const auto& p : parts)
		{
			mn = Min(mn, Min(p.aL, p.bL)); mx = Max(mx, Max(p.aR, p.bR));
		}
		for (const auto& L : lanes)
		{
			mn = Min(mn, Min(L.aL, L.bL)); mx = Max(mx, Max(L.aR, L.bR));
		}
		if (mn >= mx) { mn = -5.0f; mx = 5.0f; }
		const float margin = (mx - mn) * 0.08f + 0.5f;
		mn -= margin; mx += margin;
		const float range = mx - mn;
		auto toPx = [&](float m) -> double { return px + kBarX + (m - mn) / range * kBarW; };

		int y = py;

		// 部品バー（Quad）
		pBold(U"断面部品 ({})"_fmt(parts.size())).draw(Vec2{ px + 4, y }, ColorF{ 1.0, 1.0, 0.4 });
		y += kLH;
		const int barY = y;
		RectF{ static_cast<double>(px + kBarX), static_cast<double>(barY),
			   static_cast<double>(kBarW), static_cast<double>(kPartBarH) }
			.draw(ColorF{ 0.08, 0.08, 0.10 });

		for (const auto& p : parts)
		{
			const double pxA0 = toPx(p.aL), pxA1 = toPx(p.aR);
			const double pxB0 = toPx(p.bL), pxB1 = toPx(p.bR);
			const double yT = barY + 2;
			const double yB = barY + kPartBarH - 2;
			Quad{ Vec2{pxA0, yT}, Vec2{pxA1, yT}, Vec2{pxB1, yB}, Vec2{pxB0, yB} }
			.draw(PartTypeColor(p.type));
			Quad{ Vec2{pxA0, yT}, Vec2{pxA1, yT}, Vec2{pxB1, yB}, Vec2{pxB0, yB} }
			.drawFrame(1.0, ColorF{ 0.3 });
		}
		y += kPartBarH + 4;

		// 車線バー
		pBold(U"車線 ({})"_fmt(lanes.size())).draw(Vec2{ px + 4, y }, ColorF{ 1.0, 1.0, 0.4 });
		y += kLH;
		const int laneBarY = y;
		RectF{ static_cast<double>(px + kBarX), static_cast<double>(laneBarY),
			   static_cast<double>(kBarW), static_cast<double>(kLaneBarH) }
			.draw(ColorF{ 0.08, 0.08, 0.10 });

		for (int i = 0; i < static_cast<int>(lanes.size()); ++i)
		{
			const auto& L = lanes[i];
			const double pxA0 = toPx(L.aL), pxA1 = toPx(L.aR);
			const double pxB0 = toPx(L.bL), pxB1 = toPx(L.bR);
			const double yT = laneBarY + 2;
			const double yB = laneBarY + kLaneBarH - 2;
			const bool sel = (i == selectedLane);
			const ColorF col = sel ? ColorF{ 0.35, 0.35, 0.40 } : ColorF{ 0.22, 0.22, 0.25 };
			Quad laneQ{ Vec2{pxA0, yT}, Vec2{pxA1, yT}, Vec2{pxB1, yB}, Vec2{pxB0, yB} };
			laneQ.draw(col);
			if (sel) laneQ.drawFrame(1.0, ColorF{ 1.0, 1.0, 0.3 });

			const StringView arr = (L.dir == 0) ? U"\u2192" : U"\u2190";
			pBold(arr).drawAt(10.0, Vec2{ (pxA0 + pxA1 + pxB0 + pxB1) * 0.25,
				laneBarY + kLaneBarH * 0.5 }, ColorF{ 1, 1, 1, 0.6 });
		}

		// 選択車線の 4 隅マーカー
		if (selectedLane >= 0 && selectedLane < static_cast<int>(lanes.size()))
		{
			const auto& SL = lanes[selectedLane];
			const double yT = laneBarY + 2;
			const double yB = laneBarY + kLaneBarH - 2;
			auto tint = [](bool atA, bool isRight) {
				const ColorF base = atA ? ColorF{ 1.0, 0.55, 0.45 } : ColorF{ 0.45, 1.0, 0.55 };
				return isRight ? (base * 0.7 + ColorF{ 0.15 }) : base;
			};
			struct Corner { double x; double y; ColorF c; };
			const Corner corners[4] = {
				{ toPx(SL.aL), yT, tint(true,  false) },
				{ toPx(SL.bL), yB, tint(false, false) },
				{ toPx(SL.aR), yT, tint(true,  true)  },
				{ toPx(SL.bR), yB, tint(false, true)  },
			};
			for (const auto& c : corners)
			{
				Circle{ c.x, c.y, 4.0 }.draw(c.c);
				Circle{ c.x, c.y, 4.0 }.drawFrame(1.0, ColorF{ 0.1 });
			}
		}
		y += kLaneBarH + 4;

		// 4 隅数値入力（A端 L/R、B端 L/R）
		if (selectedLane >= 0)
		{
			auto& sl = lanes[selectedLane];
			int bx = px + 4;

			PanelWidget::label(pFont, U"A", bx, y, ColorF{ 1.0, 0.5, 0.5 });
			bx += 14;
			PanelWidget::label(pFont, U"L", bx, y);
			bx += 12;
			PanelWidget::numberInput(pFont, sl.aL, 0.25f, -50.f, 50.f, bx, y, 46, kLH, U"{:.2f}");
			bx += 50;
			PanelWidget::label(pFont, U"R", bx, y);
			bx += 12;
			PanelWidget::numberInput(pFont, sl.aR, 0.25f, -50.f, 50.f, bx, y, 46, kLH, U"{:.2f}");
			y += kLH;

			bx = px + 4;
			PanelWidget::label(pFont, U"B", bx, y, ColorF{ 0.5, 1.0, 0.5 });
			bx += 14;
			PanelWidget::label(pFont, U"L", bx, y);
			bx += 12;
			PanelWidget::numberInput(pFont, sl.bL, 0.25f, -50.f, 50.f, bx, y, 46, kLH, U"{:.2f}");
			bx += 50;
			PanelWidget::label(pFont, U"R", bx, y);
			bx += 12;
			PanelWidget::numberInput(pFont, sl.bR, 0.25f, -50.f, 50.f, bx, y, 46, kLH, U"{:.2f}");
			y += kLH;
		}
	}

	// ========================================================================
	// 右下: PanelWidget 単体（長ラベル・ツールチップ複数行）
	// ========================================================================
	void DrawPrimitiveMock(const Font& pFont, int px, int py)
	{
		const int panelW = 380;
		RectF{ px - 2, py - 2, panelW + 4, 180 }.draw(ColorF{ 0.06, 0.06, 0.09, 0.95 });
		RectF{ px - 2, py - 2, panelW + 4, 180 }.drawFrame(1.0, 0.0, ColorF{ 0.3 });

		pFont(U"PanelWidget プリミティブ").draw(Vec2{ px + 6, py - 18 }, ColorF{ 1.0, 0.9, 0.5 });

		int y = py;
		constexpr int kLH = 17;

		// 長ラベル: 小さめの幅を指定して自動幅の効き確認
		static bool t1 = false, t2 = true, t3 = false;
		PanelWidget::label(pFont, U"自動幅ボタン:", px + 6, y);
		y += kLH + 2;
		PanelWidget::toggle(pFont, U"案内標識を編集中（ON）", U"案内標識を編集（OFF）",
			t1, px + 6, y, 40, kLH, U"指定幅 40 だがラベル分に拡張");
		y += kLH + 2;
		PanelWidget::toggle(pFont, U"スタート/ゴール指定モード", U"スタート/ゴール指定モード",
			t2, px + 6, y, 40, kLH);
		y += kLH + 2;
		PanelWidget::button(pFont, U"これは長い日本語ラベルのテスト用ボタンです",
			t3, px + 6, y, 40, kLH);
		y += kLH + 6;

		// ツールチップ強制表示（マウス不要）
		PanelWidget::label(pFont, U"ツールチップ複数行（強制描画）:", px + 6, y);
		y += kLH + 2;

		PanelWidget::tipFont = &pFont;
		PanelWidget::tipText = U"A端左 / ドラッグで幅変更 / Shift で固定 / Ctrl で両側拡張";
		PanelWidget::tipPos = Vec2{ px + 6, y };
		PanelWidget::tipActive = true;
	}
}

void Main()
{
	Window::Resize(1280, 720);
	Scene::SetBackground(ColorF{ 0.12, 0.14, 0.18 });
	RegisterAssets();

	// 状態
	bool autoPlaceMode = true;
	Optional<Vec2> autoPlaceStart = Vec2{ 120, 340 };

	Array<MockRoute> routes = {
		{ 1, RouteKind::Expressway,   1, U"東名高速道路",      ColorF{ 0.3, 0.5, 0.9 } },
		{ 2, RouteKind::National,     4, U"国道4号",            ColorF{ 0.9, 0.3, 0.3 } },
		{ 3, RouteKind::Prefectural,  7, U"県道7号 清水静岡線", ColorF{ 0.95, 0.75, 0.25 } },
		{ 4, RouteKind::Named,        0, U"湾岸道路",           ColorF{ 0.4, 0.8, 0.5 } },
	};
	HashSet<int> pendingRouteIds; pendingRouteIds.emplace(2);
	NewRouteState newRouteState;
	newRouteState.expanded = true;
	newRouteState.nameEdit.text = U"新東名高速道路";

	// エッジモックデータ: テーパー形状を再現する
	Array<MockPart> parts = {
		{ -12.0f, -9.0f, -11.0f, -8.0f, 1 },   // 路肩 (A側が左に張り出し)
		{ -9.0f,  -0.25f, -8.0f, -0.25f, 0 },  // 路盤 左
		{ -0.25f, 0.25f, -0.25f, 0.25f, 2 },   // 中央帯
		{  0.25f,  9.0f,  0.25f,  8.0f, 0 },   // 路盤 右
		{  9.0f, 12.0f,   8.0f, 11.0f, 1 },    // 路肩 (B側が内側に狭まる)
	};
	Array<MockLane> lanes = {
		{ -8.5f, -5.0f, -7.5f, -4.0f, 0 },
		{ -5.0f, -1.5f, -4.0f, -0.5f, 0 },
		{  1.5f,  5.0f,  0.5f,  4.0f, 1 },
		{  5.0f,  8.5f,  4.0f,  7.5f, 1 },
	};
	int selectedLane = 1;

	int frame = 0;
	constexpr int kCaptureFrame = 4;

	while (System::Update())
	{
		// ── 左: 敷設パネル ──
		if (FontAsset::IsRegistered(Asset::CJK14) && FontAsset::IsRegistered(Asset::PanelBold14))
		{
			const Font& pFont = FontAsset(Asset::CJK14);
			const Font& pBold = FontAsset(Asset::PanelBold14);

			DrawDrawTemplatePanelMock(pFont, pBold, 20, 40, 360,
				autoPlaceMode, autoPlaceStart,
				routes, pendingRouteIds, newRouteState);

			// ── 右上: エッジ編集パネル ──
			DrawEdgePanelMock(pFont, pBold, 420, 40, parts, lanes, selectedLane);

			// ── 右下: PanelWidget 単体 ──
			DrawPrimitiveMock(pFont, 420, 300);

			// ツールチップを描画（最後に一度）
			PanelWidget::flushTooltip();
		}

		if (frame == kCaptureFrame)
		{
			ScreenCapture::SaveCurrentFrame(U"ui_review.png");
		}
		if (frame > kCaptureFrame) break;
		++frame;
	}
}
