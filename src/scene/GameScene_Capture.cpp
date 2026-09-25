#include "GameScene.hpp"
#include "../world/ZoneGrid.hpp"
#include "../road/JunctionGeometry.hpp"

using ZoneGrid::worldToZoneCell;
using ZoneGrid::cellCenterXZ;

/// @file
/// @brief 再現確認用の視点選択・撮影・走行シナリオ。通常プレイの更新から検証手順を分離する。


Vec3 GameScene::captureFocusPoint() const
{
	constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
	const int cellRadius = static_cast<int>(Ceil(360.0f / cellSize));
	Vec2 bestCenter{ WORLD_SIZE * 0.5f, WORLD_SIZE * 0.5f };
	double bestScore = -Math::Inf;

	for (const auto& settlement : m_districts)
	{
		int buildingCount = 0;
		int commercialCount = 0;
		int arterialNodeCount = 0;
		Point centerChunk;
		int centerCol = 0;
		int centerRow = 0;
		worldToZoneCell(static_cast<float>(settlement.center.x), static_cast<float>(settlement.center.y), centerChunk, centerCol, centerRow);

		for (int dz = -cellRadius; dz <= cellRadius; ++dz)
		{
			for (int dx = -cellRadius; dx <= cellRadius; ++dx)
			{
				int col = centerCol + dx;
				int row = centerRow + dz;
				Point chunkCoord = centerChunk;
				while (col < 0) { col += ZONE_CELLS; --chunkCoord.x; }
				while (col >= ZONE_CELLS) { col -= ZONE_CELLS; ++chunkCoord.x; }
				while (row < 0) { row += ZONE_CELLS; --chunkCoord.y; }
				while (row >= ZONE_CELLS) { row -= ZONE_CELLS; ++chunkCoord.y; }
				const Chunk* chunk = m_world.getChunk(chunkCoord);
				if (!chunk) continue;
				const Building& building = chunk->buildingGrid[{ col, row }];
				if (building.type == BuildingType::None || building.type == BuildingType::Farmland) continue;
				++buildingCount;
				if (building.type == BuildingType::Shop || building.type == BuildingType::Office || building.type == BuildingType::PublicFacility)
				{
					++commercialCount;
				}
			}
		}

		const double radiusSq = 720.0 * 720.0;
		for (const auto& node : m_network.nodes())
		{
			if (node.id < 0) continue;
			const double dx = node.position.x - settlement.center.x;
			const double dz = node.position.z - settlement.center.y;
			if (dx * dx + dz * dz > radiusSq) continue;
			for (const EdgeAttachment& attachment : node.attachments)
			{
				const RoadEdge* edge = m_network.getEdge(attachment.edgeId);
				if (edge && edge->roadType == RoadType::Arterial)
				{
					++arterialNodeCount;
					break;
				}
			}
		}

		const double kindBonus = (settlement.kind == MapGenerator::SettlementKind::RegionalCity) ? 50000.0
			: (settlement.kind == MapGenerator::SettlementKind::LocalTown) ? 28000.0 : 0.0;
		const double score = buildingCount * 1200.0 + commercialCount * 2200.0 + arterialNodeCount * 800.0 + kindBonus;
		if (score > bestScore)
		{
			bestScore = score;
			bestCenter = settlement.center;
		}
	}

	const float y = m_world.computeHeight(static_cast<float>(bestCenter.x), static_cast<float>(bestCenter.y));
	return Vec3{ bestCenter.x, y, bestCenter.y };
}

Vec3 GameScene::captureRuralFringePoint(Vec3 fallback) const
{
	Vec3 best = fallback;
	double bestScore = Math::Inf;
	for (int chunkY = 0; chunkY < WORLD_CHUNKS; ++chunkY)
	{
		for (int chunkX = 0; chunkX < WORLD_CHUNKS; ++chunkX)
		{
			const Chunk* chunk = m_world.getChunk(Point{ chunkX, chunkY });
			if (!chunk) continue;
			for (int row = 1; row < ZONE_CELLS - 1; ++row)
			{
				for (int col = 1; col < ZONE_CELLS - 1; ++col)
				{
					if (chunk->zoneMap[{ col, row }] != ZoneType::Agriculture) continue;

					bool touchesUrban = false;
					for (int dz = -1; dz <= 1 && !touchesUrban; ++dz)
					{
						for (int dx = -1; dx <= 1; ++dx)
						{
							const ZoneType neighbor = chunk->zoneMap[{ col + dx, row + dz }];
							if (neighbor == ZoneType::Commercial || neighbor == ZoneType::Residential
								|| neighbor == ZoneType::LowResidential || neighbor == ZoneType::Industrial)
							{
								touchesUrban = true;
								break;
							}
						}
					}
					if (!touchesUrban) continue;

					const Vec2 center = cellCenterXZ(Point{ chunkX, chunkY }, col, row);
					const double dx = center.x - fallback.x;
					const double dz = center.y - fallback.z;
					const double score = dx * dx + dz * dz;
					if (score < bestScore)
					{
						bestScore = score;
						best = Vec3{ center.x, m_world.computeHeight(static_cast<float>(center.x), static_cast<float>(center.y)), center.y };
					}
				}
			}
		}
	}
	return best;
}

Vec3 GameScene::captureStreetCornerPoint(Vec3 fallback) const
{
	constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
	const float radius = 120.0f;
	const float radiusSq = radius * radius;
	const int cellRadius = static_cast<int>(Ceil(radius / cellSize));
	int bestNode = -1;
	double bestScore = -Math::Inf;
	for (const auto& node : m_network.nodes())
	{
		if (node.id < 0 || node.attachments.size() < 3) continue;
		const double fallbackDx = node.position.x - fallback.x;
		const double fallbackDz = node.position.z - fallback.z;
		const double fallbackDistSq = fallbackDx * fallbackDx + fallbackDz * fallbackDz;
		if (fallbackDistSq > 2400.0 * 2400.0) continue;

		int buildingCount = 0;
		int commercialCount = 0;
		int parkingCount = 0;
		int arterialCount = 0;
		for (const EdgeAttachment& attachment : node.attachments)
		{
			const RoadEdge* edge = m_network.getEdge(attachment.edgeId);
			if (edge && edge->roadType == RoadType::Arterial) ++arterialCount;
		}
		for (int dz = -cellRadius; dz <= cellRadius; ++dz)
		{
			for (int dx = -cellRadius; dx <= cellRadius; ++dx)
			{
				const float wx = static_cast<float>(node.position.x) + dx * cellSize;
				const float wz = static_cast<float>(node.position.z) + dz * cellSize;
				Point cc;
				int col = 0, row = 0;
				worldToZoneCell(wx, wz, cc, col, row);
				const Chunk* chunk = m_world.getChunk(cc);
				if (!chunk) continue;
				const Building& building = chunk->buildingGrid[{ col, row }];
				if (building.type == BuildingType::None || building.type == BuildingType::Farmland) continue;
				const Vec2 center = cellCenterXZ(cc, col, row) + Vec2{ building.offsetX, building.offsetZ };
				const float bx = static_cast<float>(center.x - node.position.x);
				const float bz = static_cast<float>(center.y - node.position.z);
				if (bx * bx + bz * bz > radiusSq) continue;
				++buildingCount;
				if (building.type == BuildingType::Shop || building.type == BuildingType::Office
					|| building.type == BuildingType::PublicFacility) ++commercialCount;
				if (building.type == BuildingType::Parking) ++parkingCount;
			}
		}

		const double degreeBonus = Min<double>(static_cast<double>(node.attachments.size()), 4.0) * 9000.0;
		const double score = (buildingCount - commercialCount - parkingCount) * 14500.0 + commercialCount * 1200.0 + parkingCount * 800.0
			+ arterialCount * 3000.0 + degreeBonus - fallbackDistSq * 0.035;
		if (score > bestScore)
		{
			bestScore = score;
			bestNode = node.id;
		}
	}

	if (const RoadNode* node = m_network.getNode(bestNode))
	{
		const float y = m_world.computeHeight(
			static_cast<float>(node->position.x), static_cast<float>(node->position.z));
		return Vec3{ node->position.x, y, node->position.z };
	}
	return fallback;
}

Vec3 GameScene::captureIntersectionPoint(Vec3 fallback, int variant) const
{
	if (getData().captureNode >= 0)
	{
		if (const auto* node = m_network.getNode(getData().captureNode)) { return node->position; }
	}
	if (getData().captureRoadRenders)
	{
		Array<std::pair<double, int>> candidates;
		for (const auto& node : m_network.nodes())
		{
			if (node.id < 0 || node.attachments.size() < 3) { continue; }
			int arterial = 0;
			for (const auto& attachment : node.attachments)
			{
				const auto* edge = m_network.getEdge(attachment.edgeId);
				if (edge && edge->length > 100.0f)
				{
					for (const auto& part : edge->parts)
					{
						if (part.type == RoadPartType::Sidewalk && part.build == BuildState::Built) { ++arterial; break; }
					}
				}
			}
			if (arterial < 2) { continue; }
			const double score = node.position.distanceFromSq(fallback) + (node.attachments.size() == 3 ? 0.0 : 1e10);
			candidates << std::pair<double, int>{ score, node.id };
		}
		candidates.sort_by([](const auto& a, const auto& b) { return a.first < b.first; });
		if (!candidates.isEmpty())
		{
			const auto* node = m_network.getNode(candidates[Min(static_cast<size_t>(variant), candidates.size()-1)].second);
			const double y = m_world.computeHeight(static_cast<float>(node->position.x), static_cast<float>(node->position.z));
			if (variant == m_captureIndex / 2)
			{
				DebugLog::print(U"[RoadCapture] view={} node={} attachments={} position=({:.2f},{:.2f},{:.2f})"_fmt(m_captureIndex, node->id, node->attachments.size(), node->position.x, y, node->position.z));
			}
			return Vec3{ node->position.x, y, node->position.z };
		}
	}

	constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
	const float radius = 130.0f;
	const float radiusSq = radius * radius;
	const int cellRadius = static_cast<int>(Ceil(radius / cellSize));
	struct CaptureIntersectionCandidate
	{
		int nodeId = -1;
		double score = 0.0;
	};
	Array<CaptureIntersectionCandidate> candidates;
	for (const auto& node : m_network.nodes())
	{
		if (node.id < 0 || node.attachments.size() < 3) continue;
		const double fallbackDx = node.position.x - fallback.x;
		const double fallbackDz = node.position.z - fallback.z;
		const double fallbackDistSq = fallbackDx * fallbackDx + fallbackDz * fallbackDz;
		if (fallbackDistSq > 2600.0 * 2600.0) continue;

		int arterialCount = 0;
		int localCount = 0;
		for (const EdgeAttachment& attachment : node.attachments)
		{
			const RoadEdge* edge = m_network.getEdge(attachment.edgeId);
			if (!edge) continue;
			if (edge->roadType == RoadType::Arterial) ++arterialCount;
			else if (edge->roadType == RoadType::LocalRoad) ++localCount;
		}

		int buildingCount = 0;
		int commercialCount = 0;
		for (int dz = -cellRadius; dz <= cellRadius; ++dz)
		{
			for (int dx = -cellRadius; dx <= cellRadius; ++dx)
			{
				const float wx = static_cast<float>(node.position.x) + dx * cellSize;
				const float wz = static_cast<float>(node.position.z) + dz * cellSize;
				Point cc;
				int col = 0, row = 0;
				worldToZoneCell(wx, wz, cc, col, row);
				const Chunk* chunk = m_world.getChunk(cc);
				if (!chunk) continue;
				const Building& building = chunk->buildingGrid[{ col, row }];
				if (building.type == BuildingType::None || building.type == BuildingType::Farmland) continue;
				const Vec2 center = cellCenterXZ(cc, col, row) + Vec2{ building.offsetX, building.offsetZ };
				const float bx = static_cast<float>(center.x - node.position.x);
				const float bz = static_cast<float>(center.y - node.position.z);
				if (bx * bx + bz * bz > radiusSq) continue;
				++buildingCount;
				if (building.type == BuildingType::Shop || building.type == BuildingType::Office
					|| building.type == BuildingType::PublicFacility) ++commercialCount;
			}
		}

		if (arterialCount <= 0) continue;
		const double hierarchyBonus = (arterialCount > 0 && localCount > 0) ? 140000.0 : (arterialCount > 1 ? 90000.0 : 0.0);
		const double degreeBonus = Min<double>(static_cast<double>(node.attachments.size()), 4.0) * 12000.0;
		const double score = hierarchyBonus + arterialCount * 26000.0 + localCount * 9000.0
			+ buildingCount * 6200.0 + commercialCount * 4200.0 + degreeBonus - fallbackDistSq * 0.020;
		candidates << CaptureIntersectionCandidate{ node.id, score };
	}
	candidates.sort_by([](const CaptureIntersectionCandidate& a, const CaptureIntersectionCandidate& b)
	{
		return a.score > b.score;
	});

	Array<Vec2> accepted;
	int acceptedIndex = 0;
	for (const CaptureIntersectionCandidate& candidate : candidates)
	{
		const RoadNode* node = m_network.getNode(candidate.nodeId);
		if (!node) continue;
		const Vec2 pos{ node->position.x, node->position.z };
		bool tooClose = false;
		for (const Vec2& other : accepted)
		{
			if ((pos - other).lengthSq() < 150.0 * 150.0)
			{
				tooClose = true;
				break;
			}
		}
		if (tooClose) continue;
		accepted << pos;
		if (acceptedIndex == variant)
		{
			const float y = m_world.computeHeight(static_cast<float>(node->position.x), static_cast<float>(node->position.z));
			return Vec3{ node->position.x, y, node->position.z };
		}
		++acceptedIndex;
	}
	return captureStreetCornerPoint(fallback);
}

Vec3 GameScene::captureCoastalBufferPoint(Vec3 fallback) const
{
	Vec3 best = fallback;
	double bestScore = Math::Inf;
	for (int chunkY = 0; chunkY < WORLD_CHUNKS; ++chunkY)
	{
		for (int chunkX = 0; chunkX < WORLD_CHUNKS; ++chunkX)
		{
			const Chunk* chunk = m_world.getChunk(Point{ chunkX, chunkY });
			if (!chunk) continue;
			for (const LandPatch& patch : chunk->landPatches)
			{
				if (patch.type != LandPatchType::Beach && patch.type != LandPatchType::Seawall) continue;
				if (patch.polygon.isEmpty()) continue;
				Vec2 center{ 0.0, 0.0 };
				for (const Vec2& p : patch.polygon) center += p;
				center /= static_cast<double>(patch.polygon.size());
				const double dx = center.x - fallback.x;
				const double dz = center.y - fallback.z;
				const double distSq = dx * dx + dz * dz;
				const double typeBonus = (patch.type == LandPatchType::Seawall) ? -220000.0 : 0.0;
				const double score = distSq + typeBonus;
				if (score < bestScore)
				{
					bestScore = score;
					best = Vec3{ center.x, m_world.computeHeight(static_cast<float>(center.x), static_cast<float>(center.y)), center.y };
				}
			}
		}
	}
	return best;
}

String GameScene::captureFileName(int index) const
{
	switch (index)
	{
	case 0: return U"city_render_01_overall.png";
	case 1: return U"city_render_02_city.png";
	case 2: return U"city_render_03_close.png";
	case 3: return U"city_render_04_rural_fringe.png";
	case 4: return U"city_render_05_intersection_overview.png";
	case 5: return U"city_render_06_intersection_zoom_a.png";
	case 6: return U"city_render_07_intersection_zoom_b.png";
	case 7: return U"city_render_08_intersection_zoom_c.png";
	case 8: return U"city_render_09_intersection_zoom_d.png";
	case 9: return U"city_render_10_intersection_topdown_a.png";
	case 10: return U"city_render_11_intersection_topdown_b.png";
	case 11: return U"city_render_12_intersection_zoom_e.png";
	case 12: return U"city_render_13_intersection_zoom_f.png";
	case 13: return U"city_render_14_intersection_marking_low_a.png";
	case 14: return U"city_render_15_intersection_marking_low_b.png";
	case 15: return U"city_render_16_intersection_marking_low_c.png";
	case 16: return U"city_render_17_intersection_topdown_c.png";
	case 17: return U"city_render_18_intersection_topdown_d.png";
	case 18: return U"city_render_19_intersection_topdown_e.png";
	case 19: return U"city_render_20_street_corner.png";
	case 20: return U"city_render_21_coastal_buffer.png";
	case 21: return U"city_render_22_rural_landuse.png";
	case 22: return U"city_render_23_traffic.png";
	case 23: return U"city_render_24_traffic_close.png";
	case 24: return U"city_render_25_satoyama.png";
	case 25: return U"city_render_26_provincial_town.png";
	case 26: return U"city_render_27_castle_transition.png";
	case 27: return U"city_render_28_fringe_streets.png";
	case 28: return U"city_render_29_field_access.png";
	case 29: return U"city_render_30_farm_track_walk.png";
	case 30: return U"city_render_31_urban_convenience.png";
	case 31: return U"city_render_32_roadside_convenience.png";
	case 32: return U"city_render_33_urban_fuel.png";
	case 33: return U"city_render_34_roadside_fuel.png";
	case 34: return U"city_render_35_rural_house.png";
	case 35: return U"city_render_36_mountain_river.png";
	case 36: return U"city_render_37_mountain_village.png";
	case 37: return U"city_render_38_levee_road.png";
	default: return U"city_render_done.png";
	}
}

void GameScene::updateStreamingBenchmark()
{
	m_clock.speed = TimeSpeed::Paused;
	m_clock.hour = 13.0f;
	m_camera.setBlockInput(true);
	if (!m_benchmarkOrigin) { m_benchmarkOrigin = captureFocusPoint(); }
	const Vec3 origin = *m_benchmarkOrigin;
	const int flightFrame = Max(0, m_captureFrame - 120);
	Vec3 focus = origin + Vec3{flightFrame * 5.0, 0, Sin(flightFrame * 0.005) * 500.0};
	focus.y = m_world.computeHeight(static_cast<float>(focus.x), static_cast<float>(focus.z));
	m_camera.setCaptureState(focus, 650.0f, static_cast<float>(-45.0_deg), static_cast<float>(48.0_deg));
	const Stopwatch timer{StartImmediately::Yes};
	m_world.update(focus);
	renderWorld();
	if (Scene::FrameCount() % 120 == 0)
	{
		DBG_LOG(U"[StreamingProgress] frame={} pending={} signalsMs={:.3f}"_fmt(
			m_captureFrame, m_worldRenderer.pendingTerrainJobs(), m_renderTimings.signals));
	}
	if (m_captureFrame == 60 && m_worldRenderer.pendingTerrainJobs() > 0) { return; }
	if (m_captureFrame >= 120)
	{
		const double cpuMs = timer.msF();
		m_captureCpuTimes << cpuMs;
		m_captureFrameTimes << Scene::DeltaTime() * 1000.0;
		if (cpuMs > 40.0)
		{
			DebugLog::print(U"[FlightSpike] frame={} cpuMs={:.2f} terrainMs={:.2f} roadsMs={:.2f} shadowMs={:.2f} hudMs={:.2f}"_fmt(
				flightFrame, cpuMs, m_renderTimings.terrainOnly, m_renderTimings.roadMesh, m_cityLighting.shadowMilliseconds(),m_renderTimings.uiRenderer));
		}
	}
	if (++m_captureFrame >= 840)
	{
		TextWriter csv{U"streaming_frames.csv"};
		csv.writeln(U"frame,cpuMs,frameMs");
		for (size_t i = 0; i < m_captureCpuTimes.size(); ++i)
		{
			csv.writeln(U"{},{:.3f},{:.3f}"_fmt(i,m_captureCpuTimes[i],m_captureFrameTimes[i]));
		}
		DebugLog::print(U"[StreamingBenchmark] complete sync={} frames={}"_fmt(getData().syncTerrain,m_captureCpuTimes.size()));
		System::Exit();
	}
}

void GameScene::updateCaptureCityRenders()
{
	const int kCaptureCount = getData().captureRoadRenders ? 6 : 38;
	const int warmupFrames = m_captureIndex == 22 ? 240 : 80;

	m_clock.speed = TimeSpeed::Paused;
	m_clock.hour = 13.0f;
	m_camera.setBlockInput(true);

	if (m_captureCameraDirty)
	{
		if (getData().captureRoadRenders && m_captureIndex == 0)
		{
			int checked = 0, repaired = 0, missing = 0;
			for (const auto& node : m_network.nodes())
			{
				if (node.id < 0 || node.attachments.size() < 2) { continue; }
				bool needsCap = false;
				int builtApproaches = 0;
				for (const auto& attachment : node.attachments)
				{
					const auto* edge = m_network.getEdge(attachment.edgeId);
					if (!edge || !edge->isRoadbedBuilt() || (edge->edgeState != EdgeState::Open && edge->edgeState != EdgeState::Existing)) { continue; }
					++builtApproaches;
					needsCap = needsCap || ((edge->nodeA == node.id ? edge->cutoffA : edge->cutoffB) > 0.15f);
				}
				if (builtApproaches < 2 || !needsCap) { continue; }
				++checked;
				const auto layout = JunctionGeometry::build(m_network,node.id);
				repaired += layout.repaired;
				if (layout.asphalt.indices.isEmpty()) { ++missing; DebugLog::print(U"[JunctionAudit] missing node={}"_fmt(node.id)); }
			}
			DebugLog::print(U"[JunctionAudit] checked={} repaired={} missing={}"_fmt(checked,repaired,missing));
		}
		const Vec3 focus = captureFocusPoint();
		const Vec3 streetFocus = captureStreetCornerPoint(focus);
		const Vec3 intersectionA = captureIntersectionPoint(focus, 0);
		const Vec3 intersectionB = captureIntersectionPoint(focus, 1);
		const Vec3 intersectionC = captureIntersectionPoint(focus, 2);
		const Vec3 intersectionD = captureIntersectionPoint(focus, 3);
		const Vec3 intersectionE = captureIntersectionPoint(focus, 4);
		const Vec3 intersectionF = captureIntersectionPoint(focus, 5);
		const Vec3 fringeFocus = captureRuralFringePoint(focus);
		const Vec3 coastalFocus = captureCoastalBufferPoint(focus);
		if (getData().captureRoadRenders)
		{
			const Vec3 roadFocus = captureIntersectionPoint(focus, m_captureIndex / 2);
			const bool overhead = (m_captureIndex % 2) != 0;
			m_camera.setCaptureState(roadFocus, overhead ? 165.0f : 125.0f,
				static_cast<float>(-40.0_deg), overhead ? static_cast<float>(86.0_deg) : static_cast<float>(43.0_deg));
		}
		else
		switch (m_captureIndex)
		{
		case 0:
			m_camera.setCaptureState(focus, 1800.0f, static_cast<float>(-39.0_deg), static_cast<float>(58.0_deg));
			break;
		case 1:
			m_camera.setCaptureState(focus + Vec3{ 120.0, 0.0, -60.0 }, 820.0f, static_cast<float>(-45.0_deg), static_cast<float>(43.0_deg));
			break;
		case 2:
			m_camera.setCaptureState(streetFocus + Vec3{ 34.0, 0.0, -24.0 }, 230.0f, static_cast<float>(-50.0_deg), static_cast<float>(30.0_deg));
			break;
		case 3:
			m_camera.setCaptureState(fringeFocus + Vec3{ 70.0, 0.0, -58.0 }, 680.0f, static_cast<float>(-45.0_deg), static_cast<float>(48.0_deg));
			break;
		case 4:
			m_camera.setCaptureState(intersectionA + Vec3{ 24.0, 0.0, -18.0 }, 260.0f, static_cast<float>(-45.0_deg), static_cast<float>(48.0_deg));
			break;
		case 5:
			m_camera.setCaptureState(intersectionA + Vec3{ 18.0, 0.0, -14.0 }, 150.0f, static_cast<float>(-44.0_deg), static_cast<float>(42.0_deg));
			break;
		case 6:
			m_camera.setCaptureState(intersectionB + Vec3{ -28.0, 0.0, 22.0 }, 170.0f, static_cast<float>(-32.0_deg), static_cast<float>(44.0_deg));
			break;
		case 7:
			m_camera.setCaptureState(intersectionC + Vec3{ 28.0, 0.0, 18.0 }, 180.0f, static_cast<float>(-58.0_deg), static_cast<float>(42.0_deg));
			break;
		case 8:
			m_camera.setCaptureState(intersectionD + Vec3{ -18.0, 0.0, -26.0 }, 135.0f, static_cast<float>(34.0_deg), static_cast<float>(40.0_deg));
			break;
		case 9:
			m_camera.setCaptureState(intersectionA, 210.0f, static_cast<float>(0.0_deg), static_cast<float>(86.0_deg));
			break;
		case 10:
			m_camera.setCaptureState(intersectionB, 230.0f, static_cast<float>(90.0_deg), static_cast<float>(84.0_deg));
			break;
		case 11:
			m_camera.setCaptureState(intersectionE + Vec3{ 20.0, 0.0, 24.0 }, 140.0f, static_cast<float>(68.0_deg), static_cast<float>(43.0_deg));
			break;
		case 12:
			m_camera.setCaptureState(intersectionF + Vec3{ -22.0, 0.0, -18.0 }, 125.0f, static_cast<float>(-118.0_deg), static_cast<float>(41.0_deg));
			break;
		case 13:
			m_camera.setCaptureState(intersectionA + Vec3{ 9.0, 0.0, -7.0 }, 78.0f, static_cast<float>(-42.0_deg), static_cast<float>(31.0_deg));
			break;
		case 14:
			m_camera.setCaptureState(intersectionB + Vec3{ -9.0, 0.0, 9.0 }, 82.0f, static_cast<float>(132.0_deg), static_cast<float>(32.0_deg));
			break;
		case 15:
			m_camera.setCaptureState(intersectionC + Vec3{ 11.0, 0.0, 8.0 }, 86.0f, static_cast<float>(38.0_deg), static_cast<float>(33.0_deg));
			break;
		case 16:
			m_camera.setCaptureState(intersectionC, 190.0f, static_cast<float>(180.0_deg), static_cast<float>(86.0_deg));
			break;
		case 17:
			m_camera.setCaptureState(intersectionD, 185.0f, static_cast<float>(270.0_deg), static_cast<float>(86.0_deg));
			break;
		case 18:
			m_camera.setCaptureState(intersectionE, 170.0f, static_cast<float>(45.0_deg), static_cast<float>(86.0_deg));
			break;
		case 19:
			m_camera.setCaptureState(streetFocus + Vec3{ 26.0, 0.0, -20.0 }, 115.0f, static_cast<float>(27.0_deg), static_cast<float>(54.0_deg));
			break;
		case 20:
			m_camera.setCaptureState(coastalFocus + Vec3{ 62.0, 0.0, -42.0 }, 260.0f, static_cast<float>(38.0_deg), static_cast<float>(43.0_deg));
			break;
		case 22:
		{
			m_camera.setCaptureState(streetFocus, 300.0f, static_cast<float>(-50.0_deg), static_cast<float>(40.0_deg));
			Array<std::pair<double, int>> candidates;
			for (const auto& edge : m_network.edges())
			{
				if (!edge.isRoadbedBuilt() || edge.length < 25 || !m_simGraph->getEdge(edge.id)) { continue; }
				const auto bezier = m_network.getBezier(edge.id);
				if (!bezier) { continue; }
				const double distance = bezier->positionAt(bezier->totalLength * 0.5f).distanceFrom(streetFocus);
				if (distance < 400) { candidates.emplace_back(distance, edge.id); }
			}
			candidates.sort();
			Reseed(getData().seed);
			for (size_t index = 0; index < Min(size_t{ 200 }, candidates.size()); ++index)
			{
				const VehicleType type = index % 3 == 0 ? VehicleType::KeiCar : VehicleType::PassengerCar;
				m_vehicleManager.spawnOnEdge(candidates[index].second, *m_simGraph, type,
					candidates[(index + candidates.size() / 2) % candidates.size()].second);
			}
			DebugLog::print(U"[TrafficCapture] spawned={} candidates={}"_fmt(m_vehicleManager.vehicles().size(), candidates.size()));
			break;
		}
		case 23:
		{
			Vec3 trafficFocus = streetFocus;
			double nearest = Math::Inf;
			for (const Vehicle& vehicle : m_renderVehicles)
			{
				const double distance = vehicle.position.distanceFrom(streetFocus);
				if (vehicle.speed > 1.0f && distance < nearest) { trafficFocus = vehicle.position; nearest = distance; }
			}
			m_camera.setCaptureState(trafficFocus, 70.0f, static_cast<float>(-38.0_deg), static_cast<float>(45.0_deg));
			break;
		}
		case 24:
		{
			Vec3 woodland = fringeFocus;
			double best = Math::Inf;
			for (int z = 1; z < WORLD_CHUNKS; ++z) for (int x = 1; x < WORLD_CHUNKS; ++x)
			{
				const Chunk* chunk = m_world.getChunk(Point{x,z});
				if (!chunk) { continue; }
				for (int row = 8; row < ZONE_CELLS; row += 8) for (int col = 8; col < ZONE_CELLS; col += 8)
				{
					if (chunk->zoneMap[{col,row}] != ZoneType::Unzoned) { continue; }
					const Vec3 point{x*CHUNK_SIZE+col*16.0,chunk->heightMap[{col,row}],z*CHUNK_SIZE+row*16.0};
					if (point.y < 45 || point.y > 180) { continue; }
					const double score = point.distanceFromSq(fringeFocus);
					if (score < best) { woodland = point; best = score; }
				}
			}
			m_camera.setCaptureState(woodland,1000.0f,static_cast<float>(-55.0_deg),static_cast<float>(36.0_deg));
			DebugLog::print(U"[PhotoReferenceCapture] satoyama=({:.1f},{:.1f},{:.1f})"_fmt(woodland.x,woodland.y,woodland.z));
			break;
		}
		case 25:
		{
			Vec3 provincial = fringeFocus;
			for (const auto& settlement : m_districts)
			{
				if (settlement.kind != MapGenerator::SettlementKind::LocalTown) { continue; }
				provincial = Vec3{settlement.center.x,m_world.computeHeight(static_cast<float>(settlement.center.x),static_cast<float>(settlement.center.y)),settlement.center.y};
				break;
			}
			m_camera.setCaptureState(provincial,300.0f,static_cast<float>(-40.0_deg),static_cast<float>(35.0_deg));
			Array<int> types(static_cast<size_t>(BuildingType::Count),0);
			const int centerX=static_cast<int>(provincial.x/CHUNK_SIZE),centerZ=static_cast<int>(provincial.z/CHUNK_SIZE);
			for (int z=Max(0,centerZ-1);z<=Min(WORLD_CHUNKS-1,centerZ+1);++z)
			{
				for (int x=Max(0,centerX-1);x<=Min(WORLD_CHUNKS-1,centerX+1);++x)
				{
					const Chunk* chunk=m_world.getChunk({x,z});if (!chunk) { continue; }
					for (int row=0;row<ZONE_CELLS;++row) for (int col=0;col<ZONE_CELLS;++col)
					{
						const Vec2 point=cellCenterXZ({x,z},col,row);
						if (point.distanceFromSq(Vec2{provincial.x,provincial.z})>350.0*350.0) { continue; }
						++types[static_cast<size_t>(chunk->buildingGrid[{col,row}].type)];
					}
				}
			}
			DBG_LOG(U"[ProvincialBuildings] detached={} low={} mid={} high={} shop={} office={}"_fmt(
				types[static_cast<size_t>(BuildingType::Detached)],types[static_cast<size_t>(BuildingType::LowApartment)],
				types[static_cast<size_t>(BuildingType::MidApartment)],types[static_cast<size_t>(BuildingType::HighApartment)],
				types[static_cast<size_t>(BuildingType::Shop)],types[static_cast<size_t>(BuildingType::Office)]));
			break;
		}
		case 35:
		{
			Vec3 mountainRiver=fringeFocus;
			Vec2 mountainFlow{0,1};
			double best=-Math::Inf;
			const auto& reaches=m_world.rivers().reaches;
			HashTable<Point,Array<int>> touching;
			const auto riverKey=[](Vec3 point) { return Point{static_cast<int>(Round(point.x)),static_cast<int>(Round(point.z))}; };
			for (size_t index=0;index<reaches.size();++index)
			{
				touching[riverKey(reaches[index].start)] << static_cast<int>(index);
				touching[riverKey(reaches[index].end)] << static_cast<int>(index);
			}
			for (size_t index=0;index<reaches.size();++index)
			{
				const auto& reach=reaches[index];
				const Vec3 middle=reach.start;
				if (middle.x<2500 || middle.z<2500 || middle.x>WORLD_SIZE-2500 || middle.z>WORLD_SIZE-2500
					|| middle.y<100 || middle.y>850 || reach.halfWidth<4 || reach.halfWidth>24) { continue; }
				const Vec2 along{reach.end.x-reach.start.x,reach.end.z-reach.start.z};
				if (along.lengthSq()<4) { continue; }
				double bend=0;
				for (const int otherId:touching[riverKey(reach.start)])
				{
					if (otherId==static_cast<int>(index)) { continue; }
					const auto& other=reaches[otherId];
					if (other.end.distanceFromSq(reach.start)>.01) { continue; }
					const Vec2 incoming{other.end.x-other.start.x,other.end.z-other.start.z};
					if (incoming.lengthSq()<4) { continue; }
					bend=Max(bend,Abs(incoming.normalized().cross(along.normalized())));
				}
				const Vec2 across=Vec2{-along.y,along.x}.normalized();
				const Vec2 center{middle.x,middle.z};
				const double sideDistance=reach.halfWidth+170;
				const Vec2 left=center+across*sideDistance,right=center-across*sideDistance;
				const double leftRelief=m_world.computeHeight(static_cast<float>(left.x),static_cast<float>(left.y))-middle.y;
				const double rightRelief=m_world.computeHeight(static_cast<float>(right.x),static_cast<float>(right.y))-middle.y;
				if (Min(leftRelief,rightRelief)<18 || Max(leftRelief,rightRelief)>320) { continue; }
				const double score=Min(leftRelief,rightRelief)*.8+Min(150.0,Max(leftRelief,rightRelief))*.15
					+Min(500.0,middle.y)*.04+bend*95.0-reach.halfWidth*.4;
				if (score>best) { best=score;mountainRiver=middle;mountainFlow=along.normalized(); }
			}
			m_camera.setCaptureState(mountainRiver,350.0f,static_cast<float>(Atan2(-mountainFlow.x,-mountainFlow.y)+0.42),static_cast<float>(41.0_deg));
			DBG_LOG(U"[PhotoReferenceCapture] mountainRiver=({}, {}, {}) score={}"_fmt(mountainRiver.x,mountainRiver.y,mountainRiver.z,best));
			break;
		}
		case 36:
		{
			Vec3 mountainVillage=fringeFocus;
			Vec2 mountainDirection{0,1};
			double best=-Math::Inf;
			int candidates=0,withHouses=0;
			for (const auto& settlement:m_districts)
			{
				if (settlement.kind!=MapGenerator::SettlementKind::RuralSettlement
					|| settlement.center.x<2500 || settlement.center.y<2500
					|| settlement.center.x>WORLD_SIZE-2500 || settlement.center.y>WORLD_SIZE-2500) { continue; }
				++candidates;
				const double height=m_world.computeHeight(static_cast<float>(settlement.center.x),static_cast<float>(settlement.center.y));
				if (height<50 || height>900) { continue; }
				int buildings=0;
				Vec2 houseCenter{0,0};
				const int minX=Max(0,static_cast<int>((settlement.center.x-450)/16));
				const int maxX=Min(static_cast<int>(WORLD_SIZE/16)-1,static_cast<int>((settlement.center.x+450)/16));
				const int minZ=Max(0,static_cast<int>((settlement.center.y-450)/16));
				const int maxZ=Min(static_cast<int>(WORLD_SIZE/16)-1,static_cast<int>((settlement.center.y+450)/16));
				for (int z=minZ;z<=maxZ;++z) for (int x=minX;x<=maxX;++x)
				{
					const Chunk* chunk=m_world.getChunk({x/ZONE_CELLS,z/ZONE_CELLS});
					if (!chunk) { continue; }
					const BuildingType type=chunk->buildingGrid[{x%ZONE_CELLS,z%ZONE_CELLS}].type;
					if (type==BuildingType::Detached || type==BuildingType::RuralHouse || type==BuildingType::LowApartment || type==BuildingType::Shop)
					{
						++buildings;houseCenter+=Vec2{(x+.5)*16.0,(z+.5)*16.0};
					}
				}
				if (buildings<2) { continue; }
				++withHouses;
				houseCenter/=buildings;
				double relief=0;
				Vec2 uphill{0,1};
				for (const Vec2 offset:{Vec2{550,0},Vec2{-550,0},Vec2{0,550},Vec2{0,-550},
					Vec2{900,0},Vec2{-900,0},Vec2{0,900},Vec2{0,-900}})
				{
					const double ascent=m_world.computeHeight(static_cast<float>(settlement.center.x+offset.x),
						static_cast<float>(settlement.center.y+offset.y))-height;
					if (ascent>relief) { relief=ascent;uphill=offset.normalized(); }
				}
				const auto water=m_world.rivers().nearest(settlement.center);
				if (water.reach>=0 && water.halfWidth>35 && water.distance-water.halfWidth<400) { continue; }
				DBG_LOG(U"[MountainVillageCandidate] center=({}, {}) height={:.1f} houses={} relief={:.1f}"_fmt(
					settlement.center.x,settlement.center.y,height,buildings,relief));
				const double score=Min(30,buildings)*2.0+Min(220.0,relief)*1.4+height*.04
					+(settlement.plan.ruralForm==UrbanMorphology::RuralForm::Valley ? 30.0 : 0.0);
				if (score>best)
				{
					best=score;mountainVillage={houseCenter.x,m_world.computeHeight(static_cast<float>(houseCenter.x),static_cast<float>(houseCenter.y)),houseCenter.y};
					mountainDirection=uphill;
				}
			}
			const float yaw=static_cast<float>(Atan2(-mountainDirection.x,-mountainDirection.y));
			m_camera.setCaptureState(mountainVillage,360.0f,yaw,static_cast<float>(48.0_deg));
			DBG_LOG(U"[PhotoReferenceCapture] mountainVillage=({}, {}, {}) score={} candidates={} withHouses={}"_fmt(
				mountainVillage.x,mountainVillage.y,mountainVillage.z,best,candidates,withHouses));
			break;
		}
		case 37:
		{
			Vec3 leveeRoad=coastalFocus;
			double best=-Math::Inf;
			for (const auto& edge:m_network.edges())
			{
				if (!edge.leveeRoad) { continue; }
				const auto curve=m_network.getBezier(edge.id);
				if (!curve || curve->totalLength<45) { continue; }
				const Vec3 middle=curve->positionAt(curve->totalLength*.5f);
				const auto river=m_world.rivers().nearest({middle.x,middle.z});
				if (river.reach<0 || river.halfWidth<40 || river.distance-river.halfWidth>115) { continue; }
				const double score=river.halfWidth+Min(150.0,static_cast<double>(curve->totalLength))*.3;
				if (score>best) { best=score;leveeRoad=middle; }
			}
			m_camera.setCaptureState(leveeRoad,280.0f,static_cast<float>(-48.0_deg),static_cast<float>(55.0_deg));
			DBG_LOG(U"[PhotoReferenceCapture] leveeRoad=({}, {}, {}) score={}"_fmt(leveeRoad.x,leveeRoad.y,leveeRoad.z,best));
			break;
		}
		case 30: case 31: case 32: case 33: case 34:
			prepareRoadsideCapture(m_captureIndex-30);
			break;
		case 26: case 27: case 28: case 29:
			prepareFringeCapture(m_captureIndex-26);
			break;
		case 21:
			m_camera.setCaptureState(fringeFocus + Vec3{ -80.0, 0.0, 70.0 }, 520.0f, static_cast<float>(128.0_deg), static_cast<float>(50.0_deg));
			break;
		}
		m_captureCameraDirty = false;
		m_captureFrameTimes.clear();
		m_captureCpuTimes.clear();
		m_captureGpuTimes.clear();
		m_captureFrame = 0;
	}

	m_world.update(m_camera.focusPoint());
	if (!getData().captureRoadRenders && m_simGraph)
	{
		for (auto& response : m_simThread.drainResponses())
		{
			if (const auto* route = std::get_if<RouteResponse>(&response)) { m_vehicleManager.applyRouteResponse(*route); }
		}
		constexpr double kCaptureTimeStep = 1.0 / 60.0;
		m_clock.now += kCaptureTimeStep;
		m_vehicleManager.setTrafficDemand(calculateTrafficDemand(m_economy.population, m_citySnapshot, m_clock.hour));
		m_vehicleManager.setTrafficFocus(m_camera.focusPoint());
		m_trainManager.update(kCaptureTimeStep,m_clock.now);
		m_vehicleManager.update(kCaptureTimeStep, m_clock.now, *m_simGraph, m_network, m_roadRenderer.visibleEdges());
		for (auto& request : m_vehicleManager.collectRequests()) { m_simThread.pushRequest(std::move(request)); }
	}
	renderWorld();
	if (m_captureIndex == 22 && m_captureFrame == 20)
	{
		for (const Vehicle& vehicle : m_renderVehicles) { m_captureVehicleStartPositions[vehicle.id] = vehicle.position; }
	}

	if (m_captureFrame >= warmupFrames - 60 && m_captureFrame < warmupFrames)
	{
		m_captureFrameTimes << Scene::DeltaTime() * 1000.0;
		m_captureCpuTimes << m_renderTimings.total;
		if (m_gpuTimer.milliseconds() >= 0.0)
		{
			m_captureGpuTimes << m_gpuTimer.milliseconds();
		}
	}
	// Only sample settled geometry; async terrain must finish before the final 60 frames.
	if (m_captureFrame == warmupFrames - 61 && m_worldRenderer.pendingTerrainJobs() > 0) { return; }
	if (++m_captureFrame == warmupFrames)
	{
		const FilePath directory=U"Screenshot/"+getData().captureFolder;
		FileSystem::CreateDirectories(directory);
		const String fileName=getData().captureFolder+U"/"+captureFileName(m_captureIndex);
		ScreenCapture::SaveCurrentFrame(fileName);
		DebugLog::print(U"[CaptureLocation] view={} focus={} eye={}"_fmt(m_captureIndex,m_camera.focusPoint(),m_camera.eyePosition()));
		DebugLog::print(U"[CapturePerf] view={} frameMs={:.2f} cpuMs={:.2f} terrainMs={:.2f} roadsMs={:.2f} shadowMs={:.2f}"_fmt(
			m_captureIndex, Scene::DeltaTime() * 1000.0, m_renderTimings.total,
			m_renderTimings.terrainOnly, m_renderTimings.roadMesh, m_cityLighting.shadowMilliseconds()));
		auto percentile = [](Array<double> values, double fraction)
		{
			if (values.isEmpty()) { return -1.0; }
			values.sort();
			return values[Min(values.size() - 1, static_cast<size_t>(fraction * (values.size() - 1)))];
		};
		DebugLog::print(U"[CaptureDistribution] view={} samples={} frameP50={:.2f} frameP95={:.2f} cpuP50={:.2f} gpuForwardP50={:.2f} gpuForwardP95={:.2f}"_fmt(
			m_captureIndex, m_captureFrameTimes.size(), percentile(m_captureFrameTimes, 0.5),
			percentile(m_captureFrameTimes, 0.95), percentile(m_captureCpuTimes, 0.5),
			percentile(m_captureGpuTimes, 0.5), percentile(m_captureGpuTimes, 0.95)));
		DebugLog::print(U"[BuildingVisibility] submitted={} considered={}"_fmt(
			m_worldRenderer.buildingsSubmitted(), m_worldRenderer.buildingsConsidered()));
		if (!getData().captureRoadRenders && m_simGraph)
		{
			int moving = 0;
			double totalDistance = 0;
			for (const Vehicle& vehicle : m_renderVehicles)
			{
				const auto start = m_captureVehicleStartPositions.find(vehicle.id);
				if (start != m_captureVehicleStartPositions.end())
				{
					const double distance = vehicle.position.distanceFrom(start->second);
					if (distance > 1) { ++moving; totalDistance += distance; }
				}
			}
			DebugLog::print(U"[TrafficCapture] total={} active={} moved={} meanDistanceM={:.2f}"_fmt(
				m_vehicleManager.vehicles().size(), m_renderVehicles.size(), moving, totalDistance / Max(1, moving)));
		}
		DebugLog::print(U"[CaptureCity] saved Screenshot/{}"_fmt(fileName));
	}
	else if (m_captureFrame > warmupFrames + 8)
	{
		++m_captureIndex;
		if (m_captureIndex >= kCaptureCount)
		{
			{
				TextWriter writer{ U"Screenshot/"+getData().captureFolder+U"/validation_report.txt" };
				if (writer)
				{
					writer << U"passed=" + String{ m_cityConstraintValidationPassed ? U"true" : U"false" };
					writer << U"\n";
					writer << m_cityConstraintValidationSummary;
				}
			}
			if (!m_cityConstraintValidationPassed)
			{
				TextWriter writer{ U"Screenshot/"+getData().captureFolder+U"/constraint_validation_failed.txt" };
				if (writer) writer << U"City constraint validation failed: " + m_cityConstraintValidationSummary;
			}
			System::Exit();
			return;
		}
		m_captureCameraDirty = true;
		m_captureFrame = 0;
	}
}

void GameScene::updateTransportReview()
{
	m_clock.speed=TimeSpeed::Paused; m_clock.hour=13;
	if (m_captureCameraDirty)
	{
		const Vec3 focus=captureFocusPoint();
		if (m_captureIndex==0) { m_camera.setCaptureState(focus,850,static_cast<float>(-35_deg),static_cast<float>(68_deg)); }
		if (m_captureIndex==1) { m_camera.setCaptureState(captureStreetCornerPoint(focus),100,static_cast<float>(-40_deg),static_cast<float>(32_deg)); }
		if (m_captureIndex==2 && !m_trainNetwork.nodes().isEmpty()) { m_camera.setCaptureState(m_trainNetwork.nodes().front().position,220,static_cast<float>(-50_deg),static_cast<float>(32_deg)); }
		if (m_captureIndex==3)
		{
			for (const auto& edge : m_network.edges())
			{
				if (edge.id<0 || !edge.useElevation) { continue; }
				const Vec3 point=m_network.getBezier(edge.id)->evaluate(.5f);
				if (m_world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z))<-1)
				{
					m_camera.setCaptureState(point,200,static_cast<float>(-40_deg),static_cast<float>(42_deg)); break;
				}
			}
		}
		if (m_captureIndex==4) { m_minimapRenderer.openFullScreen(m_camera,m_network,m_trainNetwork,m_districts); }
		if (m_captureIndex==5)
		{
			auto& view=m_minimapRenderer.mapView(); view.center={focus.x,focus.z};
			view.zoomAt(view.body(Scene::Size()).center(),32,Scene::Size());
			view.showContext(view.body(Scene::Size()).center()+Vec2{80,60},Scene::Size());
		}
		if (m_captureIndex==6)
		{
			m_minimapRenderer.mapView().close();
			for (const auto& reach : m_world.rivers().reaches)
			{
				if (reach.start.y>2 && reach.start.y<45 && reach.halfWidth>30)
				{ m_camera.setCaptureState((reach.start+reach.end)*.5,280,static_cast<float>(-40_deg),static_cast<float>(35_deg)); break; }
			}
		}
		if (m_captureIndex==7)
		{
			for (const auto& edge : m_trainNetwork.edges())
			{
				const Vec3 p=m_trainNetwork.getBezier(edge.id)->evaluate(.5f);const double gap=p.y-m_world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z));
				if (gap>8 && gap<14) { m_camera.setCaptureState(p,18,static_cast<float>(-30_deg),static_cast<float>(-18_deg)); break; }
			}
		}
		if (m_captureIndex==8 || m_captureIndex==9)
		{
			const auto visit=[&](const CubicBezier& curve)
			{
				for (float arc=0;arc<curve.totalLength;arc+=4)
				{
					const Vec3 p=curve.positionAt(arc),tangent=curve.tangentAt(arc);const double depth=m_world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z))-p.y;
					if (depth>2 && depth<5) { m_camera.setCaptureState(p+Vec3{0,2.4,0},18,static_cast<float>(Atan2(-tangent.x,-tangent.z)),0);return true; }
				}
				return false;
			};
			if (m_captureIndex==8) { for (const auto& edge : m_network.edges()) { if (edge.id>=0 && edge.tunnel && visit(*m_network.getBezier(edge.id))) { break; } } }
			else { for (const auto& edge : m_trainNetwork.edges()) { if (edge.id>=0 && visit(*m_trainNetwork.getBezier(edge.id))) { break; } } }
		}
		if (m_captureIndex==10)
		{
			m_minimapRenderer.openFullScreen(m_camera,m_network,m_trainNetwork,m_districts);auto& view=m_minimapRenderer.mapView();view.zoom=2;
			const Vec2 before=view.center;view.panKeyboard({1,-1},.1,Scene::Size());
			DBG_LOG(U"[MapKeyboardReview] movedPixels={:.2f} rivers={} boundaries={}"_fmt(view.center.distanceFrom(before)*view.scale(Scene::Size()),view.rivers.size(),view.boundaries.size()));
		}
		if (m_captureIndex>=11)
		{
			m_minimapRenderer.mapView().close();
			if (m_captureIndex==13)
			{
				for (const auto& edge : m_trainNetwork.edges())
				{
					if (edge.id<0) { continue; }const auto curve=m_trainNetwork.getBezier(edge.id);const Vec3 p=curve->evaluate(.5f),tangent=curve->tangent(.5f);
					if (m_world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z))-p.y>10) { m_camera.setWalkingState(p+Vec3{0,.15,0},static_cast<float>(Atan2(tangent.x,tangent.z)));break; }
				}
			}
			else
			{
				for (const auto& edge : m_network.edges())
				{
					if (edge.id<0 || edge.tunnel || edge.length<45) { continue; }const auto curve=m_network.getBezier(edge.id);Vec3 p=curve->evaluate(.5f);const Vec3 tangent=curve->tangent(.5f),right=tangentToRight(tangent);
					const double ground=m_world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z));const Vec3 side=p+right*(edge.totalWidth()*.5+28);
					const double relief=Abs(ground-m_world.sampleHeight(static_cast<float>(side.x),static_cast<float>(side.z)));
					if ((m_captureIndex==11 && !edge.useElevation && relief>5) || (m_captureIndex==12 && edge.useElevation && ground<m_world.waterSurfaceHeight(p.x,p.z)))
					{
						p.y=(edge.usesDesignHeight() ? p.y : ground)+kRoadSurfaceLift;m_camera.setWalkingState(p,static_cast<float>(Atan2(tangent.x,tangent.z)));break;
					}
				}
			}
			DBG_LOG(U"[WalkingTransportReview] view={} eye={}"_fmt(m_captureIndex,m_camera.eyePosition()));
		}
		m_captureCameraDirty=false; m_captureFrame=0;
	}
	m_world.update(m_camera.focusPoint()); const Stopwatch frameTimer{StartImmediately::Yes}; renderWorld();
	if (m_captureFrame==70 && m_worldRenderer.pendingTerrainJobs()>0 && !m_minimapRenderer.fullScreen()) { return; }
	if (++m_captureFrame==75)
	{
		ScreenCapture::SaveCurrentFrame(U"transport_{:02}.png"_fmt(m_captureIndex));
		DBG_LOG(U"[TransportReview] rendered={} map={} cpuDrawMs={:.2f}"_fmt(m_captureIndex,m_minimapRenderer.fullScreen(),frameTimer.msF()));
	}
	if (m_captureFrame>82)
	{
		if (++m_captureIndex==14)
		{
			auto& view=m_minimapRenderer.mapView();
			view.showContext(view.body(Scene::Size()).center(),Scene::Size());
			const auto target=view.jumpFromMenu(view.menuBounds().center());
			if (target) { jumpToMapPosition(*target); }
			const Vec3 focus=m_camera.focusPoint();
			const bool passed=target && Vec2{focus.x,focus.z}.distanceFrom(*target)<.01 && !view.visible;
			DBG_LOG(U"[TransportReview] mapJumpPassed={} streets={} labels={}"_fmt(passed,view.streets.size(),view.labels.size()));
			System::Exit(); return;
		}
		m_captureCameraDirty=true;
	}
}

void GameScene::updateStreetReview()
{
	constexpr int kViewCount = 6;
	constexpr int kWarmupFrames = 140;
	constexpr int kSampleFrames = 60;
	m_clock.speed = TimeSpeed::Paused;
	m_clock.hour = 13;
	if (m_streetReviewEdges.isEmpty())
	{
		HashTable<int, std::array<int, 3>> counts;
		for (int z = 0; z < WORLD_CHUNKS; ++z)
		{
			for (int x = 0; x < WORLD_CHUNKS; ++x)
			{
				const auto* chunk = m_world.getChunk({x, z});
				if (!chunk) { continue; }
				for (const auto& building : chunk->buildingGrid)
				{
					if (building.edgeId < 0) { continue; }
					auto& count = counts[building.edgeId];
					if (building.type == BuildingType::Shop || building.type == BuildingType::Office) { ++count[0]; }
					else if (building.type == BuildingType::Detached || building.type == BuildingType::LowApartment) { ++count[1]; }
				}
			}
		}
		for (int category = 0; category < 3; ++category)
		{
			double bestScore = -1;
			Optional<int> best;
			for (const auto& edge : m_network.edges())
			{
				if (edge.id < 0 || edge.useElevation || edge.length < 85 || edge.length > 600 || edge.totalWidth() > 18) { continue; }
				const auto iterator = counts.find(edge.id);
				if (iterator == counts.end()) { continue; }
				const Vec3 point = m_network.getBezier(edge.id)->evaluate(.5f);
				double villageDistance = Math::Inf;
				for (const auto& settlement : m_districts)
				{
					if (settlement.kind == MapGenerator::SettlementKind::RuralSettlement)
					{ villageDistance = Min(villageDistance, Vec2{point.x, point.z}.distanceFrom(settlement.center)); }
				}
				const auto& count = iterator->second;
				double score = category == 0 ? count[0] * 4.0 - count[1] : count[1] * 3.0 - count[0];
				if (category == 1 && villageDistance < 1600) { continue; }
				if (category == 2) { score = villageDistance < 500 ? count[1] * 2.0 + edge.length * .02 - villageDistance * .01 : -1; }
				if (score > bestScore) { bestScore = score; best = edge.id; }
			}
			if (!best) { DBG_LOG(U"[StreetReview] no candidate category={}"_fmt(category)); System::Exit(); return; }
			m_streetReviewEdges << *best;
		}
	}
	if (m_captureCameraDirty)
	{
		const auto* edge = m_network.getEdge(m_streetReviewEdges[m_captureIndex % 3]);
		const auto curve = m_network.getBezier(edge->id);
		const float arc = curve->totalLength * (m_captureIndex < 3 ? .25f : .75f);
		Vec3 point = curve->positionAt(arc);
		Vec3 direction = curve->tangentAt(arc) * (m_captureIndex < 3 ? 1 : -1);
		point += tangentToRight(direction) * (edge->totalWidth() * .30);
		point.y = m_world.sampleHeight(static_cast<float>(point.x), static_cast<float>(point.z));
		m_camera.setWalkingState(point, static_cast<float>(Atan2(direction.x, direction.z)));
		DBG_LOG(U"[StreetReviewCamera] view={} edge={} ground={} eye={} yaw={}"_fmt(m_captureIndex, edge->id, point, m_camera.eyePosition(), Atan2(direction.x, direction.z)));
		m_captureCameraDirty = false;
		m_captureFrame = 0;
		m_captureFrameTimes.clear();
		m_captureCpuTimes.clear();
	}
	m_world.update(m_camera.focusPoint());
	renderWorld();
	if (m_captureFrame == kWarmupFrames - kSampleFrames - 1 && m_worldRenderer.pendingTerrainJobs() > 0) { return; }
	if (m_captureFrame >= kWarmupFrames - kSampleFrames && m_captureFrame < kWarmupFrames)
	{
		m_captureFrameTimes << Scene::DeltaTime() * 1000;
		m_captureCpuTimes << m_renderTimings.total;
	}
	if (++m_captureFrame == kWarmupFrames)
	{
		ScreenCapture::SaveCurrentFrame(U"street_{:02}.png"_fmt(m_captureIndex));
		m_captureFrameTimes.sort(); m_captureCpuTimes.sort();
		DBG_LOG(U"[StreetReview] view={} frames={} frameP50={:.2f} frameP95={:.2f} cpuP50={:.2f} buildings={}"_fmt(m_captureIndex, m_captureFrameTimes.size(), m_captureFrameTimes[30], m_captureFrameTimes[56], m_captureCpuTimes[30], m_worldRenderer.buildingsSubmitted()));
	}
	if (m_captureFrame > kWarmupFrames + 8)
	{
		if (++m_captureIndex == kViewCount) { System::Exit(); return; }
		m_captureCameraDirty = true;
	}
}
