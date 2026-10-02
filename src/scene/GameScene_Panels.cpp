#include "GameScene.hpp"
#include "../ui/TransportSectionControls.hpp"
#include "../ui/RoadInspectorEdit.hpp"
#include "../ui/RoadDiagramStyle.hpp"
#include "../ui/LandParcelPanel.hpp"
#include "../ui/ConstructionStatus.hpp"
#include "EdgeSectionState.hpp"
#include "../ui/PanelWidget.hpp"
#include "../ui/PanelLayout.hpp"
#include "../asset/AssetRegistrar.hpp"
#include "../road/GuideSign.hpp"
#include "../road/RoadSign.hpp"
#include "../road/RoadConstructionStart.hpp"

/// @file
/// @brief 道路の断面・計画・路線の編集。車両等の情報は InspectorPanels、信号現示は SignalPanel に置く。
namespace
{

	String formatConstructionCost(double costOku)
	{
		if (costOku < 1.0)
		{
			return U"{:.2f}億円"_fmt(costOku);
		}
		if (costOku < 10.0)
		{
			return U"{:.1f}億円"_fmt(costOku);
		}
		if (costOku < 100.0)
		{
			return U"{:.1f}億円"_fmt(costOku);
		}
		return U"{:.0f}億円"_fmt(costOku);
	}

	String formatConstructionDuration(double constructionDuration)
	{
		return ConstructionStatus::durationLabel(constructionDuration);
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

	// ── 断面編集 UI の状態 ──
	struct SectionEditState
	{
		int   selectedPart = -1;
		int   selectedLane = -1;
		// dragMode:
		//   0=none
		//   1=part移動, 2=part左端, 3=part右端
		//   4=lane移動, 5=lane左端(両側), 6=lane右端(両側)
		//   7=offsetA_L, 8=offsetB_L, 9=offsetA_R, 10=offsetB_R
		int    dragMode      = 0;
		float  dragAnchor    = 0.0f;
		bool   partsCollapsed = false;
		bool   lanesCollapsed = false;
		// 簡易ダブルクリック検出
		double lastClickTime = -1.0;
		Vec2   lastClickPos  { -1, -1 };
	};

	/// @brief マウス L の押下が直前クリックから 350ms 以内ならダブルクリックと判定
	bool detectDoubleClick(SectionEditState& st)
	{
		if (!MouseL.down()) return false;
		const double now = Scene::Time();
		const Vec2 pos = Cursor::PosF();
		const bool dbl = (st.lastClickTime > 0.0) && (now - st.lastClickTime < 0.35)
			&& (pos.distanceFrom(st.lastClickPos) < 6.0);
		st.lastClickTime = dbl ? -1.0 : now;
		st.lastClickPos  = pos;
		return dbl;
	}

	/// @brief Parts + Lanes の断面編集UIを描画する（drawEdgePanel / drawDrawTemplatePanel 共用）
	/// @return パーツ/車線が変更されたか
	bool drawRoadSections(RoadEdge& edge, SectionEditState& st,
	                      const Font& pFont, const Font& pBold, int pX, int& y)
	{
		// 道路断面の部品列と車線列を同じスケール上で編集し、ドラッグ変更をその場で edge に反映する。
		constexpr int kBarX = 6;
		constexpr int kBarW = 356;
		constexpr int kPartBarH = 36;
		constexpr int kLaneBarH = 28;
		constexpr int kEdgeGrab = 4;
		constexpr int kSectionW = 360;
		constexpr int kLH = 17;
		bool dirty = false;
		const bool doubleClickedThisFrame = detectDoubleClick(st);

		// スケーリング計算
		float extMin = 1e9f, extMax = -1e9f;
		for (const auto& p : edge.parts)
		{
			extMin = Min(extMin, Min(p.offsetA_L, p.offsetB_L));
			extMax = Max(extMax, Max(p.offsetA_R, p.offsetB_R));
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
				if (st.dragMode == 1)
				{
					// 4 隅一律平行移動
					p.offsetA_L += delta; p.offsetA_R += delta;
					p.offsetB_L += delta; p.offsetB_R += delta;
					st.dragAnchor = curM; dirty = true;
				}
				else if (st.dragMode == 2)
				{
					// 左端ドラッグ（右端固定）: A/B 両端の左端を移動
					const float minW = 0.5f;
					const float dA = Min(delta, p.widthA() - minW);
					const float dB = Min(delta, p.widthB() - minW);
					p.offsetA_L += dA; p.offsetB_L += dB;
					st.dragAnchor = curM; dirty = true;
				}
				else if (st.dragMode == 3)
				{
					// 右端ドラッグ（左端固定）: A/B 両端の右端を移動
					const float minW = 0.5f;
					const float dA = Max(delta, minW - p.widthA());
					const float dB = Max(delta, minW - p.widthB());
					p.offsetA_R += dA; p.offsetB_R += dB;
					st.dragAnchor = curM; dirty = true;
				}
			}
			else if (st.dragMode >= 11 && st.dragMode <= 14 && st.selectedPart >= 0)
			{
				auto& p = edge.parts[st.selectedPart];
				if      (st.dragMode == 11) { p.offsetA_L += delta; st.dragAnchor = curM; dirty = true; }
				else if (st.dragMode == 12) { p.offsetA_R += delta; st.dragAnchor = curM; dirty = true; }
				else if (st.dragMode == 13) { p.offsetB_L += delta; st.dragAnchor = curM; dirty = true; }
				else if (st.dragMode == 14) { p.offsetB_R += delta; st.dragAnchor = curM; dirty = true; }
			}
			else if (st.dragMode >= 4 && st.dragMode <= 10 && st.selectedLane >= 0)
			{
				auto& L = edge.lanes[st.selectedLane];
				if (st.dragMode == 4) { L.offsetA_L += delta; L.offsetA_R += delta; L.offsetB_L += delta; L.offsetB_R += delta; st.dragAnchor = curM; dirty = true; }
				else if (st.dragMode == 5) { L.offsetA_L += delta; L.offsetB_L += delta; st.dragAnchor = curM; dirty = true; }
				else if (st.dragMode == 6) { L.offsetA_R += delta; L.offsetB_R += delta; st.dragAnchor = curM; dirty = true; }
				else if (st.dragMode == 7)  { L.offsetA_L += delta; st.dragAnchor = curM; dirty = true; }
				else if (st.dragMode == 8)  { L.offsetB_L += delta; st.dragAnchor = curM; dirty = true; }
				else if (st.dragMode == 9)  { L.offsetA_R += delta; st.dragAnchor = curM; dirty = true; }
				else if (st.dragMode == 10) { L.offsetB_R += delta; st.dragAnchor = curM; dirty = true; }
			}
		}
		else if (st.dragMode != 0)
		{
			st.dragMode = 0;
		}

		// ========== Parts セクション ==========
		{
			static constexpr StringView ptNames[] = { U"路盤", U"路肩", U"中央帯", U"歩道",
				U"側溝", U"ガードレール", U"壁", U"縁石", U"法面", U"自転車道" };
			static constexpr StringView bsNames[] = { U"未建設", U"建設中", U"建設済", U"スタブ" };

			if (PanelWidget::section(pBold, U"断面部品 ({})"_fmt(edge.parts.size()), st.partsCollapsed, pX, y, kSectionW, kLH))
			{
				if (PanelWidget::button(pFont, U"+", false, pX + 4, y, 16, kLH, U"部品を追加"))
				{
					RoadPart np;
					np.type = RoadPartType::Roadbed;
					const float initOffset = edge.totalWidth() * 0.5f;
					np.offsetA_L = initOffset;
					np.offsetA_R = initOffset + 3.5f;
					np.offsetB_L = initOffset;
					np.offsetB_R = initOffset + 3.5f;
					np.build = BuildState::Built;
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
					// A 端（上辺）と B 端（下辺）のピクセル座標
					const double pxA0 = mToPixel(p.offsetA_L);
					const double pxA1 = mToPixel(p.offsetA_R);
					const double pxB0 = mToPixel(p.offsetB_L);
					const double pxB1 = mToPixel(p.offsetB_R);
					const double yTop = static_cast<double>(barY + 2);
					const double yBot = static_cast<double>(barY + kPartBarH - 2);
					// ヒット判定・ドラッグ用の代表 RectF（境界ボックス）
					const double hitX0 = Min(pxA0, pxB0);
					const double hitX1 = Max(pxA1, pxB1);
					const double hitW  = Max(hitX1 - hitX0, 2.0);
					const RectF hitRect{ hitX0, yTop, hitW, yBot - yTop };
					const bool sel = (i == st.selectedPart);

					ColorF col = RoadDiagramStyle::partTypeColor(p.type);
					if (p.build != BuildState::Built) col = col * 0.5;
					const ColorF drawCol = sel ? col.lerp(ColorF{1.0}, 0.25) : col;
					const ColorF frameCol = sel ? ColorF{1.0, 1.0, 0.3} : ColorF{0.3, 0.3, 0.3};

					// A/B 端が異なるテーパー形状を Quad で描画
					Quad{ Vec2{pxA0, yTop}, Vec2{pxA1, yTop}, Vec2{pxB1, yBot}, Vec2{pxB0, yBot} }
						.draw(drawCol);
					Quad{ Vec2{pxA0, yTop}, Vec2{pxA1, yTop}, Vec2{pxB1, yBot}, Vec2{pxB0, yBot} }
						.drawFrame(1.0, frameCol);

					if (hitW > 20)
						pFont(ptNames[static_cast<int>(p.type)]).draw(8.0,
							Vec2{ hitX0 + 2, yTop + 1 }, ColorF{1.0, 1.0, 1.0, 0.9});

					if (st.dragMode == 0 && hitRect.mouseOver())
					{
						const double mx = Cursor::PosF().x;
						const bool onLeftEdge  = (mx - hitX0 < kEdgeGrab && hitW > 10);
						const bool onRightEdge = (hitX1 - mx < kEdgeGrab && hitW > 10);
						Cursor::RequestStyle(onLeftEdge || onRightEdge
							? CursorStyle::ResizeLeftRight : CursorStyle::Hand);
						if (MouseL.down())
						{
							st.selectedPart = i;
							st.selectedLane = -1;
							st.dragAnchor = pixelToM(mx);
							if (onLeftEdge)       st.dragMode = 2;
							else if (onRightEdge) st.dragMode = 3;
							else                  st.dragMode = 1;
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

				// 部品間ギャップのダブルクリック詰め
				// default: 外側→内側 / Shift: 内側→外側 / Ctrl: 両側を広げる
				if (st.dragMode == 0 && edge.parts.size() >= 2 && doubleClickedThisFrame)
				{
					Array<RoadPart*> sorted;
					for (auto& p : edge.parts) sorted << &p;
					// 代表左端でソート
					sorted.sort_by([](const RoadPart* a, const RoadPart* b) { return a->offsetL() < b->offsetL(); });
					for (size_t i = 1; i < sorted.size(); ++i)
					{
						const float leftEnd  = sorted[i - 1]->offsetR();
						const float rightBeg = sorted[i]->offsetL();
						if (rightBeg - leftEnd <= 0.01f) continue;
						const double pxL = mToPixel(leftEnd);
						const double pxR = mToPixel(rightBeg);
						const RectF gap{ pxL, static_cast<double>(barY),
							Max(pxR - pxL, 2.0), static_cast<double>(kPartBarH) };
						if (!gap.mouseOver()) continue;
						const float gapM = rightBeg - leftEnd;
						if (KeyControl.pressed())
						{
							// 両側を広げる（A/B 同値で拡張）
							sorted[i - 1]->offsetA_R += gapM * 0.5f;
							sorted[i - 1]->offsetB_R += gapM * 0.5f;
							sorted[i]->offsetA_L     -= gapM * 0.5f;
							sorted[i]->offsetB_L     -= gapM * 0.5f;
							sorted[i]->offsetA_R     += gapM * 0.5f;
							sorted[i]->offsetB_R     += gapM * 0.5f;
						}
						else if (KeyShift.pressed())
						{
							sorted[i - 1]->offsetA_L += gapM; sorted[i - 1]->offsetA_R += gapM;
							sorted[i - 1]->offsetB_L += gapM; sorted[i - 1]->offsetB_R += gapM;
							sorted[i]->offsetA_L     -= gapM; sorted[i]->offsetA_R     -= gapM;
							sorted[i]->offsetB_L     -= gapM; sorted[i]->offsetB_R     -= gapM;
						}
						else
						{
							// 右側部品を左端に詰める
							const float shiftA = leftEnd - sorted[i]->offsetA_L;
							const float shiftB = leftEnd - sorted[i]->offsetB_L;
							sorted[i]->offsetA_L += shiftA; sorted[i]->offsetA_R += shiftA;
							sorted[i]->offsetB_L += shiftB; sorted[i]->offsetB_R += shiftB;
						}
						dirty = true;
						break;
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
					if (PanelWidget::buttonDanger(pFont, U"X", bx, y, 18, kLH, U"削除"))
					{
						edge.parts.remove_at(st.selectedPart);
						st.selectedPart = -1;
						dirty = true;
					}
					if (st.selectedPart >= 0) { dirty |= TransportSectionControls::roadbed(pFont,edge.parts[st.selectedPart],bx+24,y,kLH); }
					y += kLH;

					if (st.selectedPart >= 0)
					{
						// A 端
						bx = pX + 4;
						PanelWidget::label(pFont, U"A端L", bx, y, ColorF{1.0, 0.7, 0.4});
						if (PanelWidget::numberInput(pFont, sp.offsetA_L, 0.25f, -50.0f, 50.0f, bx + 34, y, 44, kLH, U"{:.2f}")) dirty = true;
						bx += 84;
						PanelWidget::label(pFont, U"A端R", bx, y, ColorF{1.0, 0.7, 0.4});
						if (PanelWidget::numberInput(pFont, sp.offsetA_R, 0.25f, -50.0f, 50.0f, bx + 34, y, 44, kLH, U"{:.2f}")) dirty = true;
						y += kLH;
						// B 端
						bx = pX + 4;
						PanelWidget::label(pFont, U"B端L", bx, y, ColorF{0.4, 0.7, 1.0});
						if (PanelWidget::numberInput(pFont, sp.offsetB_L, 0.25f, -50.0f, 50.0f, bx + 34, y, 44, kLH, U"{:.2f}")) dirty = true;
						bx += 84;
						PanelWidget::label(pFont, U"B端R", bx, y, ColorF{0.4, 0.7, 1.0});
						if (PanelWidget::numberInput(pFont, sp.offsetB_R, 0.25f, -50.0f, 50.0f, bx + 34, y, 44, kLH, U"{:.2f}")) dirty = true;
						y += kLH;
					}
				}
			}
			y += 4;
		}

		// ========== Lanes セクション ==========
		{
			static constexpr StringView osN[] = { U"開放", U"暫定供用", U"閉鎖", U"予約" };
			static constexpr StringView lnN[] = { U"なし", U"白実線", U"白破線", U"黄実線", U"黄二重線" };
			static constexpr StringView drN[] = { U"順方向", U"逆方向" };

			if (PanelWidget::section(pBold, U"車線 ({})"_fmt(edge.lanes.size()), st.lanesCollapsed, pX, y, kSectionW, kLH))
			{
				auto addLane = [&edge](int side)
				{
					constexpr float laneW = 3.5f;
					float edgeX = 0.0f;
					bool hasRoadbed = false;
					for (const auto& p : edge.parts)
					{
						if (p.type != RoadPartType::Roadbed) continue;
						// 代表値（平均）を使用
						const float x = (side > 0) ? p.offsetR() : p.offsetL();
						if (!hasRoadbed || (side > 0 ? x > edgeX : x < edgeX))
						{
							edgeX = x;
							hasRoadbed = true;
						}
					}
					if (!hasRoadbed)
					{
						edgeX = (side > 0) ? (edge.totalWidth() * 0.5f) : (-edge.totalWidth() * 0.5f);
					}

					// 外側のパーツをシフト & 末端 Roadbed を拡張（A/B 同値で操作）
					for (auto& p : edge.parts)
					{
						const bool outside = (side > 0)
							? (p.offsetL() >= edgeX - 1e-4f)
							: (p.offsetR() <= edgeX + 1e-4f);
						const bool extendTarget = hasRoadbed && p.type == RoadPartType::Roadbed
							&& ((side > 0 && Abs(p.offsetR() - edgeX) < 1e-4f)
								|| (side < 0 && Abs(p.offsetL() - edgeX) < 1e-4f));
						if (extendTarget)
						{
							if (side > 0)
							{
								p.offsetA_R += laneW;
								p.offsetB_R += laneW;
							}
							else
							{
								p.offsetA_L -= laneW;
								p.offsetB_L -= laneW;
							}
						}
						else if (outside)
						{
							p.offsetA_L += static_cast<float>(side) * laneW;
							p.offsetA_R += static_cast<float>(side) * laneW;
							p.offsetB_L += static_cast<float>(side) * laneW;
							p.offsetB_R += static_cast<float>(side) * laneW;
						}
					}

					Lane nl; nl.dir = LaneDir::Forward; nl.op = OpState::Open; nl.nominalWidth = laneW;
					const float laneL = (side > 0) ? edgeX : (edgeX - laneW);
					const float laneR = laneL + laneW;
					nl.offsetA_L = laneL; nl.offsetA_R = laneR;
					nl.offsetB_L = laneL; nl.offsetB_R = laneR;
					edge.lanes << nl;
				};

				if (PanelWidget::button(pFont, U"+L", false, pX + 4, y, 24, kLH, U"左に車線を追加"))
				{
					addLane(-1); dirty = true;
				}
				if (PanelWidget::button(pFont, U"+R", false, pX + 32, y, 24, kLH, U"右に車線を追加"))
				{
					addLane(+1); dirty = true;
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
						const ColorF lc = RoadDiagramStyle::lineTypeColor(L.lineLeft);
						if (lc.a > 0.01)
						{
							const double lw = (L.lineLeft == LineType::DoubleYellow) ? 3.0 : 1.0;
							Line{ Vec2{pxA0, yTop}, Vec2{pxB0, yBot} }.draw(lw, lc);
						}
					}
					// 右側ライン
					{
						const ColorF lc = RoadDiagramStyle::lineTypeColor(L.lineRight);
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
						const bool onLeftEdge  = (mx - pxMid0 < kEdgeGrab && pw > 10);
						const bool onRightEdge = (pxMid1 - mx < kEdgeGrab && pw > 10);
						Cursor::RequestStyle(onLeftEdge || onRightEdge
							? CursorStyle::ResizeLeftRight : CursorStyle::Hand);
						if (MouseL.down())
						{
							st.selectedLane = i;
							st.selectedPart = -1;
							st.dragAnchor = pixelToM(mx);
							if (onLeftEdge)       st.dragMode = 5;
							else if (onRightEdge) st.dragMode = 6;
							else                  st.dragMode = 4;
						}
						else if (MouseR.down())
						{
							edge.lanes[i].dir = (L.dir == LaneDir::Forward) ? LaneDir::Backward : LaneDir::Forward;
							dirty = true;
						}
					}
				}
				// 車線間ギャップのダブルクリック詰め（A 端・B 端を独立に処理）
			// default: 外側 → 内側 / Shift: 内側 → 外側 / Ctrl: 両側を広げる
			if (st.dragMode == 0 && edge.lanes.size() >= 2 && doubleClickedThisFrame)
			{
				struct LanePos { int idx; float aL, aR, bL, bR; };
				Array<LanePos> sorted;
				for (int i = 0; i < static_cast<int>(edge.lanes.size()); ++i)
				{
					const auto& L = edge.lanes[i];
					sorted << LanePos{ i, L.offsetA_L, L.offsetA_R, L.offsetB_L, L.offsetB_R };
				}
				sorted.sort_by([](const LanePos& a, const LanePos& b)
					{ return (a.aL + a.bL) < (b.aL + b.bL); });
				for (size_t i = 1; i < sorted.size(); ++i)
				{
					const float aGap = sorted[i].aL - sorted[i - 1].aR;
					const float bGap = sorted[i].bL - sorted[i - 1].bR;
					if (aGap <= 0.01f && bGap <= 0.01f) continue;
					const double pxL = mToPixel(Min(sorted[i - 1].aR, sorted[i - 1].bR));
					const double pxR = mToPixel(Max(sorted[i].aL,     sorted[i].bL));
					const RectF gap{ pxL, static_cast<double>(laneBarY),
						Max(pxR - pxL, 2.0), static_cast<double>(kLaneBarH) };
					if (!gap.mouseOver()) continue;
					auto& prev = edge.lanes[sorted[i - 1].idx];
					auto& cur  = edge.lanes[sorted[i].idx];
					if (KeyControl.pressed())
					{
						prev.offsetA_R += aGap * 0.5f; prev.offsetB_R += bGap * 0.5f;
						cur.offsetA_L  -= aGap * 0.5f; cur.offsetB_L  -= bGap * 0.5f;
					}
					else if (KeyShift.pressed())
					{
						prev.offsetA_L += aGap; prev.offsetA_R += aGap;
						prev.offsetB_L += bGap; prev.offsetB_R += bGap;
						cur.offsetA_L  -= aGap; cur.offsetA_R  -= aGap;
						cur.offsetB_L  -= bGap; cur.offsetB_R  -= bGap;
					}
					else
					{
						cur.offsetA_L -= aGap; cur.offsetA_R -= aGap;
						cur.offsetB_L -= bGap; cur.offsetB_R -= bGap;
					}
					dirty = true;
					break;
				}
			}

			// 選択車線の四隅マーカー（A_L / A_R / B_L / B_R 個別編集）
				// 色は 3D 空間のハンドルと統一: A 側=暖色（赤）、B 側=寒色（緑）、R 側は暗め
				if (st.selectedLane >= 0 && st.selectedLane < static_cast<int>(edge.lanes.size()))
				{
					const auto& SL = edge.lanes[st.selectedLane];
					struct Corner { double px; double py; int mode; StringView tip; ColorF col; };
					const double yTopC = static_cast<double>(laneBarY + 2);
					const double yBotC = static_cast<double>(laneBarY + kLaneBarH - 2);
					auto tint = [](bool atA, bool isRight) {
						const ColorF base = atA ? ColorF{1.0, 0.55, 0.45} : ColorF{0.45, 1.0, 0.55};
						return isRight ? (base * 0.7 + ColorF{0.15}) : base;
					};
					const Corner corners[4] = {
						{ mToPixel(SL.offsetA_L), yTopC, 7,  U"A端左 / ドラッグで幅変更", tint(true,  false) },
						{ mToPixel(SL.offsetB_L), yBotC, 8,  U"B端左 / ドラッグで幅変更", tint(false, false) },
						{ mToPixel(SL.offsetA_R), yTopC, 9,  U"A端右 / ドラッグで幅変更", tint(true,  true)  },
						{ mToPixel(SL.offsetB_R), yBotC, 10, U"B端右 / ドラッグで幅変更", tint(false, true)  },
					};
					for (const auto& c : corners)
					{
						const Circle mk{ c.px, c.py, 4.0 };
						const bool hover = mk.mouseOver();
						mk.draw(hover ? ColorF{ 1.0, 1.0, 0.4 } : c.col);
						mk.drawFrame(1.0, ColorF{ 0.1 });
						if (st.dragMode == 0 && hover)
						{
							Cursor::RequestStyle(CursorStyle::ResizeLeftRight);
							PanelWidget::tipFont = &pFont;
							PanelWidget::tipText = String{ c.tip };
							PanelWidget::tipPos  = Vec2{ c.px + 8, c.py + 8 };
							PanelWidget::tipActive = true;
							if (MouseL.down())
							{
								st.dragMode = c.mode;
								st.dragAnchor = pixelToM(Cursor::PosF().x);
							}
						}
					}
				}

				y += kLaneBarH + 2;

				if (st.selectedLane >= 0 && st.selectedLane < static_cast<int>(edge.lanes.size()))
				{
					auto& sl = edge.lanes[st.selectedLane];
					int bx = pX;

					PanelWidget::label(pBold, U"車線 {}"_fmt(st.selectedLane), bx, y, ColorF{0.8, 0.8, 1.0});
					bx += 46;
					dirty |= PanelWidget::cycle(pFont, sl.dir, drN, 2, bx, y, 66, kLH);
					bx += 68;
					dirty |= PanelWidget::cycle(pFont, sl.op, osN, 4, bx, y, 78, kLH);
					bx += 80;
					dirty |= TransportSectionControls::laneKind(pFont,sl,bx,y,kLH);
					bx += 68;
					if (PanelWidget::buttonDanger(pFont, U"X", bx, y, 18, kLH, U"削除"))
					{
						edge.lanes.remove_at(st.selectedLane);
						st.selectedLane = -1;
						dirty = true;
					}
					y += kLH;

					if (st.selectedLane >= 0)
					{
						bx = pX + 4;
						PanelWidget::label(pFont, U"幅", bx, y, ColorF{0.6});
						bx += 40;
						dirty |= PanelWidget::numberInput(pFont, sl.nominalWidth, 0.5f, 1.0f, 10.0f, bx, y, 38, kLH);
						bx += 44;
						dirty |= PanelWidget::toggle(pFont, U"左変更可", U"左変更不可", sl.canChangeLaneLeft, bx, y, 36, kLH);
						bx += 38;
						dirty |= PanelWidget::toggle(pFont, U"右変更可", U"右変更不可", sl.canChangeLaneRight, bx, y, 36, kLH);
						y += kLH;

						bx = pX + 4;
						PanelWidget::label(pFont, U"左区画線", bx, y, ColorF{0.6});
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
						dirty |= PanelWidget::numberInput(pFont, sl.offsetA_L, 0.25f, -50.f, 50.f, bx, y, 46, kLH, U"{:.2f}");
						bx += 50;
						PanelWidget::label(pFont, U"R", bx, y);
						bx += 12;
						dirty |= PanelWidget::numberInput(pFont, sl.offsetA_R, 0.25f, -50.f, 50.f, bx, y, 46, kLH, U"{:.2f}");
						y += kLH;

						bx = pX + 4;
						PanelWidget::label(pFont, U"B", bx, y, ColorF{0.5, 1.0, 0.5});
						bx += 14;
						PanelWidget::label(pFont, U"L", bx, y);
						bx += 12;
						dirty |= PanelWidget::numberInput(pFont, sl.offsetB_L, 0.25f, -50.f, 50.f, bx, y, 46, kLH, U"{:.2f}");
						bx += 50;
						PanelWidget::label(pFont, U"R", bx, y);
						bx += 12;
						dirty |= PanelWidget::numberInput(pFont, sl.offsetB_R, 0.25f, -50.f, 50.f, bx, y, 46, kLH, U"{:.2f}");
						y += kLH + 2;
					}
				}
			}
		}

		// 部品・車線ともに隙間・重なりを許容する（自動調整しない）
		// ギャップはダブルクリックで明示的に詰める
		return dirty;
	}
}


// =============================================================================
// 道路エッジ編集パネル
// =============================================================================

void GameScene::drawEdgePanel()
{
	if (!selectedEdgeId()) return;
	RoadEdge* edge = m_network.getEdge(*selectedEdgeId());
	if (!edge) { clearSelection(); return; }

	auto area = m_panelManager.beginContent(U"edge_info");
	if (!area) return;

	const auto& pFont = FontAsset(Asset::Panel14);
	const auto& pBold = FontAsset(Asset::PanelBold14);

	PanelBuilder ui(static_cast<int>(m_panelManager.getSize(U"edge_info").x));
	bool dirty = false;

	// 道路状態（計画・建設中・供用中・閉鎖・現存）
	{
		StringView stateLabel;
		ColorF stateColor;
		switch (edge->edgeState)
		{
		case EdgeState::Planned:            stateLabel = U"計画";   stateColor = ColorF{ 1.0, 0.85, 0.3 }; break;
		case EdgeState::UnderConstruction:  stateLabel = U"建設中"; stateColor = ColorF{ 1.0, 0.55, 0.2 }; break;
		case EdgeState::Open:               stateLabel = U"供用中"; stateColor = ColorF{ 0.5, 0.9, 0.5 }; break;
		case EdgeState::Closed:             stateLabel = U"閉鎖";   stateColor = ColorF{ 0.9, 0.4, 0.4 }; break;
		case EdgeState::Existing:           stateLabel = U"現存";   stateColor = ColorF{ 0.7, 0.85, 1.0 }; break;
		default:                            stateLabel = U"不明";   stateColor = ColorF{ 0.7 }; break;
		}
		ui.row(4, [&] {
			ui.label(U"状態", ColorF{ 0.6 });
			ui.label(U"●", stateColor);
			ui.label(String{ stateLabel }, stateColor);

			if (edge->edgeState == EdgeState::Planned)
			{
				const double startCost = RoadConstructionStart::estimateCost(m_network, { edge->id });
				const bool hasFunds = m_sandboxActive || RoadConstructionStart::canAfford(m_economy.funds, startCost);
				if (ui.button(U"建設", false, 60, U"{}を支出して建設を開始"_fmt(formatConstructionCost(startCost))) && hasFunds)
				{
					if (startRoadConstruction({ edge->id })) { dirty = true; }
				}
				if (!hasFunds)
				{
					ui.label(U"資金不足", ColorF{ 1.0, 0.4, 0.35 });
				}
			}
			else if (edge->edgeState == EdgeState::UnderConstruction)
			{
				double remaining = 60.0 - (m_clock.now - edge->constructionStartTime);
				if (edge->planId >= 0)
				{
					if (const RoadPlan* plan = m_network.getPlan(edge->planId))
					{
						if (plan->completionDate)
							remaining = *plan->completionDate - m_clock.now;
					}
				}
				remaining = Max(0.0, remaining);
				ui.label(U"残り {}"_fmt(formatConstructionDuration(remaining)), ColorF{ 0.8 });
			}
		});
		if (edge->edgeState == EdgeState::UnderConstruction)
		{
			const auto progress = RoadConstruction::progress(m_network,*edge,m_clock.now);
			ui.label(U"{}  {:.0f}%"_fmt(progress.name(),progress.total*100),ColorF{1,.77,.45});
		}
		ui.spacer(3);
	}

	ui.row(4, [&] {
		ui.label(U"A:{}  B:{}  {:.0f}m"_fmt(edge->nodeA, edge->nodeB, edge->length), ColorF{1.0});
		if (ui.button(U"A/B入替", false, 62, U"ノードA/Bを入れ替える"))
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
	});
	ui.spacer(2);

	// 道路種別
	{
		static constexpr StringView rtNames[] = { U"生活道路", U"幹線道路", U"自動車専用道", U"高速道路" };
		ui.row(4, [&] {
			ui.label(U"種別", ColorF{ 0.6 });
			if (ui.cycle(edge->roadType, rtNames, 4, 60)) { dirty = true; }
		});
		ui.spacer(2);
	}

	// 速度制限
	{
		ui.row(4, [&] {
			ui.label(U"速度制限", ColorF{ 0.6 });
			dirty |= RoadInspectorEdit::editSpeedLimit(*edge, [&](float& speedLimit)
			{
				ui.numberInput(speedLimit, 10.0f, 10.0f, 200.0f, U"{:.0f}", 44);
			}, [this](int nodeA, int nodeB)
			{
				notifyNetworkChanged({nodeA, nodeB});
			});
			ui.label(U"km/h", ColorF{ 0.5 });
			ui.label(U"W:{:.1f}m"_fmt(edge->totalWidth()));
			// デバッグ: 車両スポーン
			if (ui.button(U"車両生成", false, 48, U"このエッジに車両を生成"))
			{
				m_vehicleManager.spawnOnEdge(edge->id, *m_simGraph);
			}
		});

		// 高架トグル
		if (ui.toggle(U"設計高ON", U"地表追従", edge->useElevation, 100))
		{
			if (edge->useElevation)
			{
				m_network.generatePiersForEdge(edge->id, m_world);
			}
			else
			{
				m_network.removeObjectsByEdge(edge->id);
			}
			dirty = true;
		}
		ui.spacer(4);
	}

	// 所属路線（RoadRoute）- plan/22_road_route_spec.md
	if (!edge->routeIds.isEmpty())
	{
		ui.spacer(3);
		ui.row(4, [&] {
			ui.label(U"路線", ColorF{ 0.6 });
		});
		for (const int rid : edge->routeIds)
		{
			const RoadRoute* route = m_network.getRoute(rid);
			if (!route) continue;
			ui.row(4, [&] {
				ui.label(U"■", route->color);  // 色スウォッチ
				const bool active = (selectedRouteId() == rid);
				if (ui.button(route->name, active, 240, U"路線を編集"))
				{
					selectRoute(rid);
					m_routeNameEditState = TextEditState{};
					m_routeNameEditState.text = route->name;
					m_panelManager.show(U"route_info",
						U"路線 #{}"_fmt(rid), panelRightPos(U"route_info"));
				}
			});
		}
	}

	if (edge->planId >= 0)
	{
		if (const RoadPlan* plan = m_network.getPlan(edge->planId))
		{
			ui.spacer(3);
			ui.row(4, [&] {
				ui.label(U"計画", ColorF{ 0.6 });
				const bool active = (selectedRoadPlanId() == plan->id);
				const String label = U"#{} {}"_fmt(plan->id, plan->name);
				if (ui.button(label, active, 220, U"道路計画を選択"))
				{
					selectRoadPlan(plan->id);
				}
			});
			ui.row(4, [&] {
				ui.label(U"工期", ColorF{ 0.6 });
				const String state = (plan->state == PlanState::Planning) ? U"未着工"
					: (plan->state == PlanState::UnderConstruction) ? U"工事中"
					: U"完成";
				ui.label(U"{} / {}"_fmt(state, formatConstructionDuration(plan->constructionDuration)), ColorF{ 0.8 });
			});
		}
	}

	// 案内標識（plan/21_guide_sign_spec.md）
	dirty |= drawGuideSignSection(ui, *edge);

	// 断面編集（Parts + Lanes 共通関数）
	static SectionEditState edgeSectionState;
	int y = ui.height();
	dirty |= drawRoadSections(*edge, edgeSectionState, pFont, pBold, 6, y);
	EdgeSectionState::selectedPart = edgeSectionState.selectedPart;
	EdgeSectionState::selectedLane = edgeSectionState.selectedLane;

	ui.flush();
	m_panelManager.reportContentHeight(U"edge_info", y);

	if (dirty)
	{
		m_roadRenderer.invalidateEdgeCache(edge->id, edge->nodeA, edge->nodeB);
		m_roadRenderer.invalidateCachesAroundNode(edge->nodeA, m_network);
		m_roadRenderer.invalidateCachesAroundNode(edge->nodeB, m_network);
	}

	// 削除ボタン（危険操作のため赤系の色で描画）
	{
		const int btnW = 120;
		const int btnX = 6;
		const int btnY = y + 8;
		const auto& font = FontAsset(Asset::Panel14);
		const bool clicked = PanelWidget::buttonDanger(font, U"エッジを削除",
		                                               btnX, btnY, btnW, PanelBuilder::kLineH,
		                                               U"このエッジを削除します（接続数0のノードも削除）");
		PanelWidget::flushTooltip();
		m_panelManager.reportContentHeight(U"edge_info", btnY + PanelBuilder::kLineH + 6);
		if (clicked)
		{
			const int nA = edge->nodeA;
			const int nB = edge->nodeB;
			const int edgeId = edge->id;
			m_worldRenderer.invalidateTerrainForNode(nA);
			m_worldRenderer.invalidateTerrainForNode(nB);
			m_worldRenderer.invalidateTerrainForEdge(edgeId);
			m_network.removeEdge(edgeId);

			// 孤立ノード（接続なし）を削除
			if (auto* na = m_network.getNode(nA); na && na->attachments.isEmpty())
			{
				m_network.removeNode(nA);
			}
			if (auto* nb = m_network.getNode(nB); nb && nb->attachments.isEmpty())
			{
				m_network.removeNode(nB);
			}

			clearSelection();
			m_panelManager.hide(U"edge_info");
			m_roadRenderer.invalidateAllCaches();
			notifyNetworkChanged({ nA, nB });
			return;
		}
	}
}

// =============================================================================
// 案内標識編集セクション（drawEdgePanel から呼び出す）
// =============================================================================

bool GameScene::drawGuideSignSection(PanelBuilder& ui, RoadEdge& edge)
{
	return m_guideSignEditor.drawEdgeSection(ui, edge, m_network, m_panelManager, m_roadRenderer);
}


// =============================================================================
// 看板編集パネル（GuideSignEditor へフォワード）
// =============================================================================

void GameScene::drawGuideSignEditPanel()
{
	const bool dirty = m_guideSignEditor.drawEditPanel(m_network, m_panelManager, m_roadRenderer);
	if (dirty)
	{
		if (auto* gp = m_network.getGuideSign(m_guideSignEditor.editingId()))
		{
			const RoadEdge* edge = m_network.getEdge(gp->parentEdgeId);
			if (edge) m_roadRenderer.invalidateEdgeCache(gp->parentEdgeId, edge->nodeA, edge->nodeB);
			else      m_roadRenderer.invalidateAllCaches();
		}
	}
}

void GameScene::drawGuideSignEditorPanel()
{
	// close() が drawEditorPanel 内で呼ばれると editingId() が -1 になるため、先に取得する
	const int editingId = m_guideSignEditor.editingId();
	const bool dirty = m_guideSignEditor.drawEditorPanel(m_network, m_panelManager, m_roadRenderer);
	if (dirty)
	{
		if (auto* gp = m_network.getGuideSign(editingId))
		{
			const RoadEdge* edge = m_network.getEdge(gp->parentEdgeId);
			if (edge) m_roadRenderer.invalidateEdgeCache(gp->parentEdgeId, edge->nodeA, edge->nodeB);
			else      m_roadRenderer.invalidateAllCaches();
		}
	}
}

// =============================================================================
// 道路設置テンプレートパネル
// =============================================================================

void GameScene::drawRoadPlanPanel()
{
	if (m_mode != EditMode::RoadPlan) return;

	auto area = m_panelManager.beginContent(U"draw_template");
	if (!area) return;

	const auto& pFont = FontAsset(Asset::Panel14);
	const auto& pBold = FontAsset(Asset::PanelBold14);
	const int panelW = static_cast<int>(m_panelManager.getSize(U"draw_template").x);
	constexpr int kPad = 6;
	constexpr int kLH  = 17;
	int y = 0;

	RoadPlanToolbar::State toolbarState;
	toolbarState.preset = m_draftRoadPlan.preset;
	toolbarState.points = m_draftRoadPlan.editor.points().size();
	toolbarState.valid = m_draftRoadPlan.editor.valid() && !m_draftRoadPlan.draggedPoint;
	toolbarState.canUndo = m_draftRoadPlan.editor.canUndo();
	toolbarState.canRedo = m_draftRoadPlan.editor.canRedo();
	toolbarState.generated = m_draftRoadPlan.editor.generated();
	toolbarState.snapping = m_draftRoadPlan.snapping;
	toolbarState.width = m_drawTemplate.totalWidth();
	toolbarState.elevation=m_drawElevation;
	toolbarState.length = m_draftRoadPlan.editor.length();
	toolbarState.cost = m_network.estimatePlanCost(m_drawTemplate.roadType,m_draftRoadPlan.editor.constructionEquivalentLength());
	toolbarState.constructionSeconds = m_network.estimatePlanConstructionDuration(m_drawTemplate.roadType,m_draftRoadPlan.editor.constructionEquivalentLength());
	toolbarState.funds = m_sandboxActive ? Math::Inf : m_economy.funds;
	toolbarState.message = m_draftRoadPlan.message;
	toolbarState.error = m_draftRoadPlan.error;
	const auto action = RoadPlanToolbar::draw(pFont,pBold,panelW-10,toolbarState);
	using Action = RoadPlanToolbar::Action;
	if (action >= Action::Local && action <= Action::Tram)
	{
		m_draftRoadPlan.preset = static_cast<int>(action)-static_cast<int>(Action::Local);
		m_drawTemplate = RoadPlanDraft::makeRoadTemplate(m_draftRoadPlan.preset);
		rebuildDraftRoadPlan();
	}
	else if (action == Action::Generate) { generateDraftRoadPlan(); }
	else if (action == Action::Snap) { m_draftRoadPlan.snapping = !m_draftRoadPlan.snapping; }
	else if (action == Action::Undo && m_draftRoadPlan.editor.undo()) { rebuildDraftRoadPlan(); }
	else if (action == Action::Redo && m_draftRoadPlan.editor.redo()) { rebuildDraftRoadPlan(); }
	else if (action == Action::Clear) { clearDraftRoadPlan(); }
	else if (action == Action::Construct) { commitDraftRoadPlan(); }
	y = RoadPlanToolbar::kHeight+10;

	PanelWidget::label(pFont, U"計画名", kPad, y, ColorF{ 0.6 });
	PanelWidget::textInput(pFont, m_draftRoadPlan.nameEdit, kPad + 48, y, panelW - (kPad + 48) - kPad, kLH, 32);
	y += kLH + 2;

	PanelWidget::label(pFont, U"路線名", kPad, y, ColorF{ 0.6 });
	PanelWidget::textInput(pFont, m_draftRoadPlan.routeNameEdit, kPad + 48, y, panelW - (kPad + 48) - kPad, kLH, 32);
	y += kLH + 2;

	{
		const bool append = m_draftRoadPlan.appendToExistingRoute;
		if (PanelWidget::button(pFont, append ? U"既存路線へ追加" : U"新規路線作成", append, kPad, y, 110, kLH, U"保存先の路線モード"))
		{
			m_draftRoadPlan.appendToExistingRoute = !append;
		}
		y += kLH + 2;
	}

	if (m_draftRoadPlan.appendToExistingRoute)
	{
		for (const auto& route : m_network.routes())
		{
			if (route.id < 0) continue;
			const bool selected = (m_draftRoadPlan.routeId && *m_draftRoadPlan.routeId == route.id);
			if (PanelWidget::button(pFont, route.name, selected, kPad, y, panelW - kPad * 2, kLH, U"追加先路線を選択"))
			{
				m_draftRoadPlan.routeId = route.id;
				m_draftRoadPlan.routeNameEdit.text = route.name;
			}
			y += kLH + 2;
		}
	}

	static bool plansCollapsed = false;
	PanelWidget::section(pBold, U"計画一覧", plansCollapsed, 0, y, panelW, kLH, ColorF{ 0.7, 0.85, 1.0 });
	if (plansCollapsed)
	{
		PanelWidget::flushTooltip();
		m_panelManager.reportContentHeight(U"draw_template", y);
		return;
	}
	y += 2;
	for (const auto& plan : m_network.plans())
	{
		if (plan.id < 0) continue;
		const bool active = (selectedRoadPlanId() == plan.id);
		const String state = (plan.state == PlanState::Planning) ? U"未着工"
			: (plan.state == PlanState::UnderConstruction) ? U"工事中"
			: U"完成";
		if (PanelWidget::button(pFont, U"[{}] {}"_fmt(state, plan.name), active, kPad, y, panelW - kPad * 2, kLH, U"計画を選択"))
		{
			selectRoadPlan(plan.id);
		}
		y += kLH + 2;

		if (active)
		{
			const double planCost = static_cast<double>(plan.totalCost);
			const bool hasFunds = m_sandboxActive || RoadConstructionStart::canAfford(m_economy.funds, planCost);
			PanelWidget::label(pFont, U"延長 {:.0f}m / 概算 {}"_fmt(plan.totalLength, formatConstructionCost(planCost)),
				kPad + 8, y, ColorF{ 0.75 });
			y += kLH + 2;
			PanelWidget::label(pFont, U"工期 {}"_fmt(formatConstructionDuration(plan.constructionDuration)),kPad + 8,y,ColorF{0.75});
			y += kLH + 2;

			if (plan.state == PlanState::Planning)
			{
				PanelWidget::label(pFont, U"必要 {} / 資金 {:.1f}億円"_fmt(formatConstructionCost(planCost), m_economy.funds),
					 kPad + 8, y, hasFunds ? ColorF{ 0.65, 0.9, 0.65 } : ColorF{ 1.0, 0.4, 0.35 });
				y += kLH + 2;
				PanelWidget::label(pFont,U"着工時に用地内の建物・敷地を自動撤去",kPad+8,y,ColorF{1,.77,.45});
				y += kLH + 2;

				if (PanelWidget::button(pFont, U"着工", false, kPad + 8, y, 50, kLH, U"概算費用を支出して計画全体を着工") && hasFunds)
				{
					startRoadConstruction(plan.edgeIds);
				}
			}
			if (PanelWidget::buttonDanger(pFont, U"削除", kPad + 64, y, 50, kLH, U"計画を削除"))
			{
				const int planId = plan.id;
				const Array<int> edgeIds = plan.edgeIds;
				Array<int> dirtyNodes;
				if (plan.state != PlanState::Complete)
				{
					for (const int eid : edgeIds)
					{
						if (const RoadEdge* edge = m_network.getEdge(eid))
							dirtyNodes << edge->nodeA << edge->nodeB;
					}
					for (const int eid : edgeIds)
					{
						if (const RoadEdge* edge = m_network.getEdge(eid))
						{
							m_roadRenderer.invalidateEdgeCache(eid, edge->nodeA, edge->nodeB);
							m_worldRenderer.invalidateTerrainForNode(edge->nodeA);
							m_worldRenderer.invalidateTerrainForNode(edge->nodeB);
						}
						m_worldRenderer.invalidateTerrainForEdge(eid);
						m_network.removeEdge(eid);
					}
				}
				m_network.removePlan(planId);
				if (selectedRoadPlanId() == planId)
					m_selectedRoadPlanId = none;
				if (!dirtyNodes.isEmpty())
				{
					for (const int nid : dirtyNodes)
						m_roadRenderer.invalidateCachesAroundNode(nid, m_network);
					notifyNetworkChanged(dirtyNodes);
				}
				PanelWidget::flushTooltip();
				m_panelManager.reportContentHeight(U"draw_template", y + kLH + 6);
				return;
			}
			y += kLH + 2;
			if (plan.state == PlanState::UnderConstruction && !plan.edgeIds.isEmpty())
			{
				if (const auto* edge = m_network.getEdge(plan.edgeIds.front()))
				{
					ConstructionStatus::draw(pFont,RectF{kPad+8,y,panelW-32,70},RoadConstruction::progress(m_network,*edge,m_clock.now));
					y += 76;
				}
			}
		}
	}

	PanelWidget::flushTooltip();
	m_panelManager.reportContentHeight(U"draw_template", y);
}

void GameScene::drawDrawTemplatePanel()
{
	if (m_mode != EditMode::RoadDraw) return;

	auto area = m_panelManager.beginContent(U"draw_template");
	if (!area) return;

	RoadEdge* edge = &m_drawTemplate;

	const auto& pFont = FontAsset(Asset::Panel14);
	const auto& pBold = FontAsset(Asset::PanelBold14);

	const int panelW = static_cast<int>(m_panelManager.getSize(U"draw_template").x);
	constexpr int kPad = 6;
	constexpr int kLH  = 17;

	// ==========================================================================
	// Section 0: スタート/ゴール指定モード トグル
	// ==========================================================================
	int section0Height = kLH + 2;  // トグルボタン分
	{
		const bool toggled = PanelWidget::button(
			pFont,
			U"スタート/ゴール指定モード",
			m_autoPlaceMode,
			kPad, 0, panelW - kPad * 2, kLH,
			U"2 点クリックで自動経路探索・敷設");
		if (toggled)
		{
			m_autoPlaceMode = !m_autoPlaceMode;
			if (!m_autoPlaceMode)
				m_autoPlaceStart = none;
		}

		if (m_autoPlaceMode)
		{
			const String statusText = m_autoPlaceStart
				? U"スタート: ({:.0f}, {:.0f}, {:.0f})"_fmt(
				      m_autoPlaceStart->x, m_autoPlaceStart->y, m_autoPlaceStart->z)
				: U"スタート未指定 — クリックで地点を選択";
			PanelWidget::label(pFont, statusText, kPad, section0Height, ColorF{ 0.7, 1.0, 0.7 });
			section0Height += kLH + 2;
		}
	}

	// ==========================================================================
	// Section 1: 現在の組み合わせ（常時展開）
	// ==========================================================================

	// セクションヘッダ行: タイトルと「★ お気に入りに追加」ボタンを同じ行に配置
	{
		const int btnW = 140;
		const int btnX = panelW - kPad - btnW;
		const int headerY = section0Height;

		// ヘッダ背景
		RectF{ 0, static_cast<double>(headerY), static_cast<double>(panelW), static_cast<double>(kLH) }
			.draw(ColorF{ 0.1, 0.1, 0.15 });
		pBold(U"現在の組み合わせ").draw(Vec2{ kPad + 2, headerY }, ColorF{ 1.0, 1.0, 0.4 });

		if (PanelWidget::button(pFont, U"★ お気に入りに追加", false, btnX, headerY, btnW, kLH, U"現在の設定をお気に入りに保存"))
		{
			const String name = RoadTemplatePreset::autoName(*edge);
			m_roadPresets.addFavorite(RoadTemplatePreset::fromEdge(*edge, name));
			m_roadPresets.save();
		}
	}

	PanelBuilder ui(panelW, kPad, 2);
	bool dirty = false;

	// Section 0 + Section 1 ヘッダ行の合計分をオフセット
	ui.spacer(section0Height + kLH + 2);

	// 道路種別
	{
		static constexpr StringView rtNames[] = { U"生活道路", U"幹線道路", U"自動車専用道", U"高速道路" };
		ui.row(4, [&] {
			ui.label(U"種別", ColorF{ 0.6 });
			if (ui.cycle(edge->roadType, rtNames, 4, 60)) { dirty = true; }
		});
		ui.spacer(2);
	}

	// 速度制限
	{
		ui.row(4, [&] {
			ui.label(U"速度制限", ColorF{ 0.6 });
			if (ui.numberInput(edge->speedLimit, 10.0f, 10.0f, 200.0f, U"{:.0f}", 44)) { dirty = true; }
			ui.label(U"km/h", ColorF{ 0.5 });
			ui.label(U"W:{:.1f}m"_fmt(edge->totalWidth()));
		});
		ui.spacer(4);
	}

	// 断面編集（Parts + Lanes 共通関数）
	static SectionEditState tplSectionState;
	int y = ui.height();
	dirty |= drawRoadSections(*edge, tplSectionState, pFont, pBold, kPad, y);

	// ==========================================================================
	// Section 2: お気に入り
	// ==========================================================================
	{
		static bool favCollapsed = false;
		PanelWidget::section(pBold, U"お気に入り", favCollapsed, 0, y, panelW, kLH,
		                     ColorF{ 1.0, 0.85, 0.3 });

		if (!favCollapsed)
		{
			const auto& favs = m_roadPresets.favorites();
			if (favs.isEmpty())
			{
				PanelWidget::label(pFont, U"(なし) ヘッダの ★ で追加", kPad, y, ColorF{ 0.5 });
				y += kLH + 2;
			}
			else
			{
				// 削除インデックスを後処理（ループ内で erase しない）
				Optional<size_t> removePending;
				for (size_t i = 0; i < favs.size(); ++i)
				{
					const int rowY  = y;
					const int xBtn  = panelW - kPad - 20;
					const int xSel  = xBtn - 2 - 36;
					const int nameW = xSel - kPad - 2;

					PanelWidget::label(pFont, favs[i].name, kPad, rowY, ColorF{ 0.85 });

					if (PanelWidget::button(pFont, U"選択", false, xSel, rowY, 36, kLH, U"このプリセットを適用"))
					{
						favs[i].applyTo(*edge);
						dirty = true;
					}
					if (PanelWidget::buttonDanger(pFont, U"×", xBtn, rowY, 20, kLH, U"削除"))
					{
						removePending = i;
					}
					(void)nameW;
					y += kLH + 2;
				}
				if (removePending.has_value())
				{
					m_roadPresets.removeFavoriteAt(*removePending);
					m_roadPresets.save();
				}
			}
		}
	}

	// ==========================================================================
	// Section 3: デフォルト
	// ==========================================================================
	{
		static bool defCollapsed = false;
		PanelWidget::section(pBold, U"デフォルト", defCollapsed, 0, y, panelW, kLH,
		                     ColorF{ 0.7, 0.85, 1.0 });

		if (!defCollapsed)
		{
			const auto& defs = m_roadPresets.defaults();
			for (size_t i = 0; i < defs.size(); ++i)
			{
				const int rowY = y;
				const int xSel = panelW - kPad - 36;

				PanelWidget::label(pFont, defs[i].name, kPad, rowY, ColorF{ 0.8 });

				if (PanelWidget::button(pFont, U"選択", false, xSel, rowY, 36, kLH, U"このプリセットを適用"))
				{
					defs[i].applyTo(*edge);
					dirty = true;
				}
				y += kLH + 2;
			}
		}
	}

	(void)dirty;
	ui.flush();

	// ==========================================================================
	// Section 4: 所属ルート
	// ==========================================================================
	{
		static bool routeCollapsed = false;
		PanelWidget::section(pBold, U"所属ルート", routeCollapsed, 0, y, panelW, kLH,
		                     ColorF{ 0.5, 1.0, 0.7 });

		if (!routeCollapsed)
		{
			constexpr StringView kindNames[] = { U"高速道路", U"国道", U"都道府県道", U"市区町村道", U"名称路線" };

			// 既存ルート一覧（チェックボックス）
			for (const auto& route : m_network.routes())
			{
				if (route.id < 0) continue;
				const bool selected = m_pendingRouteIds.contains(route.id);
				const String lbl = U"[{}] {}"_fmt(kindNames[static_cast<int>(route.kind)], route.name);
				if (PanelWidget::button(pFont, lbl, selected, kPad, y, panelW - kPad * 2, kLH,
				                        U"このルートに含める/外す"))
				{
					if (selected)
						m_pendingRouteIds.remove(route.id);
					else
						m_pendingRouteIds << route.id;
				}
				y += kLH + 2;
			}

			// 「＋新規ルート」展開ボタン
			if (PanelWidget::button(pFont, m_newRouteState.expanded ? U"▲ 新規ルート" : U"＋ 新規ルート",
			                        m_newRouteState.expanded, kPad, y, panelW - kPad * 2, kLH, U"新規ルートを作成"))
			{
				m_newRouteState.expanded = !m_newRouteState.expanded;
			}
			y += kLH + 2;

			if (m_newRouteState.expanded)
			{
				// 種別 cycle
				{
					const int xLbl = kPad;
					const int xCtrl = xLbl + 50;
					PanelWidget::label(pFont, U"種別", xLbl, y, ColorF{ 0.6 });
					if (PanelWidget::cycle(pFont, m_newRouteState.kind, kindNames, 5, xCtrl, y,
					                       panelW - kPad - xCtrl, kLH))
					{
					}
					y += kLH + 2;
				}

				// 番号
				{
					const int xLbl = kPad;
					const int xCtrl = xLbl + 50;
					PanelWidget::label(pFont, U"番号", xLbl, y, ColorF{ 0.6 });
					PanelWidget::numberInput(pFont, m_newRouteState.number, 1, 0, 400,
					                         xCtrl, y, 60, kLH, U"{}");
					y += kLH + 2;
				}

				// 名前
				{
					const int xLbl = kPad;
					const int xCtrl = xLbl + 50;
					PanelWidget::label(pFont, U"名前", xLbl, y, ColorF{ 0.6 });
					PanelWidget::textInput(pFont, m_newRouteState.nameEdit, xCtrl, y,
					                        panelW - kPad - xCtrl, kLH, 32);
					y += kLH + 2;
				}

				// 作成ボタン
				if (PanelWidget::button(pFont, U"作成", false, kPad, y, 60, kLH, U"ルートを作成して選択に追加"))
				{
					const int newId = m_network.addRoute(
						m_newRouteState.kind,
						m_newRouteState.nameEdit.text,
						{},
						m_newRouteState.number);
					m_pendingRouteIds << newId;
					m_newRouteState.expanded = false;
					m_newRouteState.nameEdit = TextEditState{};
				}
				y += kLH + 2;
			}
		}
	}

	PanelWidget::flushTooltip();
	m_panelManager.reportContentHeight(U"draw_template", y);
}

// =============================================================================
// ノード編集パネル
// =============================================================================

void GameScene::drawNodePanel()
{
	if (!selectedNodeId()) return;
	RoadNode* node = m_network.getNode(*selectedNodeId());
	if (!node) { clearSelection(); return; }

	auto area = m_panelManager.beginContent(U"node_info");
	if (!area) return;

	const auto& pFont = FontAsset(Asset::Panel14);
	const auto& pBold = FontAsset(Asset::PanelBold14);

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
		const double prevY = node->position.y;
		if (PanelWidget::numberInput(pFont, node->position.y, 1.0, -100.0, 200.0, pX + 16, y, 54, kLH))
		{
			const double dy = node->position.y - prevY;
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
		static constexpr StringView ntNames[] = { U"端点", U"継ぎ目", U"交差点", U"分岐合流" };
		PanelWidget::label(pFont, U"種別", pX, y);
		dirty |= PanelWidget::cycle(pFont, node->type, ntNames, 4, pX + 36, y, 70, kLH);
		y += kLH + 2;
	}

	{
		static constexpr StringView trNames[] = { U"なめらか", U"急変" };
		PanelWidget::label(pFont, U"遷移", pX, y);
		dirty |= PanelWidget::cycle(pFont, node->transition, trNames, 2, pX + 42, y, 54, kLH);
		y += kLH + 4;
	}

	// Attachments セクション（折りたたみ可能）
	static bool attachCollapsed = false;
	{
		static constexpr StringView rtNames[] = { U"生活道路", U"幹線道路", U"自動車専用道", U"高速道路" };
		constexpr int kSectionW = 300;

		if (PanelWidget::section(pBold, U"接続 ({})"_fmt(node->attachments.size()), attachCollapsed, pX, y, kSectionW, kLH))
		{
			for (size_t i = 0; i < node->attachments.size(); ++i)
			{
				auto& att = node->attachments[i];
				const RoadEdge* e = m_network.getEdge(att.edgeId);

				PanelWidget::label(pFont, U"[{}] edge #{}"_fmt(i, att.edgeId), pX, y, ColorF{1.0});
				if (e)
					PanelWidget::label(pFont, U"{} {:.0f}km/h"_fmt(rtNames[static_cast<int>(e->roadType)], e->speedLimit), pX + 100, y);
				y += kLH;

				PanelWidget::label(pFont, U"横オフセット", pX + 10, y);
				dirty |= PanelWidget::numberInput(pFont, att.lateralOffset, 1.0f, -20.0f, 20.0f, pX + 34, y, 44, kLH);
				dirty |= PanelWidget::toggle(pFont, U"通過", U"通過", att.isThrough, pX + 84, y, 60, kLH);
				y += kLH;

				{
					static constexpr StringView tcNames[] = { U"なし", U"徐行優先", U"一時停止", U"信号" };
					PanelWidget::label(pFont, U"ctrl", pX + 10, y);
					if (PanelWidget::cycle(pFont, att.control, tcNames, 4, pX + 34, y, 52, kLH))
					{
						dirty = true;
						notifyNetworkChanged({ *selectedNodeId() });
					}
				}
				y += kLH;

				// コントロールポイント Y 編集
				if (auto* edge = m_network.getEdge(att.edgeId))
				{
					double& cpY = (edge->nodeA == node->id) ? edge->ctrlA.y : edge->ctrlB.y;
					PanelWidget::label(pFont, U"cpY", pX + 10, y);
					if (PanelWidget::numberInput(pFont, cpY, 1.0, -100.0, 200.0, pX + 34, y, 54, kLH))
					{
						// ベジェ形状が変わるので弧長を再計算
						if (const auto bez = m_network.getBezier(att.edgeId))
							edge->length = bez->totalLength;
						m_network.updateNodeCutoffs(node->id);
						m_network.updateLaneConnectionPaths(node->id);
						notifyNetworkChanged({ node->id });
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
		if (PanelWidget::button(pFont, U"中間ノードを削除", false, pX, y, 80, kLH, U"ノードを削除して2エッジを1本に結合"))
		{
			const int nid = *selectedNodeId();
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
				clearSelection();
				notifyNetworkChanged(neighbors);
				for (const int nid2 : neighbors)
					m_roadRenderer.invalidateCachesAroundNode(nid2, m_network);
			}
		}
		y += kLH + 4;
	}

	// Signal セクション
	{
		y += 4;
		const bool hasPlacement = node->signalPlacement.has_value();
		bool enabled = hasPlacement;
		PanelWidget::label(pBold, U"信号", pX, y, ColorF{ 1.0, 1.0, 0.4 });
		if (PanelWidget::toggle(pFont, U"ON", U"OFF", enabled, pX + 60, y, 50, kLH))
		{
			if (enabled && !hasPlacement)
			{
				const auto defIds = m_roadRenderer.signalRegistry().defIds();
				if (!defIds.isEmpty())
				{
					SignalPlacement sp;
					sp.signalDefId = defIds[0];
					sp.phases = m_network.buildDefaultSignalPhases(node->id);
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
			if (PanelWidget::button(pFont, U"編集...", false, pX + 120, y, 60, kLH, U"信号サイクルエディタを開く"))
			{
				const Vec2 panelSize = m_panelManager.getSize(U"signal_edit");
				const Vec2 ctr{
					(Scene::Width() - panelSize.x) * 0.5,
					(Scene::Height() - panelSize.y) * 0.5
				};
				m_panelManager.show(U"signal_edit",
					U"信号 - ノード #{}"_fmt(node->id), ctr);
			}
		}
		y += kLH + 2;

		// 矢印サブランプは LaneConnection の旋回分類から自動導出されるため UI 不要
	}

	// 案内標識セクション
	{
		y += 4;
		PanelWidget::label(pBold, U"案内標識", pX, y, ColorF{ 0.4, 1.0, 0.6 });
		if (PanelWidget::button(pFont, U"標識を配置", false, pX + 90, y, 80, kLH, U"この交差点の案内標識を再計算して設置"))
		{
			recomputeGuideSignsAroundNode(node->id);
		}
		y += kLH + 2;
	}

	PanelWidget::flushTooltip();
	m_panelManager.reportContentHeight(U"node_info", y);

	if (dirty)
	{
		m_network.updateNodeCutoffs(node->id);
		// attachment.control が変わった可能性があるため各エッジの標識を再生成
		for (const auto& att : node->attachments)
			m_network.recomputeAutoSignsForEdge(att.edgeId);
		m_roadRenderer.invalidateCachesAroundNode(node->id, m_network);
	}
}

void GameScene::recomputeGuideSignsAroundNode(int nodeId)
{
	const RoadNode* node = m_network.getNode(nodeId);
	if (!node)
	{
		return;
	}

	// 隣接するユニークなノード（self + 隣接ノード）をノード単位で処理する。
	// チェーン探索により標識の parentEdgeId が隣接エッジまで伸びるケースがあり、
	// エッジ単位で再計算すると後のエッジ処理が前のエッジで生成した標識を消してしまう。
	HashSet<int> nodesToProcess;
	nodesToProcess.insert(nodeId);
	for (const auto& att : node->attachments)
	{
		const RoadEdge* edge = m_network.getEdge(att.edgeId);
		if (!edge)
		{
			continue;
		}
		nodesToProcess.insert(edge->nodeA);
		nodesToProcess.insert(edge->nodeB);
	}
	for (int nid : nodesToProcess)
	{
		m_network.recomputeAutoGuideSignsForNode(nid);
	}

	// 生成された標識の parentEdgeId が直接接続エッジ以外の場合もあるため、
	// そのエッジの両端ノードのキャッシュも無効化する。
	HashSet<int> edgesToInvalidate;
	for (const auto& g : m_network.guideSigns())
	{
		if (g.id >= 0 && g.autoGenerated && g.parentEdgeId >= 0
			&& nodesToProcess.contains(g.sourceNodeId))
		{
			edgesToInvalidate.insert(g.parentEdgeId);
		}
	}
	m_roadRenderer.invalidateCachesAroundNode(nodeId, m_network);
	for (int eid : edgesToInvalidate)
	{
		const RoadEdge* e = m_network.getEdge(eid);
		if (!e)
		{
			continue;
		}
		m_roadRenderer.invalidateCachesAroundNode(e->nodeA, m_network);
		m_roadRenderer.invalidateCachesAroundNode(e->nodeB, m_network);
	}
}


// =============================================================================
// 道路路線（RoadRoute）編集パネル — plan/22_road_route_spec.md
// =============================================================================

void GameScene::drawRoutePanel()
{
	if (!selectedRouteId()) return;
	RoadRoute* route = m_network.getRoute(*selectedRouteId());
	if (!route || route->id < 0) { clearSelection(); m_panelManager.hide(U"route_info"); return; }

	auto area = m_panelManager.beginContent(U"route_info");
	if (!area) return;

	PanelBuilder ui(static_cast<int>(m_panelManager.getSize(U"route_info").x));
	bool structureDirty = false;  // 路線の kind/number 変更時: ガイド標識の再推論トリガ

	// ── 基本情報 ──
	ui.label(U"ID: {}   edges: {}"_fmt(route->id, route->edgeIds.size()), ColorF{ 1.0 });
	ui.spacer(2);

	// 名前（テキスト編集）
	ui.row(4, [&] {
		ui.label(U"名前", ColorF{ 0.6 });
		if (ui.textInput(m_routeNameEditState, 240, 64))
		{
			route->name = m_routeNameEditState.text;
			m_roadRenderer.invalidateRouteSigns(route->id);
		}
	});

	// 種別
	{
		static constexpr StringView kindNames[] = {
			U"高速道路", U"国道", U"都道府県道", U"市区町村道", U"名称路線"
		};
		ui.row(4, [&] {
			ui.label(U"種別", ColorF{ 0.6 });
			if (ui.cycle(route->kind, kindNames, 5, 90))
			{
				// kind 変更時はデフォルト色へリセット（ユーザ編集済みなら上書きしない方針だが v1 では単純化）
				route->color = RoadNetwork::defaultRouteColor(route->kind);
				structureDirty = true;
			}
		});
	}

	// 番号
	ui.row(4, [&] {
		ui.label(U"路線番号", ColorF{ 0.6 });
		if (ui.numberInput(route->number, 1, 0, 400, U"{}", 60))
		{
			structureDirty = true;
		}
	});

	// 色スウォッチ（表示のみ、将来のカラーピッカー用）
	ui.row(4, [&] {
		ui.label(U"色", ColorF{ 0.6 });
		ui.label(U"■■■■■", route->color);
	});

	ui.separator();

	// ── 構成エッジリスト（クリックで該当エッジへジャンプ） ──
	ui.label(U"構成エッジ", ColorF{ 0.9 }, true);
	for (size_t i = 0; i < route->edgeIds.size(); ++i)
	{
		const int eid = route->edgeIds[i];
		const RoadEdge* e = m_network.getEdge(eid);
		ui.row(4, [&] {
			ui.label(U"[{}]"_fmt(i), ColorF{ 0.5 });
			const bool active = (selectedEdgeId() == eid);
			const String lbl = e ? U"E#{}  {:.0f}m"_fmt(eid, e->length)
			                     : U"E#{}  (missing)"_fmt(eid);
			if (ui.button(lbl, active, 180, U"エッジを選択"))
			{
				selectEdge(eid);
				m_panelManager.show(U"edge_info",
					U"道路エッジ #{}"_fmt(eid), panelRightPos(U"edge_info"));
			}
		});
	}

	ui.spacer(4);

	// ── 着工ボタン ──
	{
		bool hasPlanned = false;
		for (const int eid : route->edgeIds)
		{
			const RoadEdge* e = m_network.getEdge(eid);
			if (e && e->edgeState == EdgeState::Planned)
			{
				hasPlanned = true;
				break;
			}
		}
		const double routeConstructionCost = RoadConstructionStart::estimateCost(m_network, route->edgeIds);
		const bool hasFunds = m_sandboxActive || RoadConstructionStart::canAfford(m_economy.funds, routeConstructionCost);
		if (hasPlanned)
		{
			ui.label(U"未着工 {} / 資金 {:.1f}億円"_fmt(formatConstructionCost(routeConstructionCost), m_economy.funds),
				hasFunds ? ColorF{ 0.65, 0.9, 0.65 } : ColorF{ 1.0, 0.4, 0.35 });
		}
		ui.row(4, [&] {
			if (ui.button(U"着工", hasPlanned && hasFunds, 60, U"概算費用を支出して Planned エッジを着工") && hasPlanned && hasFunds)
			{
				startRoadConstruction(route->edgeIds);
			}
			else if (hasPlanned && !hasFunds)
			{
				ui.label(U"資金不足", ColorF{ 1.0, 0.4, 0.35 });
			}
		});
	}

	ui.spacer(4);

	// ── 操作ボタン ──
	ui.row(4, [&] {
		if (ui.buttonDanger(U"削除", 70, U"この路線を削除"))
		{
			const int rid = route->id;
			m_network.removeRoute(rid);
			clearSelection();
			m_panelManager.hide(U"route_info");
		}
	});

	ui.flush();
	m_panelManager.reportContentHeight(U"route_info", ui.height());

	if (structureDirty)
	{
		// 路線の kind/number 変更は ガイド標識の自動生成テキストに影響
		notifyNetworkChanged(Array<int>{});
	}
}
