#include "GameScene.hpp"
#include "../ui/PanelWidget.hpp"
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

	/// @brief RoadPartType に対するデフォルト defId を返す
	StringView defaultDefIdForType(RoadPartType type)
	{
		switch (type)
		{
		case RoadPartType::Roadbed:
		case RoadPartType::Shoulder:  return U"roadbed_asphalt";
		case RoadPartType::Sidewalk:  return U"sidewalk_tile";
		case RoadPartType::Median:    return U"median_concrete";
		case RoadPartType::Curb:      return U"curb_concrete";
		case RoadPartType::Slope:     return U"slope_grass";
		case RoadPartType::Guardrail: return U"guardrail_steel";
		case RoadPartType::Wall:      return U"wall_concrete";
		default:                       return U"";
		}
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

	// ── 断面編集 UI の状態 ──
	struct SectionEditState
	{
		int   selectedPart = -1;
		int   selectedLane = -1;
		int   dragMode     = 0;   // 0=none, 1-3=part, 4-6=lane
		float dragAnchor   = 0.0f;
		bool  partsCollapsed = false;
		bool  lanesCollapsed = false;
	};

	/// @brief Parts + Lanes の断面編集UIを描画する（drawEdgePanel / drawDrawTemplatePanel 共用）
	/// @return パーツ/車線が変更されたか
	bool drawRoadSections(RoadEdge& edge, SectionEditState& st,
	                      const Font& pFont, const Font& pBold, int pX, int& y)
	{
		constexpr int kBarX = 6;
		constexpr int kBarW = 356;
		constexpr int kPartBarH = 36;
		constexpr int kLaneBarH = 28;
		constexpr int kEdgeGrab = 4;
		constexpr int kSectionW = 360;
		constexpr int kLH = 17;
		bool dirty = false;

		// スケーリング計算
		float extMin = 1e9f, extMax = -1e9f;
		for (const auto& p : edge.parts)
		{
			extMin = Min(extMin, p.offset);
			extMax = Max(extMax, p.offset + p.width);
		}
		for (const auto& L : edge.lanes)
		{
			extMin = Min(extMin, Min(L.offsetA_L, L.offsetB_L));
			extMax = Max(extMax, Max(L.offsetA_R, L.offsetB_R));
		}
		if (extMin >= extMax) { extMin = -5.0f; extMax = 5.0f; }
		const float margin = (extMax - extMin) * 0.08f + 0.5f;
		extMin -= margin;
		extMax += margin;
		const float extRange = extMax - extMin;

		auto mToPixel = [&](float m) -> double { return kBarX + (m - extMin) / extRange * kBarW; };
		auto pixelToM = [&](double px) -> float { return extMin + static_cast<float>((px - kBarX) / kBarW) * extRange; };

		static constexpr ColorF partColors[] = {
			ColorF{0.25, 0.25, 0.27}, ColorF{0.35, 0.33, 0.30}, ColorF{0.45, 0.55, 0.30},
			ColorF{0.60, 0.58, 0.55}, ColorF{0.20, 0.20, 0.22}, ColorF{0.55, 0.55, 0.55},
			ColorF{0.45, 0.42, 0.38}, ColorF{0.50, 0.48, 0.44}, ColorF{0.40, 0.52, 0.30},
			ColorF{0.30, 0.45, 0.55},
		};

		// 範囲チェック
		if (st.selectedPart >= static_cast<int>(edge.parts.size())) st.selectedPart = -1;
		if (st.selectedLane >= static_cast<int>(edge.lanes.size())) st.selectedLane = -1;

		// ── ドラッグ更新 ──
		if (st.dragMode != 0 && MouseL.pressed())
		{
			const float curM = pixelToM(Cursor::PosF().x);
			const float delta = curM - st.dragAnchor;

			if (st.dragMode >= 1 && st.dragMode <= 3 && st.selectedPart >= 0)
			{
				auto& p = edge.parts[st.selectedPart];
				if (st.dragMode == 1)      { p.offset += delta; st.dragAnchor = curM; dirty = true; }
				else if (st.dragMode == 2) { p.offset += delta; p.width -= delta; if (p.width < 0.5f) { p.offset -= (0.5f - p.width); p.width = 0.5f; } st.dragAnchor = curM; dirty = true; }
				else if (st.dragMode == 3) { p.width += delta; if (p.width < 0.5f) p.width = 0.5f; st.dragAnchor = curM; dirty = true; }
			}
			else if (st.dragMode >= 4 && st.dragMode <= 6 && st.selectedLane >= 0)
			{
				auto& L = edge.lanes[st.selectedLane];
				if (st.dragMode == 4) { L.offsetA_L += delta; L.offsetA_R += delta; L.offsetB_L += delta; L.offsetB_R += delta; st.dragAnchor = curM; dirty = true; }
				else if (st.dragMode == 5) { L.offsetA_L += delta; L.offsetB_L += delta; st.dragAnchor = curM; dirty = true; }
				else if (st.dragMode == 6) { L.offsetA_R += delta; L.offsetB_R += delta; st.dragAnchor = curM; dirty = true; }
			}
		}
		else if (st.dragMode != 0)
		{
			st.dragMode = 0;
		}

		// ========== Parts セクション ==========
		{
			static constexpr StringView ptNames[] = { U"Roadbed", U"Shoulder", U"Median", U"Sidewalk",
				U"Gutter", U"Guard", U"Wall", U"Curb", U"Slope", U"Bike" };
			static constexpr StringView bsNames[] = { U"NotBuilt", U"Building", U"Built", U"Stub" };

			if (PanelWidget::section(pBold, U"Parts ({})"_fmt(edge.parts.size()), st.partsCollapsed, pX, y, kSectionW, kLH))
			{
				if (PanelWidget::button(pFont, U"+", false, pX + 4, y, 16, kLH, U"Add part"))
				{
					RoadPart np;
					np.type = RoadPartType::Roadbed; np.width = 3.5f;
					np.offset = edge.totalWidth() * 0.5f; np.build = BuildState::Built;
					np.defId = String{ defaultDefIdForType(np.type) };
					edge.parts << np; dirty = true;
				}
				y += kLH;

				const int barY = y;
				RectF{ static_cast<double>(kBarX), static_cast<double>(barY),
				       static_cast<double>(kBarW), static_cast<double>(kPartBarH) }
					.draw(ColorF{ 0.08, 0.08, 0.10 });

				const double centerPx = mToPixel(0.0f);
				if (centerPx > kBarX && centerPx < kBarX + kBarW)
					RectF{ centerPx - 0.5, static_cast<double>(barY), 1.0, static_cast<double>(kPartBarH) }
						.draw(ColorF{ 1.0, 1.0, 1.0, 0.3 });

				for (int i = 0; i < static_cast<int>(edge.parts.size()); ++i)
				{
					const auto& p = edge.parts[i];
					const double px0 = mToPixel(p.offset);
					const double px1 = mToPixel(p.offset + p.width);
					const double pw = Max(px1 - px0, 2.0);
					const bool sel = (i == st.selectedPart);

					ColorF col = partColors[Clamp(static_cast<int>(p.type), 0, 9)];
					if (p.build != BuildState::Built) col = col * 0.5;

					RectF rect{ px0, static_cast<double>(barY + 2), pw, static_cast<double>(kPartBarH - 4) };
					rect.draw(sel ? col.lerp(ColorF{1.0}, 0.25) : col);
					rect.drawFrame(1.0, sel ? ColorF{1.0, 1.0, 0.3} : ColorF{0.3, 0.3, 0.3});

					if (pw > 20)
						pFont(ptNames[static_cast<int>(p.type)]).draw(8.0,
							Vec2{ px0 + 2, static_cast<double>(barY + 3) }, ColorF{1.0, 1.0, 1.0, 0.9});

					if (st.dragMode == 0 && rect.mouseOver())
					{
						const double mx = Cursor::PosF().x;
						if (MouseL.down())
						{
							st.selectedPart = i;
							st.selectedLane = -1;
							st.dragAnchor = pixelToM(mx);
							if (mx - px0 < kEdgeGrab && pw > 10)      st.dragMode = 2;
							else if (px1 - mx < kEdgeGrab && pw > 10) st.dragMode = 3;
							else                                       st.dragMode = 1;
						}
						else if (MouseR.down())
						{
							edge.parts[i].type = static_cast<RoadPartType>(
								(static_cast<int>(edge.parts[i].type) + 1) % 10);
							edge.parts[i].defId = String{ defaultDefIdForType(edge.parts[i].type) };
							dirty = true;
						}
					}
				}
				y += kPartBarH + 2;

				if (st.selectedPart >= 0 && st.selectedPart < static_cast<int>(edge.parts.size()))
				{
					auto& sp = edge.parts[st.selectedPart];
					int bx = pX;

					PanelWidget::label(pBold, U"[{}]"_fmt(st.selectedPart), bx, y, ColorF{1.0, 1.0, 0.5});
					bx += 24;
					if (PanelWidget::cycle(pFont, sp.type, ptNames, 10, bx, y, 56, kLH))
					{
						sp.defId = String{ defaultDefIdForType(sp.type) };
						dirty = true;
					}
					bx += 58;
					if (PanelWidget::cycle(pFont, sp.build, bsNames, 4, bx, y, 52, kLH)) dirty = true;
					bx += 58;
					if (PanelWidget::button(pFont, U"X", false, bx, y, 18, kLH, U"Remove"))
					{
						edge.parts.remove_at(st.selectedPart);
						st.selectedPart = -1;
						dirty = true;
					}
					y += kLH;

					if (st.selectedPart >= 0)
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
		{
			static constexpr StringView osN[] = { U"Open", U"Provisional", U"Closed", U"Reserved" };
			static constexpr StringView ltN[] = { U"Normal", U"Bus", U"Climb", U"Turn", U"Accel", U"Decel" };
			static constexpr StringView lnN[] = { U"None", U"Solid W", U"Dash W", U"Solid Y", U"Double Y" };
			static constexpr StringView drN[] = { U"Forward", U"Backward" };

			if (PanelWidget::section(pBold, U"Lanes ({})"_fmt(edge.lanes.size()), st.lanesCollapsed, pX, y, kSectionW, kLH))
			{
				if (PanelWidget::button(pFont, U"+", false, pX + 4, y, 16, kLH, U"Add lane"))
				{
					Lane nl; nl.dir = LaneDir::Forward; nl.op = OpState::Open; nl.nominalWidth = 3.5f;
					const float hw = edge.totalWidth() * 0.5f;
					nl.offsetA_L = hw; nl.offsetA_R = hw + 3.5f; nl.offsetB_L = hw; nl.offsetB_R = hw + 3.5f;
					edge.lanes << nl; dirty = true;
				}
				y += kLH;

				const int laneBarY = y;
				RectF{ static_cast<double>(kBarX), static_cast<double>(laneBarY),
				       static_cast<double>(kBarW), static_cast<double>(kLaneBarH) }
					.draw(ColorF{ 0.08, 0.08, 0.10 });

				const double centerPx = mToPixel(0.0f);
				if (centerPx > kBarX && centerPx < kBarX + kBarW)
					RectF{ centerPx - 0.5, static_cast<double>(laneBarY), 1.0, static_cast<double>(kLaneBarH) }
						.draw(ColorF{ 1.0, 1.0, 1.0, 0.3 });

				for (int i = 0; i < static_cast<int>(edge.lanes.size()); ++i)
				{
					const auto& L = edge.lanes[i];
					const double pxA0 = mToPixel(L.offsetA_L);
					const double pxA1 = mToPixel(L.offsetA_R);
					const double pxB0 = mToPixel(L.offsetB_L);
					const double pxB1 = mToPixel(L.offsetB_R);
					const double yTop = static_cast<double>(laneBarY + 2);
					const double yBot = static_cast<double>(laneBarY + kLaneBarH - 2);
					const bool sel = (i == st.selectedLane);

					ColorF col{0.25, 0.25, 0.27};
					if (L.op == OpState::Closed)          col = ColorF{0.18, 0.18, 0.18};
					else if (L.op == OpState::Reserved)   col = ColorF{0.22, 0.20, 0.25};
					else if (L.op == OpState::Provisional) col = ColorF{0.28, 0.27, 0.22};

					const ColorF fillCol = sel ? col.lerp(ColorF{1.0}, 0.15) : col;
					Quad laneQuad{ Vec2{pxA0, yTop}, Vec2{pxA1, yTop},
					               Vec2{pxB1, yBot}, Vec2{pxB0, yBot} };
					laneQuad.draw(fillCol);
					if (sel) laneQuad.drawFrame(1.0, ColorF{1.0, 1.0, 0.3});

					const double avgW = Max((pxA1 - pxA0 + pxB1 - pxB0) * 0.5, 2.0);
					if (avgW > 14)
					{
						const StringView arrow = (L.dir == LaneDir::Forward) ? U"\u2192" : U"\u2190";
						const ColorF arrowCol = (L.dir == LaneDir::Forward)
							? ColorF{1.0, 1.0, 1.0, 0.6} : ColorF{1.0, 0.5, 0.5, 0.6};
						pBold(arrow).drawAt(10.0,
							Vec2{ (pxA0 + pxA1 + pxB0 + pxB1) * 0.25, laneBarY + kLaneBarH * 0.5 }, arrowCol);
					}

					// 左側ライン
					{
						const ColorF lc = lineTypeColor(L.lineLeft);
						if (lc.a > 0.01)
						{
							const double lw = (L.lineLeft == LineType::DoubleYellow) ? 3.0 : 1.0;
							Line{ Vec2{pxA0, yTop}, Vec2{pxB0, yBot} }.draw(lw, lc);
						}
					}
					// 右側ライン
					{
						const ColorF lc = lineTypeColor(L.lineRight);
						if (lc.a > 0.01)
						{
							const double lw = (L.lineRight == LineType::DoubleYellow) ? 3.0 : 1.0;
							Line{ Vec2{pxA1, yTop}, Vec2{pxB1, yBot} }.draw(lw, lc);
						}
					}

					if (st.dragMode == 0 && laneQuad.mouseOver())
					{
						const double mx = Cursor::PosF().x;
						const double pxMid0 = (pxA0 + pxB0) * 0.5;
						const double pxMid1 = (pxA1 + pxB1) * 0.5;
						const double pw = Max(pxMid1 - pxMid0, 2.0);
						if (MouseL.down())
						{
							st.selectedLane = i;
							st.selectedPart = -1;
							st.dragAnchor = pixelToM(mx);
							if (mx - pxMid0 < kEdgeGrab && pw > 10)      st.dragMode = 5;
							else if (pxMid1 - mx < kEdgeGrab && pw > 10) st.dragMode = 6;
							else                                          st.dragMode = 4;
						}
						else if (MouseR.down())
						{
							edge.lanes[i].dir = (L.dir == LaneDir::Forward) ? LaneDir::Backward : LaneDir::Forward;
							dirty = true;
						}
					}
				}
				y += kLaneBarH + 2;

				if (st.selectedLane >= 0 && st.selectedLane < static_cast<int>(edge.lanes.size()))
				{
					auto& sl = edge.lanes[st.selectedLane];
					int bx = pX;

					PanelWidget::label(pBold, U"Lane {}"_fmt(st.selectedLane), bx, y, ColorF{0.8, 0.8, 1.0});
					bx += 46;
					dirty |= PanelWidget::cycle(pFont, sl.dir, drN, 2, bx, y, 66, kLH);
					bx += 68;
					dirty |= PanelWidget::cycle(pFont, sl.op, osN, 4, bx, y, 78, kLH);
					bx += 80;
					dirty |= PanelWidget::cycle(pFont, sl.type, ltN, 6, bx, y, 50, kLH);
					bx += 54;
					if (PanelWidget::button(pFont, U"X", false, bx, y, 18, kLH, U"Remove"))
					{
						edge.lanes.remove_at(st.selectedLane);
						st.selectedLane = -1;
						dirty = true;
					}
					y += kLH;

					if (st.selectedLane >= 0)
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

		if (dirty)
		{
			resolvePartOverlapAndGap(edge.parts);
			resolveLaneOverlap(edge.lanes);
		}
		return dirty;
	}
}

// =============================================================================
// 地名リストパネル
// =============================================================================

void GameScene::drawNameListPanel()
{
	auto area = m_panelManager.beginContent(U"name_list");
	if (!area) return;

	const auto& listFont = FontAsset(Asset::Panel14);
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

	// 断面編集（Parts + Lanes 共通関数）
	static SectionEditState edgeSectionState;
	dirty |= drawRoadSections(*edge, edgeSectionState, pFont, pBold, pX, y);

	PanelWidget::flushTooltip();
	m_panelManager.reportContentHeight(U"edge_info", y);

	if (dirty)
	{
		m_roadRenderer.invalidateEdgeCache(edge->id, edge->nodeA, edge->nodeB);
		m_roadRenderer.invalidateCachesAroundNode(edge->nodeA, m_network);
		m_roadRenderer.invalidateCachesAroundNode(edge->nodeB, m_network);
	}
}

// NOTE: 旧コード削除マーカー開始

// =============================================================================
// 道路設置テンプレートパネル
// =============================================================================

void GameScene::drawDrawTemplatePanel()
{
	if (m_mode != EditMode::RoadDraw) return;

	auto area = m_panelManager.beginContent(U"draw_template");
	if (!area) return;

	RoadEdge* edge = &m_drawTemplate;

	const auto& pFont = panelFont();
	const auto& pBold = panelBoldFont();

	constexpr int kPad = 6;
	constexpr int kLH = 17;
	const int pX = kPad;
	int y = 0;
	bool dirty = false;

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
		y += kLH + 4;
	}

	// 断面編集（Parts + Lanes 共通関数）
	static SectionEditState tplSectionState;
	dirty |= drawRoadSections(*edge, tplSectionState, pFont, pBold, pX, y);

	PanelWidget::flushTooltip();
	m_panelManager.reportContentHeight(U"draw_template", y);
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
	y += kLH;

	// Y 座標スピナー（変更時にコントロールポイントも連動）
	{
		PanelWidget::label(pFont, U"Y", pX, y);
		float tmpY = static_cast<float>(node->position.y);
		if (PanelWidget::spin(pFont, tmpY, 1.0f, -100.0f, 200.0f, pX + 16, y, 54, kLH))
		{
			const double dy = static_cast<double>(tmpY) - node->position.y;
			node->position.y = static_cast<double>(tmpY);
			for (const int eid : node->edgeIds())
			{
				if (auto* edge = m_network.getEdge(eid))
				{
					if (edge->nodeA == node->id)
						edge->ctrlA.y += dy;
					else
						edge->ctrlB.y += dy;
					m_network.updateEdgeElevation(eid, m_world);
				}
			}
			dirty = true;
		}
		y += kLH + 4;
	}

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
						notifyNetworkChanged({ *m_selectedNodeId });
					}
				}
				y += kLH;

				// コントロールポイント Y 編集
				if (auto* edge = m_network.getEdge(att.edgeId))
				{
					double& cpY = (edge->nodeA == node->id) ? edge->ctrlA.y : edge->ctrlB.y;
					float tmpCpY = static_cast<float>(cpY);
					PanelWidget::label(pFont, U"cpY", pX + 10, y);
					if (PanelWidget::spin(pFont, tmpCpY, 1.0f, -100.0f, 200.0f, pX + 34, y, 54, kLH))
					{
						cpY = static_cast<double>(tmpCpY);
						dirty = true;
					}
				}
				y += kLH + 2;
			}
		}
	}

	// Signal セクション（TrafficControl::Signal のエッジがあれば表示）
	{
		bool hasSignalEdge = false;
		for (const auto& att : node->attachments)
			if (att.control == TrafficControl::Signal) { hasSignalEdge = true; break; }

		if (hasSignalEdge)
		{
			y += 4;
			static bool sigCollapsed = false;
			constexpr int kSectionW = 300;

			if (PanelWidget::section(pBold, U"Signal", sigCollapsed, pX, y, kSectionW, kLH))
			{
				// 信号設置トグル
				const bool hasPlacement = node->signalPlacement.has_value();
				bool enabled = hasPlacement;
				if (PanelWidget::toggle(pFont, U"SIGNAL", U"signal", enabled, pX, y, 80, kLH))
				{
					if (enabled && !hasPlacement)
					{
						// デフォルトの信号を設置
						const auto defIds = m_roadRenderer.signalRegistry().defIds();
						if (!defIds.isEmpty())
						{
							SignalPlacement sp;
							sp.signalDefId = defIds[0];
							node->signalPlacement = sp;
							dirty = true;
						}
					}
					else if (!enabled && hasPlacement)
					{
						node->signalPlacement = none;
						dirty = true;
					}
				}
				y += kLH + 2;

				if (node->signalPlacement)
				{
					auto& sp = *node->signalPlacement;
					PanelWidget::label(pFont, U"Def: {}"_fmt(sp.signalDefId), pX, y, ColorF{ 0.8 });
					y += kLH;

					// sub_lamp 管理
					const SignalDef* sigDef = m_roadRenderer.signalRegistry().getDef(sp.signalDefId);
					if (sigDef && sigDef->subLamp)
					{
						PanelWidget::label(pFont, U"Arrows: {}"_fmt(sp.subLampStates.size()), pX, y, ColorF{ 0.8 });

						// 矢印追加ボタン
						if (PanelWidget::button(pFont, U"+", false, pX + 80, y, 20, kLH, U"Add arrow lamp"))
						{
							const auto& states = sigDef->subLamp->stateIds;
							if (!states.isEmpty())
							{
								// "off"以外の最初の状態をデフォルトに
								String defaultState = U"off";
								for (const auto& s : states)
									if (s != U"off") { defaultState = s; break; }
								sp.subLampStates << defaultState;
								dirty = true;
							}
						}
						// 矢印削除ボタン
						if (!sp.subLampStates.isEmpty())
						{
							if (PanelWidget::button(pFont, U"-", false, pX + 104, y, 20, kLH, U"Remove last arrow lamp"))
							{
								sp.subLampStates.pop_back();
								dirty = true;
							}
						}
						y += kLH + 2;

						// 各矢印の状態サイクル
						static constexpr StringView arrowNames[] = {
							U"arrow_left", U"arrow_straight", U"arrow_right", U"off"
						};
						for (int si = 0; si < static_cast<int>(sp.subLampStates.size()); ++si)
						{
							PanelWidget::label(pFont, U"[{}]"_fmt(si), pX, y, ColorF{ 0.7 });

							// 状態サイクルボタン
							const String& cur = sp.subLampStates[si];
							if (PanelWidget::button(pFont, cur, false, pX + 24, y, 100, kLH, U"Cycle arrow state"))
							{
								// 次の状態にサイクル
								const auto& states = sigDef->subLamp->stateIds;
								int idx = -1;
								for (int k = 0; k < static_cast<int>(states.size()); ++k)
									if (states[k] == cur) { idx = k; break; }
								sp.subLampStates[si] = states[(idx + 1) % static_cast<int>(states.size())];
								dirty = true;
							}
							y += kLH;
						}
					}
				}
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
