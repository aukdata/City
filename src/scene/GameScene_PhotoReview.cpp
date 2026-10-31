#include "GameScene.hpp"

/// @file
/// @brief 実写（長野道・山間の国道・地方都市の幹線）と同じ構図の運転席目線を自動で探して撮影する。
/// @details `--capture-photos` で起動する。場面ごとに条件に合う道路を探し、見つからない場面は飛ばしてログに残す。

namespace
{
	/// @brief 道路の進行方向の左車線の中心。往復道路では A→B 向きの車線を使う。
	double leftLaneOffset(const RoadEdge& edge)
	{
		double best = 0; bool found = false;
		for (const auto& lane : edge.lanes)
		{
			if (lane.dir != LaneDir::Forward) { continue; }
			const double center = (lane.offsetA_L + lane.offsetA_R) * .5;
			if (!found || center < best) { best = center; found = true; }
		}
		return best;
	}

	double segmentDistance(Vec2 a, Vec2 b, Vec2 p)
	{
		const Vec2 ab = b - a;
		const double t = Clamp((p - a).dot(ab) / Max(1e-9, ab.lengthSq()), 0.0, 1.0);
		return p.distanceFrom(a + ab * t);
	}

	bool expressway(const RoadEdge& edge) { return edge.id >= 0 && edge.roadType == RoadType::Expressway; }
	bool mainline(const RoadEdge& edge) { return expressway(edge) && edge.lanes.size() >= 2 && edge.lanes.all([](const Lane& lane) { return lane.dir == LaneDir::Forward; }); }
}

void GameScene::updatePhotoReview()
{
	constexpr int kViews = 17;
	// 地形・建物・道路設備の読み込みが落ち着くまで待つ上限。ソフトウェア描画では 1 フレーム数秒かかる。
	constexpr int kMaximumFrames = 160;
	static constexpr std::array<StringView, kViews> kNames{
		U"expressway_ahead", U"expressway_exit", U"expressway_powerline", U"expressway_noise_barrier", U"expressway_embankment",
		U"expressway_information_board", U"expressway_spacing_check", U"mountain_tunnel_portal", U"mountain_tunnel_inside",
		U"mountain_bridge", U"mountain_road", U"valley_road", U"town_arterial", U"gorge_road", U"river_meander", U"full_map", U"full_map_region" };
	m_clock.speed = TimeSpeed::Paused; m_clock.hour = 13;
	m_camera.setFirstPersonFov(50_deg);
	// 写真は 10 月末の撮影。山の紅葉の進みを合わせる。
	m_autumnOverride = .8;
	// 実写はどれも曇天。
	m_overcast = .85;
	const auto place = [&](int edgeId, double arc, double lateral, double pitch = 0)
	{
		const auto* edge = m_network.getEdge(edgeId); const auto curve = m_network.getBezier(edgeId);
		if (!edge || !curve) { return false; }
		arc = Clamp(arc, 0.0, static_cast<double>(curve->totalLength));
		const Vec3 tangent = curve->tangentAt(static_cast<float>(arc));
		Vec3 p = curve->positionAt(static_cast<float>(arc)) + tangentToRight(tangent) * lateral;
		p.y = (edge->usesDesignHeight() ? curve->positionAt(static_cast<float>(arc)).y : m_world.sampleHeight(static_cast<float>(p.x), static_cast<float>(p.z))) + kRoadSurfaceLift;
		m_camera.setWalkingState(p, static_cast<float>(Atan2(tangent.x, tangent.z)), static_cast<float>(pitch));
		return true;
	};
	const auto guideOf = [&](auto predicate) -> const GuideSignPlacement*
	{
		for (const auto& sign : m_network.guideSigns()) { if (sign.id >= 0 && predicate(sign)) { return &sign; } }
		return nullptr;
	};
	// 標識の手前 distance m（同じエッジ上で足りない分は手前のエッジから）に立つ。
	const auto beforeSign = [&](const GuideSignPlacement& sign, double distance)
	{
		const auto* edge = m_network.getEdge(sign.parentEdgeId); if (!edge) { return false; }
		const bool atA = sign.nodeEndId == edge->nodeA;
		const double arc = atA ? edge->cutoffA + sign.arcOffset : edge->length - edge->cutoffB - sign.arcOffset;
		DBG_LOG(U"[PhotoReview] sign edge={} arc={:.0f} length={:.0f} lateral={:.1f} size={}x{}"_fmt(edge->id, arc, edge->length, sign.lateralOffset, sign.widthOverride, sign.heightOverride));
		if (arc >= distance) { return place(edge->id, arc - distance, leftLaneOffset(*edge)); }
		for (const auto& previous : m_network.edges())
		{
			if (mainline(previous) && previous.nodeB == edge->nodeA) { return place(previous.id, previous.length - (distance - arc), leftLaneOffset(previous)); }
		}
		return place(edge->id, 0, leftLaneOffset(*edge));
	};
	if (m_captureCameraDirty)
	{
		// --capture-views で指定した場面だけを撮る。
		const auto& only = getData().capturePhotoViews;
		while (!only.isEmpty() && !only.contains(String{ kNames[m_captureIndex] }))
		{
			if (++m_captureIndex == kViews) { System::Exit(); return; }
		}
		bool found = false;
		const String name{ kNames[m_captureIndex] };
		if (name == U"expressway_ahead")
		{
			// 地上区間で、前方に出口予告の案内標識が見える位置。
			if (const auto* sign = guideOf([&](const GuideSignPlacement& s)
			{
				const auto* edge = m_network.getEdge(s.parentEdgeId);
				return edge && mainline(*edge) && !edge->useElevation && s.bgColor.g > .3 && s.bgColor.r < .1 && s.elements.size() <= 4 && s.widthOverride > 3;
			})) { found = beforeSign(*sign, 110); }
		}
		else if (name == U"expressway_exit")
		{
			for (const auto& object : m_network.objects())
			{
				if (object.id < 0 || object.type != RoadObjectType::DivergeBlinker) { continue; }
				const auto* ramp = m_network.getEdge(object.parentEdgeId); if (!ramp) { continue; }
				for (const auto& edge : m_network.edges())
				{
					if (mainline(edge) && edge.nodeB == ramp->nodeA && edge.length > 120) { found = place(edge.id, edge.length - 110, leftLaneOffset(edge)); break; }
				}
				if (found) { break; }
			}
		}
		else if (name == U"expressway_powerline")
		{
			// 送電線が本線を横切る地点の 220m 手前。
			for (const auto& line : m_roadRenderer.powerLines())
			{
				for (size_t i = 0; !found && i + 1 < line.towers.size(); ++i)
				{
					const Vec2 a{ line.towers[i].base.x, line.towers[i].base.z }, b{ line.towers[i + 1].base.x, line.towers[i + 1].base.z };
					for (const auto& edge : m_network.edges())
					{
						if (!mainline(edge) || edge.tunnel) { continue; }
						const auto curve = m_network.getBezier(edge.id);
						for (float arc = 0; arc < curve->totalLength; arc += 10)
						{
							const Vec3 p = curve->positionAt(arc);
							if (segmentDistance(a, b, Vec2{ p.x, p.z }) < 25 && arc > 230) { found = place(edge.id, arc - 220, leftLaneOffset(edge)); break; }
						}
						if (found) { break; }
					}
				}
				if (found) { break; }
			}
		}
		else if (name == U"expressway_noise_barrier")
		{
			for (const auto& edge : m_network.edges())
			{
				if (mainline(edge) && edge.length > 150 && edge.parts.any([](const RoadPart& part) { return part.defId == U"expressway_noise_barrier"; }))
				{
					found = place(edge.id, 15, leftLaneOffset(edge)); break;
				}
			}
		}
		else if (name == U"expressway_embankment")
		{
			// 盛土の本線を、法尻の外の田畑から見上げる。
			for (const auto& edge : m_network.edges())
			{
				if (!mainline(edge) || edge.useElevation || edge.tunnel || edge.length < 150) { continue; }
				const auto curve = m_network.getBezier(edge.id);
				const Vec3 p = curve->positionAt(edge.length * .5f), tangent = curve->tangentAt(edge.length * .5f), right = tangentToRight(tangent);
				const Vec3 side = p - right * 32;
				const double ground = m_world.sampleHeight(static_cast<float>(side.x), static_cast<float>(side.z));
				if (p.y - ground > 4 && p.y - ground < 9)
				{
					m_camera.setWalkingState(Vec3{ side.x, ground, side.z }, static_cast<float>(Atan2(tangent.x + right.x * .9, tangent.z + right.z * .9)), static_cast<float>(10_deg));
					found = true; break;
				}
			}
		}
		else if (name == U"expressway_information_board")
		{
			if (const auto* sign = guideOf([&](const GuideSignPlacement& s) { return s.bgColor.r < .1 && s.bgColor.g < .1 && s.bgColor.b < .1; })) { found = beforeSign(*sign, 60); }
		}
		else if (name == U"expressway_spacing_check")
		{
			if (const auto* sign = guideOf([&](const GuideSignPlacement& s) { return s.elements.size() == 2 && s.elements[1].text == U"確認基点"; })) { found = beforeSign(*sign, 45); }
		}
		else if (name == U"mountain_tunnel_portal" || name == U"mountain_tunnel_inside")
		{
			for (const auto& edge : m_network.edges())
			{
				if (edge.id < 0 || !edge.tunnel || expressway(edge) || edge.length < 80) { continue; }
				for (const auto& attachment : m_network.getNode(edge.nodeA)->attachments)
				{
					const auto* approach = m_network.getEdge(attachment.edgeId);
					if (!approach || approach->id == edge.id || approach->tunnel || approach->length < 50) { continue; }
					if (name == U"mountain_tunnel_inside") { found = place(edge.id, 40, leftLaneOffset(edge)); break; }
					// 坑口へ向かう向きで、取付道路の上に立つ。
					if (approach->nodeB == edge.nodeA) { found = place(approach->id, approach->length - 45, leftLaneOffset(*approach)); }
					else
					{
						const auto curve = m_network.getBezier(approach->id);
						const Vec3 tangent = -curve->tangentAt(45);
						Vec3 p = curve->positionAt(45) + tangentToRight(tangent) * -leftLaneOffset(*approach);
						p.y = (approach->usesDesignHeight() ? curve->positionAt(45).y : m_world.sampleHeight(static_cast<float>(p.x), static_cast<float>(p.z))) + kRoadSurfaceLift;
						m_camera.setWalkingState(p, static_cast<float>(Atan2(tangent.x, tangent.z))); found = true;
					}
					break;
				}
				if (found) { break; }
			}
		}
		else if (name == U"mountain_bridge")
		{
			for (const auto& edge : m_network.edges())
			{
				if (edge.id < 0 || expressway(edge) || !edge.useElevation || edge.tunnel || edge.lanes.size() != 2 || edge.length < 50) { continue; }
				const auto curve = m_network.getBezier(edge.id);
				const Vec3 mid = curve->positionAt(edge.length * .5f);
				if (mid.y - m_world.sampleHeight(static_cast<float>(mid.x), static_cast<float>(mid.z)) > 6) { found = place(edge.id, 4, leftLaneOffset(edge)); break; }
			}
		}
		else if (name == U"mountain_road" || name == U"valley_road")
		{
			// 山道は片側が大きく落ちる斜面の道、谷の道は両岸が高い谷底の道を選ぶ。道路中心から左右 250m の地盤で測る。
			const bool valley = name == U"valley_road";
			double best = valley ? 12 : 20;
			for (const auto& edge : m_network.edges())
			{
				if (edge.id < 0 || expressway(edge) || edge.useElevation || edge.tunnel || edge.lanes.size() != 2 || edge.length < 70 || edge.farmAccess) { continue; }
				const auto curve = m_network.getBezier(edge.id);
				const Vec3 mid = curve->positionAt(edge.length * .5f), right = tangentToRight(curve->tangentAt(edge.length * .5f));
				const auto ground = [&](double side) { const Vec3 p = mid + right * side; return m_world.sampleHeight(static_cast<float>(p.x), static_cast<float>(p.z)) - mid.y; };
				const double left = ground(-250), rightRise = ground(250);
				const double score = valley ? Min(left, rightRise) : Max(Abs(left), Abs(rightRise));
				if (score > best) { best = score; found = place(edge.id, Max(0.0, edge.length * .5 - 60), leftLaneOffset(edge)); }
			}
		}
		else if (name == U"town_arterial")
		{
			const Vec3 focus = captureFocusPoint();
			double best = Math::Inf;
			for (const auto& edge : m_network.edges())
			{
				if (edge.id < 0 || expressway(edge) || edge.useElevation || edge.tunnel || edge.length < 140 || edge.farmAccess) { continue; }
				if (edge.roadType != RoadType::Arterial || edge.lanes.size() < 4) { continue; }
				const auto curve = m_network.getBezier(edge.id);
				const Vec3 mid = curve->positionAt(edge.length * .5f);
				const double distance = Vec2{ mid.x - focus.x, mid.z - focus.z }.length();
				if (distance < best) { best = distance; found = place(edge.id, 20, leftLaneOffset(edge)); }
			}
		}
		else if (name == U"gorge_road")
		{
			// 国道41号の飛騨川沿いのように、片側が川へ落ち、反対側が山へ立ち上がる道。
			double best = 0;
			for (const auto& edge : m_network.edges())
			{
				if (edge.id < 0 || expressway(edge) || edge.useElevation || edge.tunnel || edge.lanes.size() != 2 || edge.length < 70 || edge.farmAccess) { continue; }
				const auto curve = m_network.getBezier(edge.id);
				const Vec3 mid = curve->positionAt(edge.length * .5f), right = tangentToRight(curve->tangentAt(edge.length * .5f));
				const auto river = m_world.rivers().nearest(Vec2{ mid.x, mid.z });
				if (river.reach < 0 || river.distance - river.halfWidth > 90) { continue; }
				const Vec2 toRiver = (river.center - Vec2{ mid.x, mid.z });
				const double side = toRiver.dot(Vec2{ right.x, right.z }) > 0 ? -1 : 1;
				const Vec3 slope = mid + right * (side * 220);
				const double rise = m_world.sampleHeight(static_cast<float>(slope.x), static_cast<float>(slope.z)) - mid.y;
				if (rise > best) { best = rise; found = place(edge.id, Max(0.0, edge.length * .5 - 50), leftLaneOffset(edge)); }
			}
		}
		else if (name == U"river_meander")
		{
			// 平野で最も広い川を斜め上空 1.6km から見下ろし、蛇行を確かめる。
			const auto& rivers = m_world.rivers().reaches;
			double best = 0;
			for (const auto& reach : rivers)
			{
				const double width = reach.halfWidth;
				if (width > best && reach.start.y < 120 && reach.start.y > 2) { best = width; }
			}
			for (const auto& reach : rivers)
			{
				if (reach.halfWidth < best * .999 || reach.start.y >= 120 || reach.start.y <= 2) { continue; }
				const Vec3 flow = (reach.end - reach.start).normalized();
				m_camera.setCaptureState(reach.start, 1600.0f, static_cast<float>(Atan2(flow.x, flow.z)), static_cast<float>(35_deg));
				found = true; break;
			}
		}
		else if (name == U"full_map")
		{
			// 全画面地図。地形の陰影・川・道路が描かれるかを確かめる。
			m_minimapRenderer.openFullScreen(m_camera, m_network, m_trainNetwork, m_districts);
			found = true;
		}
		else if (name == U"full_map_region")
		{
			// 地図全体。湾・平野・山地の配置を見る。
			if (!m_minimapRenderer.fullScreen()) { m_minimapRenderer.openFullScreen(m_camera, m_network, m_trainNetwork, m_districts); }
			auto& view = m_minimapRenderer.mapView();
			view.zoom = 1; view.center = Vec2{ WORLD_SIZE * .5, WORLD_SIZE * .5 };
			found = true;
		}
		DBG_LOG(U"[PhotoReview] view={} name={} found={} eye={}"_fmt(m_captureIndex, name, found, m_camera.eyePosition()));
		m_captureCameraDirty = false; m_captureFrame = 0; m_photoSettledFrames = 0; m_photoSavedAt = found ? -1 : 0;
	}
	m_world.update(m_camera.focusPoint());
	renderWorld();
	++m_captureFrame;
	const bool map = m_minimapRenderer.fullScreen();
	const bool settled = map ? m_minimapRenderer.fullTerrainReady() : m_worldRenderer.pendingTerrainJobs() == 0 && m_roadRenderer.pendingDetailJobs() == 0;
	m_photoSettledFrames = settled ? m_photoSettledFrames + 1 : 0;
	if (m_photoSavedAt < 0 && m_captureFrame >= 16 && (m_photoSettledFrames >= 6 || m_captureFrame >= (map ? kMaximumFrames * 8 : kMaximumFrames)))
	{
		ScreenCapture::SaveCurrentFrame(U"{}/photo_{:02}_{}.png"_fmt(getData().captureFolder, m_captureIndex, kNames[m_captureIndex]));
		DBG_LOG(U"[PhotoReview] saved view={} frame={} settled={} map={}"_fmt(m_captureIndex, m_captureFrame, m_photoSettledFrames, map));
		m_photoSavedAt = m_captureFrame;
	}
	if (m_photoSavedAt >= 0 && m_captureFrame > m_photoSavedAt + 3)
	{
		if (++m_captureIndex == kViews) { System::Exit(); return; }
		m_captureCameraDirty = true;
	}
}
