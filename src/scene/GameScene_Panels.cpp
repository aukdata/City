#include "GameScene.hpp"
#include "../ui/PanelWidget.hpp"

namespace
{
	const Font& panelFont()
	{
		static const Font f{ FontMethod::MSDF, 14 };
		return f;
	}
	const Font& panelBoldFont()
	{
		static const Font f{ FontMethod::MSDF, 14, Typeface::Bold };
		return f;
	}

	/// @brief Parts を offset 昇順にソートし、隙間・重なりを除去する
	void resolvePartOverlapAndGap(Array<RoadPart>& parts)
	{
		if (parts.size() < 2) return;
		parts.sort_by([](const RoadPart& a, const RoadPart& b) { return a.offset < b.offset; });
		for (size_t i = 1; i < parts.size(); ++i)
			parts[i].offset = parts[i - 1].offset + parts[i - 1].width;
	}

	/// @brief Lanes を offsetA_L 昇順にソートし、重なりを押し出す（隙間は許容）
	void resolveLaneOverlap(Array<Lane>& lanes)
	{
		if (lanes.size() < 2) return;
		lanes.sort_by([](const Lane& a, const Lane& b) { return a.offsetA_L < b.offsetA_L; });
		for (size_t i = 1; i < lanes.size(); ++i)
		{
			// A端
			const float overA = lanes[i - 1].offsetA_R - lanes[i].offsetA_L;
			if (overA > 0.001f) { lanes[i].offsetA_L += overA; lanes[i].offsetA_R += overA; }
			// B端
			const float overB = lanes[i - 1].offsetB_R - lanes[i].offsetB_L;
			if (overB > 0.001f) { lanes[i].offsetB_L += overB; lanes[i].offsetB_R += overB; }
		}
	}

	/// @brief LineType → 断面バー描画用の色
	ColorF lineTypeColor(LineType lt)
	{
		switch (lt)
		{
		case LineType::SolidWhite:  return ColorF{1.0, 1.0, 1.0};
		case LineType::DashedWhite: return ColorF{1.0, 1.0, 1.0, 0.5};
		case LineType::SolidYellow: return ColorF{1.0, 0.85, 0.0};
		case LineType::DoubleYellow:return ColorF{1.0, 0.85, 0.0};
		default:                    return ColorF{0, 0, 0, 0};
		}
	}
}

// =============================================================================
// 地名リストパネル
// =============================================================================

void GameScene::drawNameListPanel()
{
	auto area = m_panelManager.beginContent(U"name_list");
	if (!area) return;

	static const Font listFont{ FontMethod::MSDF, 14 };
	constexpr int kLineH = 22;
	constexpr int kPad = 8;

	double y = kPad;

	for (size_t idx = 0; idx < m_districts.size(); ++idx)
	{
		const auto& s = m_districts[idx];
		StringView typeStr;
		ColorF typeColor;
		switch (s.type)
		{
		case MapGenerator::SettlementType::Urban:
			typeStr = U"[U]"; typeColor = ColorF{ 1.0, 0.4, 0.4 }; break;
		case MapGenerator::SettlementType::Suburbs:
			typeStr = U"[S]"; typeColor = ColorF{ 0.4, 0.8, 1.0 }; break;
		default:
			typeStr = U"[R]"; typeColor = ColorF{ 0.6, 0.8, 0.5 }; break;
		}

		const RectF itemRect{ static_cast<double>(kPad), y,
			240.0 - kPad * 2, static_cast<double>(kLineH) };
		const bool hovered = itemRect.mouseOver();

		if (hovered)
			itemRect.draw(ColorF{ 1, 1, 1, 0.1 });

		listFont(typeStr).draw(Vec2{ kPad, y + 2 }, typeColor);
		listFont(s.name).draw(Vec2{ kPad + 30, y + 2 },
			hovered ? Palette::Yellow : Palette::White);

		if (hovered && MouseL.down())
		{
			const float h = m_world.computeHeight(
				static_cast<float>(s.center.x), static_cast<float>(s.center.y));
			m_camera.setFocus(Vec3{ s.center.x, h, s.center.y });
			if (m_camera.mode() != CameraMode::Overview)
				m_camera.cycleMode();
		}

		y += kLineH;
	}
	m_panelManager.reportContentHeight(U"name_list", y);
}

// =============================================================================
// 道路エッジ編集パネル
// =============================================================================

void GameScene::drawEdgePanel()
{
	if (!m_selectedEdgeId) return;
	RoadEdge* edge = m_network.getEdge(*m_selectedEdgeId);
	if (!edge) { m_selectedEdgeId = none; return; }

	auto area = m_panelManager.beginContent(U"edge_info");
	if (!area) return;

	const auto& pFont = panelFont();
	const auto& pBold = panelBoldFont();

	constexpr int kPad = 6;
	constexpr int kLH = 17;
	const int pX = kPad;
	int y = 0;
	bool dirty = false;

	PanelWidget::label(pFont, U"A:{}  B:{}  {:.0f}m"_fmt(edge->nodeA, edge->nodeB, edge->length), pX, y, ColorF{1.0});
	if (PanelWidget::button(pFont, U"Swap A/B", false, pX + 200, y, 62, kLH, U"Swap nodeA/B"))
	{
		std::swap(edge->nodeA, edge->nodeB);
		std::swap(edge->ctrlA, edge->ctrlB);
		std::swap(edge->cutoffA, edge->cutoffB);
		for (auto& L : edge->lanes)
		{
			std::swap(L.offsetA_L, L.offsetB_L);
			std::swap(L.offsetA_R, L.offsetB_R);
		}
		dirty = true;
	}
	y += kLH + 2;

	// 道路種別
	{
		static constexpr StringView rtNames[] = { U"Local", U"Arterial", U"Express", U"Highway" };
		PanelWidget::label(pFont, U"Type", pX, y, ColorF{ 0.6 });
		if (PanelWidget::cycle(pFont, edge->roadType, rtNames, 4, pX + 34, y, 60, kLH)) dirty = true;
		y += kLH + 2;
	}

	// 速度制限
	{
		PanelWidget::label(pFont, U"Speed", pX, y, ColorF{ 0.6 });
		if (PanelWidget::spin(pFont, edge->speedLimit, 10.0f, 10.0f, 200.0f, pX + 44, y, 44, kLH, U"{:.0f}")) dirty = true;
		PanelWidget::label(pFont, U"km/h", pX + 90, y, ColorF{ 0.5 });
		PanelWidget::label(pFont, U"W:{:.1f}m"_fmt(edge->totalWidth()), pX + 130, y);

		// デバッグ: 車両スポーン
		if (PanelWidget::button(pFont, U"Spawn", false, pX + 200, y, 48, kLH, U"Spawn vehicle on this edge"))
		{
			m_vehicleManager.spawnOnEdge(edge->id, *m_simGraph);
		}
		y += kLH;

		// 高架トグル
		if (PanelWidget::toggle(pFont, U"Elevated ON", U"Elevated", edge->useElevation, pX, y, 80, kLH))
		{
			if (edge->useElevation)
				m_network.generatePiersForEdge(edge->id, m_world);
			else
				m_network.removeObjectsByEdge(edge->id);
			dirty = true;
		}
		y += kLH + 4;
	}

	// ── 断面共通パラメータ ──
	constexpr int kBarX = kPad;
	constexpr int kBarW = 356;
	constexpr int kPartBarH = 36;
	constexpr int kLaneBarH = 28;
	constexpr int kEdgeGrab = 4;  // 端ドラッグ判定幅 [px]
	constexpr int kSectionW = 360;

	// 全パーツ+車線の最小/最大オフセットを求めてスケーリング
	float extMin = 1e9f, extMax = -1e9f;
	for (const auto& p : edge->parts)
	{
		extMin = Min(extMin, p.offset);
		extMax = Max(extMax, p.offset + p.width);
	}
	for (const auto& L : edge->lanes)
	{
		extMin = Min(extMin, Min(L.offsetA_L, L.offsetB_L));
		extMax = Max(extMax, Max(L.offsetA_R, L.offsetB_R));
	}
	if (extMin >= extMax) { extMin = -5.0f; extMax = 5.0f; }
	const float margin = (extMax - extMin) * 0.08f + 0.5f;
	extMin -= margin;
	extMax += margin;
	const float extRange = extMax - extMin;

	// メートル → ピクセル変換ラムダ
	auto mToPixel = [&](float m) -> double { return kBarX + (m - extMin) / extRange * kBarW; };
	auto pixelToM = [&](double px) -> float { return extMin + static_cast<float>((px - kBarX) / kBarW) * extRange; };

	// パーツ種別色
	static constexpr ColorF partColors[] = {
		ColorF{0.25, 0.25, 0.27},  // Roadbed  アスファルト暗灰
		ColorF{0.35, 0.33, 0.30},  // Shoulder 路肩（暗い砂利色）
		ColorF{0.45, 0.55, 0.30},  // Median   中央帯（緑地）
		ColorF{0.60, 0.58, 0.55},  // Sidewalk コンクリート歩道
		ColorF{0.20, 0.20, 0.22},  // Gutter   側溝（暗灰）
		ColorF{0.55, 0.55, 0.55},  // Guard    ガードレール（金属灰）
		ColorF{0.45, 0.42, 0.38},  // Wall     擁壁（コンクリート）
		ColorF{0.50, 0.48, 0.44},  // Curb     縁石
		ColorF{0.40, 0.52, 0.30},  // Slope    のり面（草地）
		ColorF{0.30, 0.45, 0.55},  // BikeLane 自転車レーン（青系）
	};

	// ── ドラッグ状態 (static) ──
	// dragMode: 0=none, 1=part center, 2=part left edge, 3=part right edge
	//           4=lane center, 5=lane left edge, 6=lane right edge
	static int  selectedPart = -1;
	static int  selectedLane = -1;
	static int  dragMode = 0;
	static float dragAnchor = 0.0f;  // ドラッグ開始時の offset バックアップ

	// パーツ/車線インデックスの範囲チェック
	if (selectedPart >= static_cast<int>(edge->parts.size())) selectedPart = -1;
	if (selectedLane >= static_cast<int>(edge->lanes.size())) selectedLane = -1;

	// ── ドラッグ更新（毎フレーム） ──
	if (dragMode != 0 && MouseL.pressed())
	{
		const float curM = pixelToM(Cursor::PosF().x);
		const float delta = curM - dragAnchor;

		if (dragMode >= 1 && dragMode <= 3 && selectedPart >= 0)
		{
			auto& p = edge->parts[selectedPart];
			if (dragMode == 1)      { p.offset += delta; dragAnchor = curM; dirty = true; }
			else if (dragMode == 2) { const float dw = delta; p.offset += dw; p.width -= dw; if (p.width < 0.5f) { p.offset -= (0.5f - p.width); p.width = 0.5f; } dragAnchor = curM; dirty = true; }
			else if (dragMode == 3) { p.width += delta; if (p.width < 0.5f) p.width = 0.5f; dragAnchor = curM; dirty = true; }
		}
		else if (dragMode >= 4 && dragMode <= 6 && selectedLane >= 0)
		{
			auto& L = edge->lanes[selectedLane];
			if (dragMode == 4) { L.offsetA_L += delta; L.offsetA_R += delta; L.offsetB_L += delta; L.offsetB_R += delta; dragAnchor = curM; dirty = true; }
			else if (dragMode == 5) { L.offsetA_L += delta; L.offsetB_L += delta; dragAnchor = curM; dirty = true; }
			else if (dragMode == 6) { L.offsetA_R += delta; L.offsetB_R += delta; dragAnchor = curM; dirty = true; }
		}
	}
	else if (dragMode != 0)
	{
		dragMode = 0;
	}

	// ========== Parts セクション ==========
	static bool partsCollapsed = false;
	{
		static constexpr StringView ptNames[] = { U"Roadbed", U"Shoulder", U"Median", U"Sidewalk",
			U"Gutter", U"Guard", U"Wall", U"Curb", U"Slope", U"Bike" };
		static constexpr StringView bsNames[] = { U"NotBuilt", U"Building", U"Built", U"Stub" };

		if (PanelWidget::section(pBold, U"Parts ({})"_fmt(edge->parts.size()), partsCollapsed, pX, y, kSectionW, kLH))
		{
			// [+] ボタン
			if (PanelWidget::button(pFont, U"+", false, pX + 4, y, 16, kLH, U"Add part"))
			{
				RoadPart np;
				np.type = RoadPartType::Roadbed; np.width = 3.5f;
				np.offset = edge->totalWidth() * 0.5f; np.build = BuildState::Built;
				edge->parts << np; dirty = true;
			}
			y += kLH;

			// ── 断面バー描画 ──
			const int barY = y;
			RectF{ static_cast<double>(kBarX), static_cast<double>(barY),
			       static_cast<double>(kBarW), static_cast<double>(kPartBarH) }
				.draw(ColorF{ 0.08, 0.08, 0.10 });

			// 中心線
			const double centerPx = mToPixel(0.0f);
			if (centerPx > kBarX && centerPx < kBarX + kBarW)
				RectF{ centerPx - 0.5, static_cast<double>(barY), 1.0, static_cast<double>(kPartBarH) }
					.draw(ColorF{ 1.0, 1.0, 1.0, 0.3 });

			for (int i = 0; i < static_cast<int>(edge->parts.size()); ++i)
			{
				const auto& p = edge->parts[i];
				const double px0 = mToPixel(p.offset);
				const double px1 = mToPixel(p.offset + p.width);
				const double pw = Max(px1 - px0, 2.0);
				const bool sel = (i == selectedPart);

				ColorF col = partColors[Clamp(static_cast<int>(p.type), 0, 9)];
				if (p.build != BuildState::Built) col = col * 0.5;

				RectF rect{ px0, static_cast<double>(barY + 2), pw, static_cast<double>(kPartBarH - 4) };
				rect.draw(sel ? col.lerp(ColorF{1.0}, 0.25) : col);
				rect.drawFrame(1.0, sel ? ColorF{1.0, 1.0, 0.3} : ColorF{0.3, 0.3, 0.3});

				// ラベル（幅に余裕があれば）
				if (pw > 20)
					pFont(ptNames[static_cast<int>(p.type)]).draw(8.0,
						Vec2{ px0 + 2, static_cast<double>(barY + 3) }, ColorF{1.0, 1.0, 1.0, 0.9});

				// クリック/ドラッグ判定
				if (dragMode == 0 && rect.mouseOver())
				{
					const double mx = Cursor::PosF().x;
					if (MouseL.down())
					{
						selectedPart = i;
						selectedLane = -1;
						dragAnchor = pixelToM(mx);
						if (mx - px0 < kEdgeGrab && pw > 10)      dragMode = 2;  // left edge
						else if (px1 - mx < kEdgeGrab && pw > 10) dragMode = 3;  // right edge
						else                                       dragMode = 1;  // center
					}
					else if (MouseR.down())
					{
						// 右クリック → type サイクル
						edge->parts[i].type = static_cast<RoadPartType>(
							(static_cast<int>(edge->parts[i].type) + 1) % 10);
						dirty = true;
					}
				}
			}
			y += kPartBarH + 2;

			// ── 選択パーツの詳細行 ──
			if (selectedPart >= 0 && selectedPart < static_cast<int>(edge->parts.size()))
			{
				auto& sp = edge->parts[selectedPart];
				int bx = pX;

				PanelWidget::label(pBold, U"[{}]"_fmt(selectedPart), bx, y, ColorF{1.0, 1.0, 0.5});
				bx += 24;
				if (PanelWidget::cycle(pFont, sp.type, ptNames, 10, bx, y, 56, kLH)) dirty = true;
				bx += 58;
				if (PanelWidget::cycle(pFont, sp.build, bsNames, 4, bx, y, 52, kLH)) dirty = true;
				bx += 58;
				if (PanelWidget::button(pFont, U"X", false, bx, y, 18, kLH, U"Remove"))
				{
					edge->parts.remove_at(selectedPart);
					selectedPart = -1;
					dirty = true;
				}
				y += kLH;

				if (selectedPart >= 0)
				{
					bx = pX + 4;
					PanelWidget::label(pFont, U"w", bx, y, ColorF{0.6});
					if (PanelWidget::spin(pFont, sp.width, 0.25f, 0.5f, 50.0f, bx + 12, y, 44, kLH, U"{:.2f}")) dirty = true;
					bx += 62;
					PanelWidget::label(pFont, U"offset", bx, y, ColorF{0.6});
					if (PanelWidget::spin(pFont, sp.offset, 0.25f, -50.0f, 50.0f, bx + 46, y, 48, kLH, U"{:.2f}")) dirty = true;
					y += kLH;
				}
			}
		}
		y += 4;
	}

	// ========== Lanes セクション ==========
	static bool lanesCollapsed = false;
	{
		static constexpr StringView osN[] = { U"Open", U"Provisional", U"Closed", U"Reserved" };
		static constexpr StringView ltN[] = { U"Normal", U"Bus", U"Climb", U"Turn", U"Accel", U"Decel" };
		static constexpr StringView lnN[] = { U"None", U"Solid W", U"Dash W", U"Solid Y", U"Double Y" };
		static constexpr StringView drN[] = { U"Forward", U"Backward" };

		if (PanelWidget::section(pBold, U"Lanes ({})"_fmt(edge->lanes.size()), lanesCollapsed, pX, y, kSectionW, kLH))
		{
			// [+] ボタン
			if (PanelWidget::button(pFont, U"+", false, pX + 4, y, 16, kLH, U"Add lane"))
			{
				Lane nl; nl.dir = LaneDir::Forward; nl.op = OpState::Open; nl.nominalWidth = 3.5f;
				const float hw = edge->totalWidth() * 0.5f;
				nl.offsetA_L = hw; nl.offsetA_R = hw + 3.5f; nl.offsetB_L = hw; nl.offsetB_R = hw + 3.5f;
				edge->lanes << nl; dirty = true;
			}
			y += kLH;

			// ── 車線バー描画 ──
			const int laneBarY = y;
			RectF{ static_cast<double>(kBarX), static_cast<double>(laneBarY),
			       static_cast<double>(kBarW), static_cast<double>(kLaneBarH) }
				.draw(ColorF{ 0.08, 0.08, 0.10 });

			// 中心線
			const double centerPx = mToPixel(0.0f);
			if (centerPx > kBarX && centerPx < kBarX + kBarW)
				RectF{ centerPx - 0.5, static_cast<double>(laneBarY), 1.0, static_cast<double>(kLaneBarH) }
					.draw(ColorF{ 1.0, 1.0, 1.0, 0.3 });

			for (int i = 0; i < static_cast<int>(edge->lanes.size()); ++i)
			{
				const auto& L = edge->lanes[i];
				const double px0 = mToPixel(L.offsetA_L);
				const double px1 = mToPixel(L.offsetA_R);
				const double pw = Max(px1 - px0, 2.0);
				const bool sel = (i == selectedLane);

				// 路面色（アスファルト暗灰ベース、状態で変化）
				ColorF col{0.25, 0.25, 0.27};
				if (L.op == OpState::Closed)          col = ColorF{0.18, 0.18, 0.18};
				else if (L.op == OpState::Reserved)   col = ColorF{0.22, 0.20, 0.25};
				else if (L.op == OpState::Provisional) col = ColorF{0.28, 0.27, 0.22};

				RectF rect{ px0, static_cast<double>(laneBarY + 2), pw, static_cast<double>(kLaneBarH - 4) };
				rect.draw(sel ? col.lerp(ColorF{1.0}, 0.15) : col);
				if (sel) rect.drawFrame(1.0, ColorF{1.0, 1.0, 0.3});

				// 方向矢印（Forward=白、Backward=薄赤）
				if (pw > 14)
				{
					const StringView arrow = (L.dir == LaneDir::Forward) ? U"\u2192" : U"\u2190";
					const ColorF arrowCol = (L.dir == LaneDir::Forward)
						? ColorF{1.0, 1.0, 1.0, 0.6} : ColorF{1.0, 0.5, 0.5, 0.6};
					pBold(arrow).drawAt(10.0,
						Vec2{ (px0 + px1) * 0.5, laneBarY + kLaneBarH * 0.5 }, arrowCol);
				}

				// 左側ライン (lineLeft)
				{
					const ColorF lc = lineTypeColor(L.lineLeft);
					if (lc.a > 0.01)
					{
						const double lw = (L.lineLeft == LineType::DoubleYellow) ? 3.0 : 1.0;
						RectF{ px0 - lw * 0.5, static_cast<double>(laneBarY + 2), lw,
						       static_cast<double>(kLaneBarH - 4) }.draw(lc);
					}
				}
				// 右側ライン (lineRight)
				{
					const ColorF lc = lineTypeColor(L.lineRight);
					if (lc.a > 0.01)
					{
						const double lw = (L.lineRight == LineType::DoubleYellow) ? 3.0 : 1.0;
						RectF{ px1 - lw * 0.5, static_cast<double>(laneBarY + 2), lw,
						       static_cast<double>(kLaneBarH - 4) }.draw(lc);
					}
				}

				// クリック/ドラッグ判定
				if (dragMode == 0 && rect.mouseOver())
				{
					const double mx = Cursor::PosF().x;
					if (MouseL.down())
					{
						selectedLane = i;
						selectedPart = -1;
						dragAnchor = pixelToM(mx);
						if (mx - px0 < kEdgeGrab && pw > 10)      dragMode = 5;
						else if (px1 - mx < kEdgeGrab && pw > 10) dragMode = 6;
						else                                       dragMode = 4;
					}
					else if (MouseR.down())
					{
						edge->lanes[i].dir = (L.dir == LaneDir::Forward) ? LaneDir::Backward : LaneDir::Forward;
						dirty = true;
					}
				}
			}
			y += kLaneBarH + 2;

			// ── 選択車線の詳細行 ──
			if (selectedLane >= 0 && selectedLane < static_cast<int>(edge->lanes.size()))
			{
				auto& sl = edge->lanes[selectedLane];
				int bx = pX;

				PanelWidget::label(pBold, U"Lane {}"_fmt(selectedLane), bx, y, ColorF{0.8, 0.8, 1.0});
				bx += 46;
				dirty |= PanelWidget::cycle(pFont, sl.dir, drN, 2, bx, y, 66, kLH);
				bx += 68;
				dirty |= PanelWidget::cycle(pFont, sl.op, osN, 4, bx, y, 78, kLH);
				bx += 80;
				dirty |= PanelWidget::cycle(pFont, sl.type, ltN, 6, bx, y, 50, kLH);
				bx += 54;
				if (PanelWidget::button(pFont, U"X", false, bx, y, 18, kLH, U"Remove"))
				{
					edge->lanes.remove_at(selectedLane);
					selectedLane = -1;
					dirty = true;
				}
				y += kLH;

				if (selectedLane >= 0)
				{
					bx = pX + 4;
					PanelWidget::label(pFont, U"Width", bx, y, ColorF{0.6});
					bx += 40;
					dirty |= PanelWidget::spin(pFont, sl.nominalWidth, 0.5f, 1.0f, 10.0f, bx, y, 38, kLH);
					bx += 44;
					dirty |= PanelWidget::toggle(pFont, U"L OK", U"L --", sl.canChangeLaneLeft, bx, y, 36, kLH);
					bx += 38;
					dirty |= PanelWidget::toggle(pFont, U"R OK", U"R --", sl.canChangeLaneRight, bx, y, 36, kLH);
					y += kLH;

					bx = pX + 4;
					PanelWidget::label(pFont, U"Line L", bx, y, ColorF{0.6});
					bx += 48;
					dirty |= PanelWidget::cycle(pFont, sl.lineLeft, lnN, 5, bx, y, 66, kLH);
					bx += 70;
					PanelWidget::label(pFont, U"R", bx, y, ColorF{0.6});
					bx += 14;
					dirty |= PanelWidget::cycle(pFont, sl.lineRight, lnN, 5, bx, y, 66, kLH);
					y += kLH;

					bx = pX + 4;
					PanelWidget::label(pFont, U"A", bx, y, ColorF{1.0, 0.5, 0.5});
					bx += 14;
					PanelWidget::label(pFont, U"L", bx, y);
					bx += 12;
					dirty |= PanelWidget::spin(pFont, sl.offsetA_L, 0.25f, -50.f, 50.f, bx, y, 46, kLH, U"{:.2f}");
					bx += 50;
					PanelWidget::label(pFont, U"R", bx, y);
					bx += 12;
					dirty |= PanelWidget::spin(pFont, sl.offsetA_R, 0.25f, -50.f, 50.f, bx, y, 46, kLH, U"{:.2f}");
					y += kLH;

					bx = pX + 4;
					PanelWidget::label(pFont, U"B", bx, y, ColorF{0.5, 1.0, 0.5});
					bx += 14;
					PanelWidget::label(pFont, U"L", bx, y);
					bx += 12;
					dirty |= PanelWidget::spin(pFont, sl.offsetB_L, 0.25f, -50.f, 50.f, bx, y, 46, kLH, U"{:.2f}");
					bx += 50;
					PanelWidget::label(pFont, U"R", bx, y);
					bx += 12;
					dirty |= PanelWidget::spin(pFont, sl.offsetB_R, 0.25f, -50.f, 50.f, bx, y, 46, kLH, U"{:.2f}");
					y += kLH + 2;
				}
			}
		}
	}

	PanelWidget::flushTooltip();
	m_panelManager.reportContentHeight(U"edge_info", y);

	if (dirty)
	{
		resolvePartOverlapAndGap(edge->parts);
		resolveLaneOverlap(edge->lanes);
		m_roadRenderer.invalidateEdgeCache(edge->id, edge->nodeA, edge->nodeB);
		m_roadRenderer.invalidateCachesAroundNode(edge->nodeA, m_network);
		m_roadRenderer.invalidateCachesAroundNode(edge->nodeB, m_network);
	}
}

// =============================================================================
// ノード編集パネル
// =============================================================================

void GameScene::drawNodePanel()
{
	if (!m_selectedNodeId) return;
	RoadNode* node = m_network.getNode(*m_selectedNodeId);
	if (!node) { m_selectedNodeId = none; return; }

	auto area = m_panelManager.beginContent(U"node_info");
	if (!area) return;

	const auto& pFont = panelFont();
	const auto& pBold = panelBoldFont();

	constexpr int kPad = 6;
	constexpr int kLH = 17;
	const int pX = kPad;
	int y = 0;
	bool dirty = false;

	PanelWidget::label(pFont, U"pos: ({:.0f}, {:.1f}, {:.0f})"_fmt(
		node->position.x, node->position.y, node->position.z), pX, y, ColorF{1.0});
	y += kLH + 4;

	{
		static constexpr StringView ntNames[] = { U"Endpoint", U"Joint", U"Intersect", U"Diverge" };
		PanelWidget::label(pFont, U"Type", pX, y);
		dirty |= PanelWidget::cycle(pFont, node->type, ntNames, 4, pX + 36, y, 70, kLH);
		y += kLH + 2;
	}

	{
		static constexpr StringView trNames[] = { U"Blend", U"Abrupt" };
		PanelWidget::label(pFont, U"Trans", pX, y);
		dirty |= PanelWidget::cycle(pFont, node->transition, trNames, 2, pX + 42, y, 54, kLH);
		y += kLH + 4;
	}

	// Attachments セクション（折りたたみ可能）
	static bool attachCollapsed = false;
	{
		static constexpr StringView rtNames[] = { U"Local", U"Arterial", U"Express", U"Highway" };
		constexpr int kSectionW = 300;

		if (PanelWidget::section(pBold, U"Attachments ({})"_fmt(node->attachments.size()), attachCollapsed, pX, y, kSectionW, kLH))
		{
			for (size_t i = 0; i < node->attachments.size(); ++i)
			{
				auto& att = node->attachments[i];
				const RoadEdge* e = m_network.getEdge(att.edgeId);

				PanelWidget::label(pFont, U"[{}] edge #{}"_fmt(i, att.edgeId), pX, y, ColorF{1.0});
				if (e)
					PanelWidget::label(pFont, U"{} {:.0f}km/h"_fmt(rtNames[static_cast<int>(e->roadType)], e->speedLimit), pX + 100, y);
				y += kLH;

				PanelWidget::label(pFont, U"lat", pX + 10, y);
				dirty |= PanelWidget::spin(pFont, att.lateralOffset, 1.0f, -20.0f, 20.0f, pX + 34, y, 44, kLH);
				dirty |= PanelWidget::toggle(pFont, U"THROUGH", U"through", att.isThrough, pX + 84, y, 60, kLH);
				y += kLH;

				{
					static constexpr StringView tcNames[] = { U"None", U"Yield", U"Stop", U"Signal" };
					PanelWidget::label(pFont, U"ctrl", pX + 10, y);
					if (PanelWidget::cycle(pFont, att.control, tcNames, 4, pX + 34, y, 52, kLH))
					{
						dirty = true;
						notifyNetworkChanged();
					}
				}
				y += kLH + 2;
			}
		}
	}

	PanelWidget::flushTooltip();
	m_panelManager.reportContentHeight(U"node_info", y);

	if (dirty)
	{
		m_network.updateNodeCutoffs(node->id);
		m_roadRenderer.invalidateCachesAroundNode(node->id, m_network);
	}
}

// =============================================================================
// 車両情報パネル
// =============================================================================

void GameScene::drawVehiclePanel()
{
	if (!m_selectedVehicleId) return;

	const Vehicle* veh = nullptr;
	for (const auto& v : m_vehicleManager.vehicles())
	{
		if (v.id == *m_selectedVehicleId) { veh = &v; break; }
	}
	if (!veh)
	{
		m_selectedVehicleId = none;
		m_trackingVehicle = false;
		m_panelManager.hide(U"vehicle_info");
		return;
	}

	auto area = m_panelManager.beginContent(U"vehicle_info");
	if (!area) return;

	const auto& pFont = panelFont();
	const auto& pBold = panelBoldFont();

	constexpr int kPad = 6;
	constexpr int kLH = 17;
	const int pX = kPad;
	int y = 0;

	static constexpr StringView typeNames[] = {
		U"PassengerCar", U"KeiCar", U"Moped", U"LightVehicle",
		U"Bus", U"SmallTruck", U"LargeTruck", U"Emergency"
	};
	const int typeIdx = static_cast<int>(veh->type);
	PanelWidget::label(pFont, U"Type: {}"_fmt(typeIdx < 8 ? typeNames[typeIdx] : U"?"), pX, y, ColorF{1.0});
	y += kLH;

	PanelWidget::label(pFont, U"Speed: {:.1f} km/h"_fmt(veh->speed * 3.6f), pX, y, ColorF{1.0});
	y += kLH;

	static constexpr StringView locNames[] = { U"OnLane", U"OnConnection", U"ChangingLane" };
	PanelWidget::label(pFont, U"Location: {}"_fmt(locNames[static_cast<int>(veh->location)]), pX, y, ColorF{1.0});
	y += kLH;

	PanelWidget::label(pFont, U"Edge: {}  Lane: {}"_fmt(veh->currentEdge, veh->currentLane), pX, y, ColorF{1.0});
	y += kLH;

	PanelWidget::label(pFont, U"Goal Edge: {}"_fmt(veh->goalEdgeId), pX, y, ColorF{1.0});
	// 選択中エッジをゴールに設定するボタン
	if (m_selectedEdgeId)
	{
		if (PanelWidget::button(pFont, U"Set E{}"_fmt(*m_selectedEdgeId), false,
		                        pX + 120, y, 70, kLH, U"Set selected edge as goal"))
		{
			m_vehicleManager.setGoalAndReroute(veh->id, *m_selectedEdgeId, *m_simGraph);
		}
	}
	y += kLH + 4;

	// 追跡ボタン
	{
		const RectF btn{ static_cast<double>(pX), static_cast<double>(y), 120.0, static_cast<double>(kLH + 2) };
		const bool hover = btn.mouseOver();
		btn.draw(m_trackingVehicle ? ColorF{ 0.2, 0.5, 0.8, 0.8 } : (hover ? ColorF{ 0.3, 0.3, 0.3, 0.8 } : ColorF{ 0.2, 0.2, 0.2, 0.6 }));
		PanelWidget::label(pBold, m_trackingVehicle ? U"Tracking ON" : U"Track", pX + 4, y, ColorF{1.0});
		if (hover && MouseL.down())
			m_trackingVehicle = !m_trackingVehicle;
		y += kLH + 6;
	}

	// 経路ウェイポ���ント
	const int wpCount = static_cast<int>(veh->routeWaypoints.size());
	PanelWidget::label(pBold, U"Route: {}/{} waypoints"_fmt(veh->routeIdx, wpCount), pX, y, ColorF{1.0, 1.0, 0.4});
	y += kLH + 2;

	if (wpCount == 0)
	{
		PanelWidget::label(pFont, veh->routeRequested ? U"(requesting...)" : U"(no route)", pX, y, ColorF{0.6});
		y += kLH;
	}

	const int showStart = Max(0, veh->routeIdx - 2);
	const int showEnd   = Min(wpCount, veh->routeIdx + 10);
	for (int i = showStart; i < showEnd; ++i)
	{
		const auto& wp = veh->routeWaypoints[i];
		const bool isCurrent = (i == veh->routeIdx);
		const bool isPast    = (i < veh->routeIdx);

		const RoadEdge* edge = m_network.getEdge(wp.edgeId);
		const String label = U"{} E:{} L:{} {:.0f}m"_fmt(
			isCurrent ? U">" : (isPast ? U" " : U" "),
			wp.edgeId, wp.laneIndex, wp.edgeLength);

		const RectF itemRect{ static_cast<double>(pX), static_cast<double>(y),
			260.0, static_cast<double>(kLH) };
		const bool itemHover = itemRect.mouseOver();

		if (itemHover)
			itemRect.draw(ColorF{ 0.3, 0.3, 0.5, 0.4 });

		const ColorF color = isPast ? ColorF{ 0.4 }
			: (isCurrent ? ColorF{ 0.0, 1.0, 1.0 }
			: (itemHover ? ColorF{ 1.0, 1.0, 0.0 } : ColorF{ 1.0 }));
		PanelWidget::label(pFont, label, pX + 2, y, color);

		if (itemHover && MouseL.down() && edge)
		{
			const RoadNode* node = m_network.getNode(edge->nodeA);
			if (node)
				m_camera.setFocus(node->position);
		}

		y += kLH;
	}

	PanelWidget::flushTooltip();
	m_panelManager.reportContentHeight(U"vehicle_info", y);
}
