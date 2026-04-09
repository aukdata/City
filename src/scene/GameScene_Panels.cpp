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

	// Dissolve ボタン（接続エッジが2本のときのみ表示）
	if (node->attachments.size() == 2)
	{
		if (PanelWidget::button(pFont, U"Dissolve", false, pX, y, 80, kLH, U"Remove node and merge 2 edges into 1"))
		{
			const int nid = *m_selectedNodeId;
			// dissolve 前に隣接ノードを収集
			Array<int> neighbors;
			for (const auto& att : node->attachments)
			{
				if (const RoadEdge* e = m_network.getEdge(att.edgeId))
				{
					const int other = (e->nodeA == nid) ? e->nodeB : e->nodeA;
					neighbors << other;
				}
			}
			if (const auto newEdgeId = m_network.dissolveNode(nid))
			{
				m_selectedNodeId = none;
				notifyNetworkChanged(neighbors);
			}
		}
		y += kLH + 4;
	}

	// Signal セクション
	{
		y += 4;
		const bool hasPlacement = node->signalPlacement.has_value();
		bool enabled = hasPlacement;
		PanelWidget::label(pBold, U"Signal", pX, y, ColorF{ 1.0, 1.0, 0.4 });
		if (PanelWidget::toggle(pFont, U"ON", U"OFF", enabled, pX + 60, y, 50, kLH))
		{
			if (enabled && !hasPlacement)
			{
				const auto defIds = m_roadRenderer.signalRegistry().defIds();
				if (!defIds.isEmpty())
				{
					SignalPlacement sp;
					sp.signalDefId = defIds[0];
					node->signalPlacement = sp;
					// 全 Signal エッジの attachment.control を設定
					for (auto& att : node->attachments)
					{
						att.control = TrafficControl::Signal;
					}
					dirty = true;
				}
			}
			else if (!enabled && hasPlacement)
			{
				node->signalPlacement = none;
				for (auto& att : node->attachments)
				{
					if (att.control == TrafficControl::Signal)
					{
						att.control = TrafficControl::None;
					}
				}
				dirty = true;
			}
		}
		if (hasPlacement)
		{
			if (PanelWidget::button(pFont, U"Edit...", false, pX + 120, y, 60, kLH, U"Open signal cycle editor"))
			{
				const Vec2 panelSize = m_panelManager.getSize(U"signal_edit");
				const Vec2 ctr{
					(Scene::Width() - panelSize.x) * 0.5,
					(Scene::Height() - panelSize.y) * 0.5
				};
				m_panelManager.show(U"signal_edit",
					U"Signal - Node #{}"_fmt(node->id), ctr);
			}
		}
		y += kLH + 2;

		// 矢印信号の種別選択（方向ごと）
		if (hasPlacement)
		{
			auto& spRef = *node->signalPlacement;
			const SignalDef* sigDef = m_roadRenderer.signalRegistry().getDef(spRef.signalDefId);
			if (sigDef && sigDef->subLamp)
			{
				static constexpr StringView arrowTypes[] = { U"arrow_left", U"arrow_straight", U"arrow_right" };
				static constexpr StringView arrowLabels[] = { U"L", U"S", U"R" };

				for (const auto& att : node->attachments)
				{
					if (att.control != TrafficControl::Signal) continue;
					PanelWidget::label(pFont, U"E{}"_fmt(att.edgeId), pX, y, ColorF{ 0.7 });

					auto& states = spRef.subLampStates[att.edgeId];
					int bx = pX + 36;
					for (int ai = 0; ai < 3; ++ai)
					{
						const String id{ arrowTypes[ai] };
						bool has = states.contains(id);
						if (PanelWidget::toggle(pFont, arrowLabels[ai], arrowLabels[ai], has, bx, y, 24, kLH))
						{
							if (has && !states.contains(id))
							{
								states << id;
							}
							else if (!has)
							{
								states.remove(id);
							}
							if (states.isEmpty())
							{
								spRef.subLampStates.erase(att.edgeId);
							}
							dirty = true;
						}
						bx += 26;
					}
					y += kLH;
				}
				y += 2;
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

	// ── PanelLayout テスト: 追跡ボタンだけ新システムで描画 ──
	if (!m_vehicleLayoutReady)
	{
		m_vehicleLayout.button(U"track", U"Track", m_trackingVehicle, 120);
		m_vehicleLayoutReady = true;
	}

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

	// 追跡ボタン（PanelLayout で描画）
	{
		const int contentW = static_cast<int>(m_panelManager.getSize(U"vehicle_info").x) - pX * 2;
		m_vehicleLayout.update(contentW);
		m_vehicleLayout.draw();
		if (m_vehicleLayout.clicked(U"track"))
		{
			m_trackingVehicle = !m_trackingVehicle;
		}
		y += m_vehicleLayout.contentHeight() + 4;
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

// ===== 信号サイクル編集パネル =====

namespace
{
	/// @brief LineType に応じた描画色を返す
	ColorF lineColor(LineType lt)
	{
		switch (lt)
		{
		case LineType::SolidWhite:  return ColorF{ 1.0, 1.0, 1.0 };
		case LineType::DashedWhite: return ColorF{ 1.0, 1.0, 1.0 };
		case LineType::SolidYellow: return ColorF{ 1.0, 0.9, 0.0 };
		case LineType::DoubleYellow:return ColorF{ 1.0, 0.9, 0.0 };
		default: return ColorF{ 0.0, 0.0 };
		}
	}

	/// @brief RoadPartType に応じた描画色を返す
	ColorF partColor(RoadPartType type)
	{
		switch (type)
		{
		case RoadPartType::Roadbed:   return ColorF{ 0.25, 0.25, 0.28 };
		case RoadPartType::Sidewalk:  return ColorF{ 0.50, 0.48, 0.44 };
		case RoadPartType::Curb:      return ColorF{ 0.55, 0.53, 0.50 };
		case RoadPartType::Median:    return ColorF{ 0.35, 0.38, 0.30 };
		case RoadPartType::Guardrail: return ColorF{ 0.45, 0.45, 0.48 };
		case RoadPartType::Slope:     return ColorF{ 0.35, 0.45, 0.25 };
		case RoadPartType::Shoulder:  return ColorF{ 0.30, 0.30, 0.28 };
		default: return ColorF{ 0.3 };
		}
	}

	/// @brief 2D 3次ベジェ補間
	Vec2 bezier2D(const Vec2& p0, const Vec2& p1, const Vec2& p2, const Vec2& p3, double t)
	{
		const double m = 1.0 - t;
		return p0 * (m * m * m) + p1 * (3 * m * m * t) + p2 * (3 * m * t * t) + p3 * (t * t * t);
	}

	/// @brief 2D ベジェ帯（内側+外側カーブ）を Polygon 化する
	Array<Vec2> bezierBand(const Vec2& iP0, const Vec2& iP3, const Vec2& oP0, const Vec2& oP3,
	                       const Vec2& tan0, const Vec2& tan3, int div = 8)
	{
		const double dI = Max((iP3 - iP0).length() / 3.0, 1.0);
		const double dO = Max((oP3 - oP0).length() / 3.0, 1.0);

		Array<Vec2> pts;
		// 内側: 0→1
		for (int k = 0; k <= div; ++k)
		{
			const double t = k / static_cast<double>(div);
			pts << bezier2D(iP0, iP0 + tan0 * dI, iP3 + tan3 * dI, iP3, t);
		}
		// 外側: 1→0（逆順）
		for (int k = div; k >= 0; --k)
		{
			const double t = k / static_cast<double>(div);
			pts << bezier2D(oP0, oP0 + tan0 * dO, oP3 + tan3 * dO, oP3, t);
		}
		return pts;
	}

	/// @brief エッジの cutoff 位置の情報（2D 図描画用）
	struct EdgeCap2D
	{
		Vec2 center;     ///< カットオフ中心（2D）
		Vec2 fwd;        ///< ノード外向き正規化方向
		Vec2 right;      ///< 右方向
		double angle;    ///< 角度（ソート用）
		const RoadEdge* edge;
		int edgeId;
		bool isNodeA;
	};
}

void GameScene::drawSignalEditPanel()
{
	if (!m_selectedNodeId) { m_panelManager.hide(U"signal_edit"); return; }
	RoadNode* node = m_network.getNode(*m_selectedNodeId);
	if (!node || !node->signalPlacement) { m_panelManager.hide(U"signal_edit"); return; }

	auto area = m_panelManager.beginContent(U"signal_edit");
	if (!area) return;

	auto& sp = *node->signalPlacement;
	const auto& pFont = panelFont();
	const auto& pBold = panelBoldFont();
	constexpr int kPad = 6;
	constexpr int kLH = 17;
	constexpr int kLeftW = 180;  // 左ペイン幅
	const Vec2 panelSize = m_panelManager.getSize(U"signal_edit");
	bool dirty = false;

	// 信号エッジを収集（左ペインの表示順を固定）
	Array<int> signalEdgeIds;
	for (const auto& att : node->attachments)
	{
		if (att.control == TrafficControl::Signal)
		{
			signalEdgeIds << att.edgeId;
		}
	}

	// ========================================
	// 左ペイン: フェーズ一覧
	// ========================================
	int ly = kPad;
	constexpr int kRowH = 38; // フェーズ行の高さ（2段: ランプ + 時間）

	PanelWidget::label(pBold, U"Phases", kPad, ly, ColorF{ 1.0, 1.0, 0.4 });
	if (PanelWidget::button(pFont, U"+", false, kLeftW - 26, ly, 20, kLH, U"Add phase"))
	{
		SignalPhaseDef ph;
		ph.duration = 30.0f;
		sp.phases << std::move(ph);
		m_signalEditPhase = static_cast<int>(sp.phases.size()) - 1;
		dirty = true;
	}
	ly += kLH + 4;

	float totalDuration = 0.0f;
	for (int pi = 0; pi < static_cast<int>(sp.phases.size()); ++pi)
	{
		auto& ph = sp.phases[pi];
		totalDuration += ph.duration;

		const bool selected = (pi == m_signalEditPhase);
		const ColorF bg = selected ? ColorF{ 0.25, 0.35, 0.55 } : ColorF{ 0.16 };
		RectF{ static_cast<double>(kPad), static_cast<double>(ly),
		       static_cast<double>(kLeftW - kPad * 2), static_cast<double>(kRowH) }.rounded(3).draw(bg);

		// クリックでフェーズ選択
		{
			auto hit = PanelWidget::hitTest(pFont, kPad, ly, kLeftW - kPad * 2, kRowH);
			if (hit.clickL)
			{
				m_signalEditPhase = pi;
			}
		}

		// 1段目: フェーズ番号 + 小さな信号ランプ
		int lx = kPad + 4;
		PanelWidget::label(pFont, U"P{}"_fmt(pi + 1), lx, ly + 1, selected ? ColorF{ 1.0 } : ColorF{ 0.7 });
		lx += 22;

		for (const int eid : signalEdgeIds)
		{
			const bool g = ph.greenEdgeIds.contains(eid);
			const ColorF lampC = g ? ColorF{ 0.1, 0.9, 0.3 } : ColorF{ 0.9, 0.15, 0.1 };
			Circle{ Vec2{ lx + 5.0, ly + 8.0 }, 4.0 }.draw(lampC);
			lx += 14;
		}

		// 削除ボタン（右端）
		if (sp.phases.size() > 1)
		{
			if (PanelWidget::button(pFont, U"x", false, kLeftW - 24, ly + 1, 16, kLH - 2, U"Delete phase"))
			{
				sp.phases.remove_at(pi);
				if (m_signalEditPhase >= static_cast<int>(sp.phases.size()))
				{
					m_signalEditPhase = Max(0, static_cast<int>(sp.phases.size()) - 1);
				}
				dirty = true;
				break;
			}
		}

		// 2段目: 持続時間
		if (PanelWidget::spin(pFont, ph.duration, 5.0f, 5.0f, 120.0f,
		                      kPad + 4, ly + kLH + 1, 56, kLH - 2))
		{
			dirty = true;
		}
		PanelWidget::label(pFont, U"s", kPad + 62, ly + kLH + 1, ColorF{ 0.5 });

		ly += kRowH + 3;
	}

	// サイクル合計
	ly += 4;
	PanelWidget::label(pBold, U"Cycle: {:.0f}s"_fmt(totalDuration), kPad, ly, ColorF{ 0.9, 0.8, 0.4 });
	ly += kLH + 4;

	// ========================================
	// 右ペイン: 交差点図 + 信号表示
	// ========================================
	const double rightX = kLeftW;
	const double rightW = panelSize.x - kLeftW;
	const double diagramSize = Min(rightW - kPad, 380.0);
	const Vec2 center{ rightX + rightW * 0.5, kPad + diagramSize * 0.5 };
	const double armLen = diagramSize * 0.28;
	constexpr double kScale = 4.5;

	// 選択フェーズの青エッジ集合
	HashSet<int> greenSet;
	if (m_signalEditPhase >= 0 && m_signalEditPhase < static_cast<int>(sp.phases.size()))
	{
		for (const int eid : sp.phases[m_signalEditPhase].greenEdgeIds)
		{
			greenSet.insert(eid);
		}
	}

	auto outwardOffset = [](float off, float width, bool isNodeA) -> std::pair<float, float>
	{
		if (isNodeA) return { off, off + width };
		return { -(off + width), -off };
	};

	// 道路全幅の outward left/right を取得
	auto getRoadExtent = [&](const RoadEdge* edge, bool isNodeA) -> std::pair<float, float>
	{
		float minL = 1e9f, maxR = -1e9f;
		for (const auto& p : edge->parts)
		{
			if (p.build != BuildState::Built) continue;
			const auto [l, r] = outwardOffset(p.offset, p.width, isNodeA);
			minL = Min(minL, l);
			maxR = Max(maxR, r);
		}
		return { minL, maxR };
	};

	// 路盤の outward left/right を取得
	auto getRoadbedExtent = [&](const RoadEdge* edge, bool isNodeA) -> std::pair<float, float>
	{
		float minL = 1e9f, maxR = -1e9f;
		for (const auto& p : edge->parts)
		{
			if (p.type != RoadPartType::Roadbed) continue;
			const auto [l, r] = outwardOffset(p.offset, p.width, isNodeA);
			minL = Min(minL, l);
			maxR = Max(maxR, r);
		}
		return { minL, maxR };
	};

	// ---- カットオフ情報を収集（角度順ソート）----
	Array<EdgeCap2D> caps;
	for (const auto& att : node->attachments)
	{
		const RoadEdge* edge = m_network.getEdge(att.edgeId);
		if (!edge) continue;
		const auto bez = m_network.getBezier(att.edgeId);
		if (!bez) continue;

		const bool isNodeA = (edge->nodeA == node->id);
		const float cutoff = isNodeA ? edge->cutoffA : edge->cutoffB;

		Vec3 capTan;
		if (isNodeA)
		{
			const float s = Clamp(cutoff - 0.1f, 0.0f, bez->totalLength * 0.45f);
			capTan = bez->tangentAt(s);
		}
		else
		{
			const float s = Clamp(bez->totalLength - cutoff + 0.1f, bez->totalLength * 0.55f, bez->totalLength);
			capTan = -bez->tangentAt(s);
		}

		const Vec2 fwd{ capTan.x, capTan.z };
		const double fwdLen = fwd.length();
		if (fwdLen < 1e-6) continue;
		const Vec2 dn = fwd / fwdLen;

		EdgeCap2D cap;
		cap.center  = center + dn * (cutoff * kScale);
		cap.fwd     = dn;
		cap.right   = Vec2{ -dn.y, dn.x };
		cap.angle   = Math::Atan2(dn.y, dn.x);
		cap.edge    = edge;
		cap.edgeId  = att.edgeId;
		cap.isNodeA = isNodeA;
		caps << cap;
	}
	caps.sort_by([](const EdgeCap2D& a, const EdgeCap2D& b) { return a.angle < b.angle; });

	// ---- 交差点内エリアを全周ポリゴンで塗りつぶし ----
	if (caps.size() >= 2)
	{
		const int N = static_cast<int>(caps.size());
		const ColorF junctionColor{ 0.25, 0.25, 0.28 };
		constexpr int kBezDiv = 12;

		// 全周ポリゴンの頂点を構築
		Array<Vec2> boundary;
		for (int i = 0; i < N; ++i)
		{
			const auto& capCur = caps[i];
			const auto& capNext = caps[(i + 1) % N];
			const auto [rbL, rbR] = getRoadbedExtent(capCur.edge, capCur.isNodeA);
			const auto [nbL, nbR] = getRoadbedExtent(capNext.edge, capNext.isNodeA);

			// このエッジの左端→右端
			boundary << (capCur.center + capCur.right * (rbL * kScale));
			boundary << (capCur.center + capCur.right * (rbR * kScale));

			// 次のエッジの左端へベジェ曲線
			const Vec2 pA = capCur.center + capCur.right * (rbR * kScale);
			const Vec2 pB = capNext.center + capNext.right * (nbL * kScale);
			const Vec2 tanA{ -capCur.fwd.x, -capCur.fwd.y };
			const Vec2 tanB{ -capNext.fwd.x, -capNext.fwd.y };
			const double d = Max((pB - pA).length() / 3.0, 2.0);

			for (int k = 1; k < kBezDiv; ++k)
			{
				const double t = k / static_cast<double>(kBezDiv);
				boundary << bezier2D(pA, pA + tanA * d, pB + tanB * d, pB, t);
			}
		}

		// 三角形ファンで塗りつぶし
		for (int k = 0; k < static_cast<int>(boundary.size()); ++k)
		{
			const int next = (k + 1) % static_cast<int>(boundary.size());
			Triangle{ center, boundary[k], boundary[next] }.draw(junctionColor);
		}
	}

	// ---- 各エッジアーム ----
	for (const auto& cap : caps)
	{
		const auto* edge = cap.edge;
		const Vec2& dn = cap.fwd;
		const Vec2& rt = cap.right;

		// パーツ描画
		for (const auto& part : edge->parts)
		{
			if (part.build != BuildState::Built) continue;

			const auto [oL, oR] = outwardOffset(part.offset, part.width, cap.isNodeA);
			const double pLeft  = static_cast<double>(oL) * kScale;
			const double pRight = static_cast<double>(oR) * kScale;

			const Vec2 nearL = cap.center + rt * pLeft;
			const Vec2 nearR = cap.center + rt * pRight;
			const Vec2 farL  = nearL + dn * armLen;
			const Vec2 farR  = nearR + dn * armLen;

			Quad{ nearL, nearR, farR, farL }.draw(partColor(part.type));
		}

		// 道路外周の白線
		const auto [totalL, totalR] = getRoadExtent(edge, cap.isNodeA);
		{
			const Vec2 outerL0 = cap.center + rt * (totalL * kScale);
			const Vec2 outerL1 = outerL0 + dn * armLen;
			const Vec2 outerR0 = cap.center + rt * (totalR * kScale);
			const Vec2 outerR1 = outerR0 + dn * armLen;
			Line{ outerL0, outerL1 }.draw(1.5, ColorF{ 1.0 });
			Line{ outerR0, outerR1 }.draw(1.5, ColorF{ 1.0 });
		}

		// 車線区画線描画
		for (const auto& lane : edge->lanes)
		{
			if (lane.op != OpState::Open && lane.op != OpState::Provisional) continue;

			auto drawLaneLine = [&](LineType lt, float rawOffset)
			{
				if (lt == LineType::None) return;
				const double off = static_cast<double>(cap.isNodeA ? rawOffset : -rawOffset) * kScale;
				const Vec2 p0 = cap.center + rt * off;
				const Vec2 p1 = p0 + dn * armLen;
				const bool dashed = (lt == LineType::DashedWhite);
				if (dashed)
				{
					for (double dd = 0; dd < armLen; dd += 10.0)
					{
						const double d1 = Min(dd + 4.0, armLen);
						Line{ p0 + dn * dd, p0 + dn * d1 }.draw(1.0, lineColor(lt));
					}
				}
				else
				{
					Line{ p0, p1 }.draw(1.0, lineColor(lt));
				}
			};
			const float oL = cap.isNodeA ? lane.offsetA_L : lane.offsetB_L;
			const float oR = cap.isNodeA ? lane.offsetA_R : lane.offsetB_R;
			if (cap.isNodeA)
			{
				drawLaneLine(lane.lineLeft, oL);
				drawLaneLine(lane.lineRight, oR);
			}
			else
			{
				drawLaneLine(lane.lineRight, oL);
				drawLaneLine(lane.lineLeft, oR);
			}
		}

		// --- 信号機描画（横向き・クリック可能）---
		const auto* att = node->getAttachment(cap.edgeId);
		const bool hasSigControl = att && att->control == TrafficControl::Signal;
		const bool isGreen = greenSet.contains(cap.edgeId);

		if (hasSigControl)
		{
			float roadRight = 0.0f;
			for (const auto& part : edge->parts)
			{
				if (part.type == RoadPartType::Roadbed)
				{
					const auto [l, r] = outwardOffset(part.offset, part.width, cap.isNodeA);
					roadRight = Max(roadRight, r);
				}
			}
			const Vec2 sigPos = cap.center + dn * 2.0 + rt * (roadRight * kScale + 30.0);

			constexpr double kSigW = 100.0;
			constexpr double kSigH = 32.0;
			RectF{ Arg::center = sigPos, kSigW, kSigH }.rounded(4).draw(ColorF{ 0.3, 0.3, 0.32 });
			RectF{ Arg::center = sigPos, kSigW - 2, kSigH - 2 }.rounded(3).draw(ColorF{ 0.2, 0.2, 0.22 });

			constexpr double kLampR = 10.0;
			constexpr double kLampSpacing = 28.0;
			const Vec2 redPos    = sigPos + Vec2{ -kLampSpacing, 0 };
			const Vec2 yellowPos = sigPos;
			const Vec2 greenPos  = sigPos + Vec2{ kLampSpacing, 0 };
			const ColorF offColor{ 0.12 };

			Circle{ redPos,    kLampR }.draw(isGreen ? offColor : ColorF{ 1.0, 0.15, 0.1 });
			Circle{ redPos,    kLampR }.drawFrame(1.0, ColorF{ 0.35 });
			Circle{ yellowPos, kLampR }.draw(offColor);
			Circle{ yellowPos, kLampR }.drawFrame(1.0, ColorF{ 0.35 });
			Circle{ greenPos,  kLampR }.draw(isGreen ? ColorF{ 0.1, 1.0, 0.3 } : offColor);
			Circle{ greenPos,  kLampR }.drawFrame(1.0, ColorF{ 0.35 });

			// クリックで青/赤切替
			if (m_signalEditPhase >= 0 && m_signalEditPhase < static_cast<int>(sp.phases.size()))
			{
				const int hx = static_cast<int>(sigPos.x - kSigW * 0.5);
				const int hy = static_cast<int>(sigPos.y - kSigH * 0.5);
				auto hit = PanelWidget::hitTest(pFont, hx, hy,
				                                static_cast<int>(kSigW), static_cast<int>(kSigH),
				                                U"Click to toggle");
				if (hit.clickL)
				{
					auto& ph = sp.phases[m_signalEditPhase];
					if (isGreen) ph.greenEdgeIds.remove(cap.edgeId);
					else         ph.greenEdgeIds << cap.edgeId;
					dirty = true;
				}
			}

			// 矢印ランプ
			const auto subIt = sp.subLampStates.find(cap.edgeId);
			if (subIt != sp.subLampStates.end() && !subIt->second.isEmpty())
			{
				const Vec2 arrowBase = sigPos + Vec2{ 0, kSigH * 0.5 + 18.0 };
				constexpr double kArrSp = 32.0;
				const int cnt = static_cast<int>(subIt->second.size());
				const double tw = (cnt - 1) * kArrSp;
				RectF{ Arg::center = arrowBase, tw + 36.0, 32.0 }.rounded(3).draw(ColorF{ 0.3, 0.3, 0.32 });

				for (int ai = 0; ai < cnt; ++ai)
				{
					const Vec2 ap = arrowBase + Vec2{ -tw * 0.5 + ai * kArrSp, 0 };
					const String& stId = subIt->second[ai];
					const bool lit = isGreen && stId != U"off";
					const ColorF ac = lit ? ColorF{ 0.1, 1.0, 0.3 } : offColor;
					if (lit && stId == U"arrow_left")
						Triangle{ ap + Vec2{-8, 0}, ap + Vec2{4, -7}, ap + Vec2{4, 7} }.draw(ac);
					else if (lit && stId == U"arrow_right")
						Triangle{ ap + Vec2{8, 0}, ap + Vec2{-4, -7}, ap + Vec2{-4, 7} }.draw(ac);
					else if (lit && stId == U"arrow_straight")
						Triangle{ ap + Vec2{0, -8}, ap + Vec2{-7, 4}, ap + Vec2{7, 4} }.draw(ac);
					else
						Circle{ ap, 8.0 }.draw(ac);
					Circle{ ap, 9.0 }.drawFrame(1.0, ColorF{ 0.35 });
				}
			}
		}

		// 進行方向矢印
		if (hasSigControl && isGreen)
		{
			const Vec2 inward{ -dn.x, -dn.y };
			const Vec2 tip  = cap.center + inward * 6.0;
			const Vec2 base = cap.center + dn * (armLen * 0.5);
			const Vec2 perp = rt * 3.0;
			Line{ base, tip }.draw(2.5, ColorF{ 0.2, 0.85, 0.4, 0.7 });
			Triangle{ tip, tip - inward * 10.0 + perp * 2.5,
			          tip - inward * 10.0 - perp * 2.5 }.draw(ColorF{ 0.2, 0.85, 0.4, 0.7 });
		}

		// エッジIDラベル
		pFont(U"E{}"_fmt(cap.edgeId)).drawAt(cap.center + dn * (armLen + 12.0), ColorF{ 0.8 });
	}

	// ========================================
	// 高さ計算
	// ========================================
	const int totalHeight = Max(ly, static_cast<int>(diagramSize) + kPad * 2);

	if (dirty)
	{
		m_vehicleManager.markLightsDirty();
	}

	PanelWidget::flushTooltip();
	m_panelManager.reportContentHeight(U"signal_edit", totalHeight);
}
