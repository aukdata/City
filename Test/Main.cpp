# include <Siv3D.hpp> // Siv3D v0.6.16
# include "src/asset/AssetRegistrar.hpp"
# include "src/ui/PanelManager.hpp"
# include "src/ui/PanelWidget.hpp"
# include "src/road/RoadTypes.hpp"

/// @brief 信号フェーズ編集パネルの描画テスト
void Main()
{
	Scene::SetBackground(ColorF{ 0.2, 0.2, 0.25 });
	Window::Resize(900, 600);

	// アセット登録（フォント等）
	RegisterAssets();

	// ---- ダミーデータ: 4方向交差点 ----
	Array<LaneConnection> connections;
	for (int i = 0; i < 8; ++i)
	{
		LaneConnection c;
		c.id = i;
		c.fromEdgeId = i / 2;
		c.fromLaneIndex = i % 2;
		c.toEdgeId = (i / 2 + 2) % 4;
		c.toLaneIndex = i % 2;
		connections << c;
	}

	// 信号フェーズ: 2フェーズ（南北青 / 東西青）
	SignalPlacement sp;
	sp.signalDefId = U"signal_jp_3lamp";
	{
		SignalPhaseDef p1;
		p1.duration = 30.0f;
		p1.greenConnectionIds = { 0, 1, 4, 5 };
		sp.phases << p1;
	}
	{
		SignalPhaseDef p2;
		p2.duration = 25.0f;
		p2.greenConnectionIds = { 2, 3, 6, 7 };
		sp.phases << p2;
	}

	int selectedPhase = 0;

	// ---- PanelManager セットアップ ----
	PanelManager panelMgr;
	panelMgr.registerPanel(U"signal_edit", Vec2{ 700, 500 }, true, true);
	panelMgr.show(U"signal_edit", U"Signal - Node #42 (Test)", Vec2{ 100, 50 });

	// ---- 描画ループ（自動終了） ----
	int frame = 0;
	constexpr int kCaptureFrame = 3;

	while (System::Update())
	{
		panelMgr.handleInput();
		panelMgr.drawBackground(U"signal_edit");

		auto area = panelMgr.beginContent(U"signal_edit");
		if (area)
		{
			const auto pFont = FontAsset(Asset::Panel14);
			const auto pBold = FontAsset(Asset::PanelBold14);
			constexpr int kPad = 6;
			constexpr int kLH = 17;
			constexpr int kLeftW = 180;
			constexpr int kRowH = 38;

			int ly = kPad;

			// ---- 左ペイン: フェーズ一覧 ----
			PanelWidget::label(pBold, U"Phases", kPad, ly, ColorF{ 1.0, 1.0, 0.4 });
			PanelWidget::button(pFont, U"+", false, kLeftW - 26, ly, 20, kLH, U"Add phase");
			ly += kLH + 4;

			const int totalConn = static_cast<int>(connections.size());
			float totalDuration = 0.0f;

			for (int pi = 0; pi < static_cast<int>(sp.phases.size()); ++pi)
			{
				auto& ph = sp.phases[pi];
				totalDuration += ph.duration;

				const bool sel = (pi == selectedPhase);
				const ColorF bg = sel ? ColorF{ 0.25, 0.35, 0.55 } : ColorF{ 0.16 };
				RectF{ static_cast<double>(kPad), static_cast<double>(ly),
				       static_cast<double>(kLeftW - kPad * 2), static_cast<double>(kRowH) }
					.rounded(3).draw(bg);

				int lx = kPad + 4;
				PanelWidget::label(pFont, U"P{}"_fmt(pi + 1), lx, ly + 1,
				                   sel ? ColorF{ 1.0 } : ColorF{ 0.7 });
				lx += 22;

				for (const auto& conn : connections)
				{
					const bool g = ph.greenConnectionIds.contains(conn.id);
					const ColorF lampC = g ? ColorF{ 0.1, 0.9, 0.3 } : ColorF{ 0.9, 0.15, 0.1 };
					Circle{ Vec2{ lx + 3.0, ly + 8.0 }, 2.5 }.draw(lampC);
					lx += 7;
				}

				if (sp.phases.size() > 1)
				{
					PanelWidget::button(pFont, U"x", false, kLeftW - 24, ly + 1, 16, kLH - 2, U"Delete phase");
				}

				PanelWidget::spin(pFont, ph.duration, 1.0f, 5.0f, 120.0f,
				                  kPad + 4, ly + kLH + 1, 56, kLH - 2);
				PanelWidget::label(pFont, U"s  {}/{} green"_fmt(ph.greenConnectionIds.size(), totalConn),
				                   kPad + 62, ly + kLH + 1, ColorF{ 0.55 });

				ly += kRowH + 3;
			}

			ly += 4;
			PanelWidget::label(pBold, U"Cycle: {:.0f}s"_fmt(totalDuration), kPad, ly, ColorF{ 0.9, 0.8, 0.4 });
			ly += kLH + 4;

			// ---- 右ペイン: 簡易交差点図 ----
			const Vec2 panelSize = panelMgr.getSize(U"signal_edit");
			const double rightW = panelSize.x - kLeftW;
			const double diagramSize = Min(rightW - kPad, 380.0);
			const Vec2 center{ kLeftW + rightW * 0.5, kPad + diagramSize * 0.5 };
			constexpr double armLen = 80.0;

			Circle{ center, diagramSize * 0.25 }.draw(ColorF{ 0.25, 0.25, 0.28 });

			const Array<Vec2> dirs = { Vec2{0, -1}, Vec2{1, 0}, Vec2{0, 1}, Vec2{-1, 0} };
			const Array<String> labels = { U"E0", U"E1", U"E2", U"E3" };

			for (int i = 0; i < 4; ++i)
			{
				const Vec2& d = dirs[i];
				const Vec2 armStart = center + d * 30.0;
				const Vec2 armEnd = center + d * (30.0 + armLen);
				const Vec2 perp{ -d.y, d.x };
				constexpr double hw = 12.0;

				Quad{ armStart + perp * hw, armStart - perp * hw,
				      armEnd - perp * hw, armEnd + perp * hw }
					.draw(ColorF{ 0.35, 0.35, 0.38 });
				Line{ armStart + perp * hw, armEnd + perp * hw }.draw(1.5, ColorF{ 1.0 });
				Line{ armStart - perp * hw, armEnd - perp * hw }.draw(1.5, ColorF{ 1.0 });

				for (double dd = 0; dd < armLen; dd += 10.0)
				{
					Line{ armStart + d * dd, armStart + d * Min(dd + 4.0, armLen) }
						.draw(1.0, ColorF{ 1.0, 1.0, 1.0, 0.5 });
				}

				pFont(labels[i]).drawAt(armEnd + d * 14.0, ColorF{ 0.8 });
			}

			// LaneConnection パス
			HashSet<int> greenSet;
			if (selectedPhase >= 0 && selectedPhase < static_cast<int>(sp.phases.size()))
			{
				for (int cid : sp.phases[selectedPhase].greenConnectionIds)
					greenSet.insert(cid);
			}

			for (const auto& conn : connections)
			{
				const Vec2 from = center + dirs[conn.fromEdgeId] * 25.0;
				const Vec2 to = center + dirs[conn.toEdgeId] * 25.0;
				const bool isGreen = greenSet.contains(conn.id);
				const ColorF lineC = isGreen
					? ColorF{ 0.2, 0.95, 0.4, 0.85 }
					: ColorF{ 0.95, 0.25, 0.15, 0.55 };
				const Vec2 perp1{ -dirs[conn.fromEdgeId].y, dirs[conn.fromEdgeId].x };
				const Vec2 off = perp1 * (conn.fromLaneIndex == 0 ? -4.0 : 4.0);

				Line{ from + off, to + off }.draw(isGreen ? 2.5 : 1.5, lineC);

				const Vec2 mid = (from + off + to + off) * 0.5;
				Circle{ mid, 6.0 }.draw(isGreen ? ColorF{ 0.1, 0.95, 0.35 } : ColorF{ 0.95, 0.2, 0.1 });
				Circle{ mid, 6.0 }.drawFrame(1.2, ColorF{ 0.0, 0.0, 0.0, 0.7 });
			}

			PanelWidget::flushTooltip();
			const int totalHeight = Max(ly, static_cast<int>(diagramSize) + kPad * 2);
			panelMgr.reportContentHeight(U"signal_edit", totalHeight);
		}

		if (frame == kCaptureFrame)
		{
			ScreenCapture::SaveCurrentFrame(U"signal_edit_test.png");
		}
		if (frame > kCaptureFrame)
		{
			break;
		}
		++frame;
	}

	Logger << U"[Test] signal_edit_test.png saved.";
}
