#include "GameScene.hpp"
#include "../ui/LandParcelPanel.hpp"
#include "../ui/PanelWidget.hpp"
#include "../ui/PanelLayout.hpp"
#include "../asset/AssetRegistrar.hpp"

/// @file
/// @brief 地名・車両・建物・敷地の情報表示と一時停止メニュー。選択状態は GameScene が所有する。

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
		switch (s.kind)
		{
		case MapGenerator::SettlementKind::RegionalCity:
			typeStr = U"[城]"; typeColor = ColorF{ 1.0, 0.4, 0.4 }; break;
		case MapGenerator::SettlementKind::LocalTown:
			typeStr = U"[宿]"; typeColor = ColorF{ 0.4, 0.8, 1.0 }; break;
		default:
			typeStr = U"[村]"; typeColor = ColorF{ 0.6, 0.8, 0.5 }; break;
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
			leaveDriving(true);
			m_camera.setFocus(Vec3{ s.center.x, h, s.center.y });
			if (m_camera.mode() != CameraMode::Overview)
				m_camera.cycleMode();
		}

		y += kLineH;
	}
	m_panelManager.reportContentHeight(U"name_list", y);
}

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

	const auto& pFont = FontAsset(Asset::Panel14);

	PanelBuilder ui(static_cast<int>(m_panelManager.getSize(U"vehicle_info").x));

	static constexpr StringView typeNames[] = {
		U"乗用車", U"軽自動車", U"原付", U"小型車",
		U"バス", U"小型トラック", U"大型トラック", U"緊急車両"
	};
	const int typeIdx = static_cast<int>(veh->type);
	ui.label(U"種別: {}"_fmt(typeIdx < 8 ? typeNames[typeIdx] : U"?"), ColorF{1.0});
	ui.label(U"速度: {:.1f} km/h"_fmt(veh->speed * 3.6f), ColorF{1.0});

	static constexpr StringView locNames[] = { U"車線上", U"交差点内", U"車線変更中" };
	ui.label(U"位置種別: {}"_fmt(locNames[static_cast<int>(veh->location)]), ColorF{1.0});
	ui.label(U"エッジ: {}  車線: {}"_fmt(veh->currentEdge, veh->currentLane), ColorF{1.0});

	ui.row(4, [&] {
		ui.label(U"目標エッジ: {}"_fmt(veh->goalEdgeId), ColorF{1.0});
		// 選択中エッジをゴールに設定するボタン
		if (selectedEdgeId())
		{
			if (ui.button(U"目標に設定", false, 70, U"選択中エッジを目標に設定"))
			{
				m_vehicleManager.setGoalAndReroute(veh->id, *selectedEdgeId(), *m_simGraph);
			}
		}
	});
	ui.spacer(4);

	// 追跡ボタン
	if (ui.button(m_trackingVehicle ? U"追跡中" : U"追跡", m_trackingVehicle, 120))
	{
		m_trackingVehicle = !m_trackingVehicle;
	}
	ui.spacer(6);

	// 経路ウェイポイント
	const int wpCount = static_cast<int>(veh->routeWaypoints.size());
	ui.label(U"経路: {}/{} 点"_fmt(veh->routeIdx, wpCount), ColorF{1.0, 1.0, 0.4}, true);
	ui.spacer(2);

	if (wpCount == 0)
	{
		ui.label(veh->routeRequested ? U"(経路要求中...)" : U"(経路なし)", ColorF{0.6});
	}

	// ウェイポイントリスト（手動座標制御）
	constexpr int kLH = 17;
	constexpr int kPad = 6;
	int y = ui.height();
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

		const RectF itemRect{ static_cast<double>(kPad), static_cast<double>(y),
			260.0, static_cast<double>(kLH) };
		const bool itemHover = itemRect.mouseOver();

		if (itemHover)
		{
			itemRect.draw(ColorF{ 0.3, 0.3, 0.5, 0.4 });
		}

		const ColorF color = isPast ? ColorF{ 0.4 }
			: (isCurrent ? ColorF{ 0.0, 1.0, 1.0 }
			: (itemHover ? ColorF{ 1.0, 1.0, 0.0 } : ColorF{ 1.0 }));
		PanelWidget::label(pFont, label, kPad + 2, y, color);

		if (itemHover && MouseL.down() && edge)
		{
			const RoadNode* node = m_network.getNode(edge->nodeA);
			if (node)
			{
				leaveDriving(true);
				m_camera.setFocus(node->position);
			}
		}

		y += kLH;
	}

	ui.flush();
	m_panelManager.reportContentHeight(U"vehicle_info", y);
}

void GameScene::drawBuildingPanel()
{
	if (!m_selectedBuilding)
	{
		m_panelManager.hide(U"building_info");
		return;
	}
	const auto& ref = *m_selectedBuilding;
	Chunk* chunk = m_world.getChunk(Point{ ref.chunkX, ref.chunkZ });
	if (!chunk)
	{
		m_panelManager.hide(U"building_info");
		return;
	}
	const Building& b = chunk->buildingGrid[{ ref.col, ref.row }];
	if (b.type == BuildingType::None)
	{
		m_panelManager.hide(U"building_info");
		return;
	}
	const auto hitBox = m_worldRenderer.buildingHitBox(*chunk, m_world, ref.col, ref.row);

	auto area = m_panelManager.beginContent(U"building_info");
	if (!area) return;

	PanelBuilder ui(static_cast<int>(m_panelManager.getSize(U"building_info").x));

	static constexpr StringView typeNames[] = {
		U"(None)", U"戸建て住宅", U"低層マンション", U"中層マンション", U"高層マンション",
		U"店舗", U"オフィス", U"工場", U"農地", U"公園", U"公共施設", U"駐車場", U"コンビニ（都市型）", U"コンビニ（郊外型）",
		U"給油所（都市型）", U"給油所（郊外型）", U"田舎の民家",
		U"高層オフィス", U"市役所", U"郊外ショッピングモール", U"総合病院", U"学校"
	};
	const int typeIdx = static_cast<int>(b.type);
	const StringView typeName = (typeIdx >= 0 && typeIdx < static_cast<int>(std::size(typeNames)))
		? typeNames[typeIdx] : U"?";

	constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
	const Vec3 origin = chunk->worldOrigin();
	const double cx = origin.x + (ref.col + 0.5) * cellSize;
	const double cz = origin.z + (ref.row + 0.5) * cellSize;

	ui.label(U"種別: {}"_fmt(typeName), ColorF{1.0});
	if (hitBox)
	{
		ui.label(U"描画サイズ: {:.1f} x {:.1f} x {:.1f} m"_fmt(
			hitBox->size.x, hitBox->size.y, hitBox->size.z), ColorF{1.0});
	}
	else
	{
		ui.label(U"描画高さ: {:.1f} m"_fmt(buildingHeight(b.type)), ColorF{1.0});
	}
	const int cap = buildingCapacity(b.type);
	if (cap > 0)
		ui.label(U"収容: {} 人"_fmt(cap), ColorF{1.0});
	ui.label(U"建設時刻: {:.1f}"_fmt(b.builtAt), ColorF{0.8, 0.8, 0.8});
	ui.label(U"向き: {:.1f}°"_fmt(Math::ToDegrees(b.angle)), ColorF{0.8, 0.8, 0.8});
	ui.label(U"接道エッジ: {}  t={:.3f}"_fmt(b.edgeId, b.edgeT), ColorF{0.8, 0.8, 0.8});
	if (b.edgeId >= 0)
	{
		if (const auto bez = m_network.getBezier(b.edgeId))
		{
			const float t = Clamp(b.edgeT, 0.0f, 1.0f);
			const float arc = bez->totalLength * t;
			Vec3 p = bez->positionAt(arc);
			if (const RoadEdge* e = m_network.getEdge(b.edgeId))
			{
				if (e->useElevation) p.y += 4.0;
				else p.y = m_world.sampleHeight(static_cast<float>(p.x), static_cast<float>(p.z));
			}
			ui.label(U"接道位置: ({:.1f}, {:.1f}, {:.1f})"_fmt(p.x, p.y, p.z), ColorF{0.8, 0.8, 0.8});
		}
	}
	ui.label(U"位置: ({:.0f}, {:.0f})"_fmt(cx, cz), ColorF{0.8, 0.8, 0.8});
	ui.label(U"Chunk({}, {}) Cell({}, {})"_fmt(ref.chunkX, ref.chunkZ, ref.col, ref.row),
		ColorF{0.6, 0.6, 0.6});
	ui.spacer(6);
	if (ui.buttonDanger(U"建物を削除", 120, U"この建物を削除して空き地に戻す"))
	{
		chunk->buildingGrid[{ ref.col, ref.row }] = Building{};
		chunk->meshDirty = true;
		clearSelection();
		m_panelManager.hide(U"building_info");
		ui.flush();
		m_panelManager.reportContentHeight(U"building_info", ui.height());
		return;
	}

	ui.flush();
	m_panelManager.reportContentHeight(U"building_info", ui.height());
}

void GameScene::resumeFromPauseMenu()
{
	m_showPauseMenu=false;
	m_clock.speed=m_pauseResumeSpeed;
}

void GameScene::drawPauseMenu()
{
	const auto action=m_pauseMenu.draw(FontAsset(Asset::UI20),Scene::Size(),getData().effectVolume);
	if (!action) { return; }
	switch (*action)
	{
	case PauseMenu::Action::Resume: resumeFromPauseMenu(); break;
	case PauseMenu::Action::Save: saveGame(); break;
	case PauseMenu::Action::Title:
		m_showPauseMenu=false;
		changeScene(SceneState::Title,0s);
		break;
	case PauseMenu::Action::Quit: System::Exit(); break;
	case PauseMenu::Action::Volume:
		getData().effectVolume=getData().effectVolume>.6 ? .6 : (getData().effectVolume>.3 ? .3 : (getData().effectVolume>0 ? 0 : 1));
		m_soundEffects.setVolume(getData().effectVolume);
		m_soundEffects.play(SoundEffects::Cue::Select);
		break;
	}
}

void GameScene::drawLandParcelPanel()
{
	if (!m_selectedLandParcel || m_selection.kind!=SelectionKind::LandParcel) { m_panelManager.hide(U"land_info"); return; }
	if (!m_panelManager.isVisible(U"land_info")) { clearSelection(); return; }
	const Chunk* chunk=m_world.getChunk(m_selectedLandParcel->chunkCoord);
	if (!chunk) { clearSelection(); return; }
	const LandPatch* patch=nullptr;
	for (const auto& candidate : chunk->landPatches) { if (candidate.id==m_selectedLandParcel->id) { patch=&candidate; break; } }
	if (!patch) { clearSelection(); return; }
	if (m_landParcelRevision!=m_worldRenderer.geometryRevision())
	{
		const MeshData surface=m_worldRenderer.landPatchSurface(m_world,m_network,chunk->coord,*patch);
		m_landParcelOutline=surface.indices.isEmpty() ? Mesh{} : Mesh{surface};
		m_landParcelRevision=m_worldRenderer.geometryRevision();
	}
	const auto area=m_panelManager.beginContent(U"land_info");
	if (!area) { return; }
	LandParcelPanel::draw(FontAsset(Asset::Panel14),*patch,{8,10});
	m_panelManager.reportContentHeight(U"land_info",120);
}
