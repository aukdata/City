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
		y += kLH + 4;
	}

	// Parts
	static constexpr StringView ptNames[] = { U"Roadbed", U"Shoulder", U"Median", U"Sidewalk",
		U"Gutter", U"Guard", U"Wall", U"Curb", U"Slope", U"Bike" };
	static constexpr StringView bsNames[] = { U"NotBuilt", U"Building", U"Built", U"Stub" };
	PanelWidget::label(pBold, U"Parts ({})"_fmt(edge->parts.size()), pX, y, ColorF{1.0, 1.0, 0.4});
	if (PanelWidget::button(pFont, U"+", false, pX + 72, y, 16, kLH, U"Add part"))
	{
		RoadPart np;
		np.type = RoadPartType::Roadbed; np.width = 3.5f;
		np.offset = edge->totalWidth() * 0.5f; np.build = BuildState::Built;
		edge->parts << np; dirty = true;
	}
	y += kLH;
	int partToRemove = -1;
	int partSwapA = -1, partSwapB = -1;
	for (int i = 0; i < static_cast<int>(edge->parts.size()); ++i)
	{
		auto& p = edge->parts[i];
		const int n = static_cast<int>(edge->parts.size());
		int bx = pX;

		if (i > 0     && PanelWidget::button(pFont, U"^", false, bx, y, 14, kLH)) { partSwapA = i; partSwapB = i - 1; }
		bx += 15;
		if (i < n - 1 && PanelWidget::button(pFont, U"v", false, bx, y, 14, kLH)) { partSwapA = i; partSwapB = i + 1; }
		bx += 17;
		if (PanelWidget::cycle(pFont, p.type, ptNames, 10, bx, y, 56, kLH)) dirty = true;
		bx += 58;
		if (PanelWidget::cycle(pFont, p.build, bsNames, 4, bx, y, 52, kLH)) dirty = true;
		bx += 54;
		PanelWidget::label(pFont, U"w", bx, y, ColorF{ 0.5 });
		if (PanelWidget::spin(pFont, p.width, 0.5f, 0.5f, 50.0f, bx + 10, y, 34, kLH)) dirty = true;
		bx += 46;
		PanelWidget::label(pFont, U"o", bx, y, ColorF{ 0.5 });
		if (PanelWidget::spin(pFont, p.offset, 0.5f, -50.0f, 50.0f, bx + 10, y, 38, kLH)) dirty = true;
		bx += 50;
		if (PanelWidget::button(pFont, U"X", false, bx, y, 16, kLH, U"Remove")) partToRemove = i;
		y += kLH;
	}
	if (partSwapA >= 0 && partSwapB >= 0) { std::swap(edge->parts[partSwapA], edge->parts[partSwapB]); dirty = true; }
	if (partToRemove >= 0) { edge->parts.remove_at(partToRemove); dirty = true; }
	y += 4;

	// Lanes
	static constexpr StringView osN[] = { U"Open", U"Provisional", U"Closed", U"Reserved" };
	static constexpr StringView ltN[] = { U"Normal", U"Bus", U"Climb", U"Turn", U"Accel", U"Decel" };
	static constexpr StringView lnN[] = { U"None", U"Solid W", U"Dash W", U"Solid Y", U"Double Y" };
	static constexpr StringView drN[] = { U"Forward", U"Backward" };
	PanelWidget::label(pBold, U"Lanes ({})"_fmt(edge->lanes.size()), pX, y, ColorF{1.0, 1.0, 0.4});
	if (PanelWidget::button(pFont, U"+", false, pX + 72, y, 16, kLH, U"Add lane"))
	{
		Lane nl; nl.dir = LaneDir::Forward; nl.op = OpState::Open; nl.nominalWidth = 3.5f;
		const float hw = edge->totalWidth() * 0.5f;
		nl.offsetA_L = hw; nl.offsetA_R = hw + 3.5f; nl.offsetB_L = hw; nl.offsetB_R = hw + 3.5f;
		edge->lanes << nl; dirty = true;
	}
	y += kLH;
	int laneToRemove = -1;
	int laneSwapA = -1, laneSwapB = -1;
	for (int i = 0; i < static_cast<int>(edge->lanes.size()); ++i)
	{
		auto& L = edge->lanes[i];
		const int ln = static_cast<int>(edge->lanes.size());
		int bx;

		bx = pX;
		if (i > 0      && PanelWidget::button(pFont, U"^", false, bx, y, 16, kLH)) { laneSwapA = i; laneSwapB = i - 1; }
		bx += 17;
		if (i < ln - 1 && PanelWidget::button(pFont, U"v", false, bx, y, 16, kLH)) { laneSwapA = i; laneSwapB = i + 1; }
		bx += 18;
		PanelWidget::label(pBold, U"Lane {}"_fmt(i), bx, y, ColorF{0.8, 0.8, 1.0});
		bx += 46;
		dirty |= PanelWidget::cycle(pFont, L.dir, drN, 2, bx, y, 66, kLH);
		bx += 68;
		dirty |= PanelWidget::cycle(pFont, L.op, osN, 4, bx, y, 78, kLH);
		bx += 80;
		dirty |= PanelWidget::cycle(pFont, L.type, ltN, 6, bx, y, 50, kLH);
		if (PanelWidget::button(pFont, U"X", false, pX + 348, y, 18, kLH, U"Remove")) laneToRemove = i;
		y += kLH;

		bx = pX + 8;
		PanelWidget::label(pFont, U"Change", bx, y);
		bx += 50;
		dirty |= PanelWidget::toggle(pFont, U"Left OK", U"Left --", L.canChangeLaneLeft, bx, y, 56, kLH);
		bx += 58;
		dirty |= PanelWidget::toggle(pFont, U"Right OK", U"Right --", L.canChangeLaneRight, bx, y, 58, kLH);
		bx += 64;
		PanelWidget::label(pFont, U"Width", bx, y);
		bx += 40;
		dirty |= PanelWidget::spin(pFont, L.nominalWidth, 0.5f, 1.0f, 10.0f, bx, y, 38, kLH);
		y += kLH;

		bx = pX + 8;
		PanelWidget::label(pFont, U"Line Left", bx, y);
		bx += 72;
		dirty |= PanelWidget::cycle(pFont, L.lineLeft, lnN, 5, bx, y, 66, kLH);
		bx += 72;
		PanelWidget::label(pFont, U"Right", bx, y);
		bx += 42;
		dirty |= PanelWidget::cycle(pFont, L.lineRight, lnN, 5, bx, y, 66, kLH);
		y += kLH;

		bx = pX + 8;
		PanelWidget::label(pFont, U"A", bx, y, ColorF{1.0, 0.5, 0.5});
		bx += 14;
		PanelWidget::label(pFont, U"Left", bx, y);
		bx += 34;
		dirty |= PanelWidget::spin(pFont, L.offsetA_L, 0.25f, -50.f, 50.f, bx, y, 46, kLH, U"{:.2f}");
		bx += 50;
		PanelWidget::label(pFont, U"Right", bx, y);
		bx += 40;
		dirty |= PanelWidget::spin(pFont, L.offsetA_R, 0.25f, -50.f, 50.f, bx, y, 46, kLH, U"{:.2f}");
		y += kLH;

		bx = pX + 8;
		PanelWidget::label(pFont, U"B", bx, y, ColorF{0.5, 1.0, 0.5});
		bx += 14;
		PanelWidget::label(pFont, U"Left", bx, y);
		bx += 34;
		dirty |= PanelWidget::spin(pFont, L.offsetB_L, 0.25f, -50.f, 50.f, bx, y, 46, kLH, U"{:.2f}");
		bx += 50;
		PanelWidget::label(pFont, U"Right", bx, y);
		bx += 40;
		dirty |= PanelWidget::spin(pFont, L.offsetB_R, 0.25f, -50.f, 50.f, bx, y, 46, kLH, U"{:.2f}");
		y += kLH + 4;
	}
	if (laneSwapA >= 0 && laneSwapB >= 0) { std::swap(edge->lanes[laneSwapA], edge->lanes[laneSwapB]); dirty = true; }
	if (laneToRemove >= 0) { edge->lanes.remove_at(laneToRemove); dirty = true; }

	PanelWidget::flushTooltip();
	m_panelManager.reportContentHeight(U"edge_info", y);

	if (dirty)
	{
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

	static constexpr StringView rtNames[] = { U"Local", U"Arterial", U"Express", U"Highway" };
	PanelWidget::label(pBold, U"Attachments ({})"_fmt(node->attachments.size()), pX, y, ColorF{1.0, 1.0, 0.4});
	y += kLH;

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
