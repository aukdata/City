#include "GameScene.hpp"
#include "../ui/RoadDiagramStyle.hpp"
#include "../ui/PanelWidget.hpp"
#include "../asset/AssetRegistrar.hpp"

/// @file
/// @brief 交差点の信号現示の編集と模式図。道路断面の編集 UI から独立させる。

namespace
{
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

	/// @brief outward フレームの offset を返す（左端・右端を個別に受け取る）
	std::pair<float, float> outwardOffset(float oL, float oR, bool isNodeA)
	{
		if (isNodeA) return { oL, oR };
		return { -oR, -oL };
	}

	/// @brief 道路全幅の outward left/right を取得
	std::pair<float, float> getRoadExtent(const RoadEdge* edge, bool isNodeA)
	{
		float minL = 1e9f, maxR = -1e9f;
		for (const auto& p : edge->parts)
		{
			if (p.build != BuildState::Built) continue;
			const auto [l, r] = outwardOffset(p.offsetL(), p.offsetR(), isNodeA);
			minL = Min(minL, l);
			maxR = Max(maxR, r);
		}
		return { minL, maxR };
	}

	/// @brief 路盤の outward left/right を取得
	std::pair<float, float> getRoadbedExtent(const RoadEdge* edge, bool isNodeA)
	{
		float minL = 1e9f, maxR = -1e9f;
		for (const auto& p : edge->parts)
		{
			if (p.type != RoadPartType::Roadbed) continue;
			const auto [l, r] = outwardOffset(p.offsetL(), p.offsetR(), isNodeA);
			minL = Min(minL, l);
			maxR = Max(maxR, r);
		}
		return { minL, maxR };
	}

	/// @brief 信号編集パネルの交差点図を描画する
	/// @param[in,out] dirty 変更があった場合 true にセットされる
	void drawSignalDiagram(const RoadNode& node, const RoadNetwork& network,
	                       const Font& pFont, Vec2 panelSize, int kLeftW,
	                       const HashSet<int>& greenSet, SignalPhaseDef* curPhasePtr,
	                       bool& dirty)
	{
		constexpr int kPad = 6;
		const double rightX = kLeftW;
		const double rightW = panelSize.x - kLeftW;
		const double diagramSize = Min(rightW - kPad, 380.0);
		const Vec2 center{ rightX + rightW * 0.5, kPad + diagramSize * 0.5 };
		const double armLen = diagramSize * 0.28;
		constexpr double kScale = 4.5;

		auto flipY = [&](Vec2 p) -> Vec2 { return { p.x, 2.0 * center.y - p.y }; };

		// ---- カットオフ情報を収集（角度順ソート）----
		Array<EdgeCap2D> caps;
		for (const auto& att : node.attachments)
		{
			const RoadEdge* edge = network.getEdge(att.edgeId);
			if (!edge) continue;
			const auto bez = network.getBezier(att.edgeId);
			if (!bez) continue;

			const bool isNodeA = (edge->nodeA == node.id);
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

			const float cutoffArc = isNodeA ? cutoff : (bez->totalLength - cutoff);
			const Vec3 cutPos = bez->positionAt(cutoffArc);

			EdgeCap2D cap;
			cap.center  = center + Vec2{
				(cutPos.x - node.position.x) * kScale,
				(cutPos.z - node.position.z) * kScale
			};
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

			Array<Vec2> boundary;
			for (int i = 0; i < N; ++i)
			{
				const auto& capCur = caps[i];
				const auto& capNext = caps[(i + 1) % N];
				const auto [rbL, rbR] = getRoadbedExtent(capCur.edge, capCur.isNodeA);
				const auto [nbL, nbR] = getRoadbedExtent(capNext.edge, capNext.isNodeA);

				boundary << (capCur.center + capCur.right * (rbL * kScale));
				boundary << (capCur.center + capCur.right * (rbR * kScale));

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

			for (int k = 0; k < static_cast<int>(boundary.size()); ++k)
			{
				const int next = (k + 1) % static_cast<int>(boundary.size());
				Triangle{ center, flipY(boundary[next]), flipY(boundary[k]) }.draw(junctionColor);
			}
		}

		// ---- 各エッジアーム ----
		for (const auto& cap : caps)
		{
			const auto* edge = cap.edge;
			const Vec2& dn = cap.fwd;
			const Vec2& rt = cap.right;

			for (const auto& part : edge->parts)
			{
				if (part.build != BuildState::Built) continue;

				const auto [oL, oR] = outwardOffset(part.offsetL(), part.offsetR(), cap.isNodeA);
				const double pLeft  = static_cast<double>(oL) * kScale;
				const double pRight = static_cast<double>(oR) * kScale;

				const Vec2 nearL = cap.center + rt * pLeft;
				const Vec2 nearR = cap.center + rt * pRight;
				const Vec2 farL  = nearL + dn * armLen;
				const Vec2 farR  = nearR + dn * armLen;

				Quad{ flipY(nearL), flipY(nearR), flipY(farR), flipY(farL) }.draw(RoadDiagramStyle::partTypeColor(part.type));
			}

			const auto [totalL, totalR] = getRoadExtent(edge, cap.isNodeA);
			{
				const Vec2 outerL0 = cap.center + rt * (totalL * kScale);
				const Vec2 outerL1 = outerL0 + dn * armLen;
				const Vec2 outerR0 = cap.center + rt * (totalR * kScale);
				const Vec2 outerR1 = outerR0 + dn * armLen;
				Line{ flipY(outerL0), flipY(outerL1) }.draw(1.5, ColorF{ 1.0 });
				Line{ flipY(outerR0), flipY(outerR1) }.draw(1.5, ColorF{ 1.0 });
			}

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
							Line{ flipY(p0 + dn * dd), flipY(p0 + dn * d1) }.draw(1.0, RoadDiagramStyle::lineTypeColor(lt));
						}
					}
					else
					{
						Line{ flipY(p0), flipY(p1) }.draw(1.0, RoadDiagramStyle::lineTypeColor(lt));
					}
				};
				const float oL = cap.isNodeA ? lane.offsetA_L : lane.offsetB_L;
				const float oR = cap.isNodeA ? lane.offsetA_R : lane.offsetB_R;
				drawLaneLine(lane.lineLeft, oL);
				drawLaneLine(lane.lineRight, oR);
			}

			pFont(U"E{}"_fmt(cap.edgeId)).drawAt(flipY(cap.center + dn * (armLen + 12.0)), ColorF{ 0.8 });
		}

		// ---- LaneConnection 描画 + クリックトグル ----
		{
			auto worldToDiag = [&](const Vec3& w) -> Vec2
			{
				return flipY(center + Vec2{
					(w.x - node.position.x) * kScale,
					(w.z - node.position.z) * kScale
				});
			};

			constexpr int kBezDiv = 14;
			struct ConnDraw { int connId; bool isGreen; Array<Vec2> path; };
			Array<ConnDraw> draws;
			draws.reserve(node.laneConnections.size());
			for (const auto& conn : node.laneConnections)
			{
				const float bezLen = conn.path.totalLength;
				if (bezLen <= 0.0f) continue;
				ConnDraw dd;
				dd.connId  = conn.id;
				dd.isGreen = greenSet.contains(conn.id);
				dd.path.reserve(kBezDiv + 1);
				for (int k = 0; k <= kBezDiv; ++k)
				{
					const float s = (k / static_cast<float>(kBezDiv)) * bezLen;
					dd.path << worldToDiag(conn.path.positionAt(s));
				}
				draws << std::move(dd);
			}

			auto drawLines = [&](bool greenPass)
			{
				for (const auto& dd : draws)
				{
					if (dd.isGreen != greenPass) continue;
					const ColorF lineC = greenPass
						? ColorF{ 0.2, 0.95, 0.4, 0.85 }
						: ColorF{ 0.95, 0.25, 0.15, 0.55 };
					const double thickness = greenPass ? 2.5 : 1.5;
					for (int k = 0; k + 1 < static_cast<int>(dd.path.size()); ++k)
						Line{ dd.path[k], dd.path[k + 1] }.draw(thickness, lineC);
				}
			};
			drawLines(false);
			drawLines(true);

			for (const auto& dd : draws)
			{
				const Vec2 mid = dd.path[kBezDiv / 2];
				constexpr double r = 6.0;
				const ColorF handleC = dd.isGreen
					? ColorF{ 0.1, 0.95, 0.35 }
					: ColorF{ 0.95, 0.2, 0.1 };
				Circle{ mid, r }.draw(handleC);
				Circle{ mid, r }.drawFrame(1.2, ColorF{ 0.0, 0.0, 0.0, 0.7 });

				if (curPhasePtr)
				{
					const int hx = static_cast<int>(mid.x - r);
					const int hy = static_cast<int>(mid.y - r);
					const int hw = static_cast<int>(r * 2);
					auto hit = PanelWidget::hitTest(pFont, hx, hy, hw, hw, U"青/赤を切替");
					if (hit.clickL)
					{
						if (dd.isGreen) curPhasePtr->greenConnectionIds.remove(dd.connId);
						else            curPhasePtr->greenConnectionIds << dd.connId;
						dirty = true;
					}
				}
			}
		}
	}
}

void GameScene::drawSignalEditPanel()
{
	// Signal 選択 or Node 選択どちらでも対応
	const Optional<int> sigNodeId = (m_selection.kind == SelectionKind::Signal)
		? Optional<int>{ m_selection.id }
		: selectedNodeId();
	if (!sigNodeId) { m_panelManager.hide(U"signal_edit"); return; }
	RoadNode* node = m_network.getNode(*sigNodeId);
	if (!node || !node->signalPlacement) { m_panelManager.hide(U"signal_edit"); return; }

	auto area = m_panelManager.beginContent(U"signal_edit");
	if (!area) return;

	auto& sp = *node->signalPlacement;
	const auto& pFont = FontAsset(Asset::Panel14);
	const auto& pBold = FontAsset(Asset::PanelBold14);
	constexpr int kPad = 6;
	constexpr int kLH = 17;
	constexpr int kLeftW = 180;  // 左ペイン幅
	const Vec2 panelSize = m_panelManager.getSize(U"signal_edit");
	bool dirty = false;

	// ========================================
	// 左ペイン: フェーズ一覧
	// ========================================
	int ly = kPad;
	constexpr int kRowH = 38; // フェーズ行の高さ（2段: ランプ + 時間）

	PanelWidget::label(pBold, U"フェーズ", kPad, ly, ColorF{ 1.0, 1.0, 0.4 });
	if (PanelWidget::button(pFont, U"+", false, kLeftW - 26, ly, 20, kLH, U"フェーズを追加"))
	{
		SignalPhaseDef ph;
		ph.duration = 30.0f;
		sp.phases << std::move(ph);
		m_signalEditPhase = static_cast<int>(sp.phases.size()) - 1;
		dirty = true;
	}
	ly += kLH + 4;

	const int totalConn = static_cast<int>(node->laneConnections.size());

	float totalDuration = 0.0f;
	for (int pi = 0; pi < static_cast<int>(sp.phases.size()); ++pi)
	{
		auto& ph = sp.phases[pi];
		totalDuration += ph.duration + kYellowDuration;

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

		// 1段目: フェーズ番号 + LaneConnection 別の小ランプ
		int lx = kPad + 4;
		PanelWidget::label(pFont, U"P{}"_fmt(pi + 1), lx, ly + 1, selected ? ColorF{ 1.0 } : ColorF{ 0.7 });
		lx += 22;

		for (const auto& conn : node->laneConnections)
		{
			const bool g = ph.greenConnectionIds.contains(conn.id);
			const ColorF lampC = g ? ColorF{ 0.1, 0.9, 0.3 } : ColorF{ 0.9, 0.15, 0.1 };
			Circle{ Vec2{ lx + 3.0, ly + 8.0 }, 2.5 }.draw(lampC);
			lx += 7;
			if (lx > kLeftW - 32) break;  // 表示幅オーバー対策
		}

		// 削除ボタン（右端）
		if (sp.phases.size() > 1)
		{
			if (PanelWidget::button(pFont, U"x", false, kLeftW - 24, ly + 1, 16, kLH - 2, U"フェーズを削除"))
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

		// 2段目: 持続時間（実時間秒）+ 青連数
		if (PanelWidget::numberInput(pFont, ph.duration, 1.0f, 5.0f, 120.0f,
		                      kPad + 4, ly + kLH + 1, 56, kLH - 2))
		{
			dirty = true;
		}
		PanelWidget::label(pFont, U"秒  {}/{} 青"_fmt(ph.greenConnectionIds.size(), totalConn),
		                   kPad + 62, ly + kLH + 1, ColorF{ 0.55 });

		ly += kRowH + 3;
	}

	// サイクル合計
	ly += 4;
	PanelWidget::label(pBold, U"サイクル: {:.0f}秒"_fmt(totalDuration), kPad, ly, ColorF{ 0.9, 0.8, 0.4 });
	ly += kLH + 4;

	// ========================================
	// 右ペイン: 交差点図 + 信号表示
	// ========================================
	HashSet<int> greenSet;
	SignalPhaseDef* curPhasePtr = nullptr;
	if (m_signalEditPhase >= 0 && m_signalEditPhase < static_cast<int>(sp.phases.size()))
	{
		curPhasePtr = &sp.phases[m_signalEditPhase];
		for (const int cid : curPhasePtr->greenConnectionIds)
			greenSet.insert(cid);
	}

	drawSignalDiagram(*node, m_network, pFont, panelSize, kLeftW, greenSet, curPhasePtr, dirty);

	const double rightW = panelSize.x - kLeftW;
	const double diagramSize = Min(rightW - kPad, 380.0);
	const int totalHeight = Max(ly, static_cast<int>(diagramSize) + kPad * 2);

	if (dirty)
	{
		m_vehicleManager.markLightsDirty();
	}

	PanelWidget::flushTooltip();
	m_panelManager.reportContentHeight(U"signal_edit", totalHeight);
}
