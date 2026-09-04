
#include "ZoneManager.hpp"
#include <cmath>

// ===== 定数 =====

namespace
{
	constexpr float kCellSize       = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;  // 16.0f [m]
	constexpr float kSpawnThreshold = 0.70f;  ///< 建物生成に必要な最低スコア
	constexpr float kDecayThreshold = 0.10f;  ///< 衰退・撤去が起きるスコア上限
	constexpr float kRoadClearance  = 10.0f;  ///< 道路と建物の最低クリアランス [m]

	struct RoadInfo
	{
		float dist;
		float angle;
		int edgeId;
		float edgeT;
	};

	RoadInfo nearestRoadInfo(float centX, float centZ, const RoadNetwork& network)
	{
		float minDistSq = 1e12f;
		float bestAngle = 0.0f;
		int bestEdgeId = -1;
		float bestEdgeT = 0.0f;

		for (const auto& edge : network.edges())
		{
			if (edge.id < 0) continue;
			const RoadNode* na = network.getNode(edge.nodeA);
			const RoadNode* nb = network.getNode(edge.nodeB);
			if (!na || !nb) continue;

			const float p0x = static_cast<float>(na->position.x);
			const float p0z = static_cast<float>(na->position.z);
			const float p1x = static_cast<float>(edge.ctrlA.x);
			const float p1z = static_cast<float>(edge.ctrlA.z);
			const float p2x = static_cast<float>(edge.ctrlB.x);
			const float p2z = static_cast<float>(edge.ctrlB.z);
			const float p3x = static_cast<float>(nb->position.x);
			const float p3z = static_cast<float>(nb->position.z);

			constexpr int kSamples = 8;
			for (int i = 0; i <= kSamples; ++i)
			{
				const float t  = static_cast<float>(i) / kSamples;
				const float t1 = 1.0f - t;
				const float c0 = t1*t1*t1;
				const float c1 = 3.0f*t1*t1*t;
				const float c2 = 3.0f*t1*t*t;
				const float c3 = t*t*t;
				const float px = c0*p0x + c1*p1x + c2*p2x + c3*p3x;
				const float pz = c0*p0z + c1*p1z + c2*p2z + c3*p3z;
				const float ddx = px - centX;
				const float ddz = pz - centZ;
				const float dSq = ddx*ddx + ddz*ddz;
				if (dSq < minDistSq)
				{
					minDistSq = dSq;
					// Bezier tangent: dB/dt = 3[t1^2(P1-P0) + 2t1t(P2-P1) + t^2(P3-P2)]
					const float tanx = t1*t1*(p1x-p0x) + 2.0f*t1*t*(p2x-p1x) + t*t*(p3x-p2x);
					const float tanz = t1*t1*(p1z-p0z) + 2.0f*t1*t*(p2z-p1z) + t*t*(p3z-p2z);
					bestAngle = std::atan2(tanz, tanx);
					bestEdgeId = edge.id;
					bestEdgeT = t;
				}
			}
		}

		return { Math::Sqrt(minDistSq), bestAngle, bestEdgeId, bestEdgeT };
	}

	inline float scoreFromDist(float dist)
	{
		constexpr float kNearDist = 32.0f;
		constexpr float kFarDist  = 64.0f;
		if      (dist < kNearDist) return 1.0f;
		else if (dist < kFarDist)  return 1.0f - (dist - kNearDist) / (kFarDist - kNearDist);
		else                       return 0.0f;
	}
}

ZoneDevelopmentDemand calculateZoneDevelopmentDemand(int population, const CitySnapshot& snapshot)
{
	ZoneDevelopmentDemand demand;
	const double housingRatio = (population > 0)
		? static_cast<double>(snapshot.housingCapacity) / population : 1.0;
	demand.residential = Clamp(1.25 - housingRatio, 0.10, 1.0);
	const double desiredCommercial = Max(1.0, population / 500.0);
	demand.commercial = Clamp((desiredCommercial - snapshot.commercialBuildings) / desiredCommercial,
		0.10, 1.0);
	const double desiredIndustrial = Max(1.0, population / 1200.0);
	demand.industrial = Clamp((desiredIndustrial - snapshot.industrialBuildings) / desiredIndustrial,
		0.10, 1.0);
	return demand;
}

// ===== 座標変換 =====

std::pair<Point, Point> ZoneManager::worldToCell(Vec3 worldPos)
{
	const int chunkX = static_cast<int>(Math::Floor(worldPos.x / CHUNK_SIZE));
	const int chunkY = static_cast<int>(Math::Floor(worldPos.z / CHUNK_SIZE));

	const float lx = static_cast<float>(worldPos.x - chunkX * CHUNK_SIZE);
	const float lz = static_cast<float>(worldPos.z - chunkY * CHUNK_SIZE);

	const int cellX = Clamp(static_cast<int>(lx / kCellSize), 0, ZONE_CELLS - 1);
	const int cellY = Clamp(static_cast<int>(lz / kCellSize), 0, ZONE_CELLS - 1);

	return { { chunkX, chunkY }, { cellX, cellY } };
}

Vec3 ZoneManager::cellToWorld(Point chunkCoord, Point cellCoord)
{
	const double ox = static_cast<double>(chunkCoord.x) * CHUNK_SIZE;
	const double oz = static_cast<double>(chunkCoord.y) * CHUNK_SIZE;
	const double cx = ox + (cellCoord.x + 0.5) * kCellSize;
	const double cz = oz + (cellCoord.y + 0.5) * kCellSize;
	return Vec3{ cx, 0.0, cz };
}

// ===== ゾーン操作 =====

void ZoneManager::paintZone(World& world, Vec3 worldPos, ZoneType zone, int brushRadius)
{
	// 円形ブラシで近傍セルを書き換え、チャンク境界をまたぐ塗りも同じ手順で処理する。
	const auto [chunkCoord, centerCell] = worldToCell(worldPos);

	for (int dy = -brushRadius; dy <= brushRadius; ++dy)
	{
		for (int dx = -brushRadius; dx <= brushRadius; ++dx)
		{
			// ブラシ形状: 円形（チェビシェフ距離ではなくユークリッド距離）
			if (dx * dx + dy * dy > brushRadius * brushRadius + brushRadius) continue;

			int cx = centerCell.x + dx;
			int cy = centerCell.y + dy;

			// チャンク境界を跨ぐ場合は隣チャンクへ転送
			Point cc = chunkCoord;
			if (cx < 0) { cx += ZONE_CELLS; --cc.x; }
			else if (cx >= ZONE_CELLS) { cx -= ZONE_CELLS; ++cc.x; }
			if (cy < 0) { cy += ZONE_CELLS; --cc.y; }
			else if (cy >= ZONE_CELLS) { cy -= ZONE_CELLS; ++cc.y; }

			Chunk* chunk = world.getChunk(cc);
			if (!chunk) continue;
			chunk->zoneMap[{ cx, cy }] = zone;
		}
	}
}

void ZoneManager::paintZoneRect(World& world, Vec3 a, Vec3 b, ZoneType zone)
{
	// 矩形指定はワールド座標をセル範囲へ落とし込み、対象セルをまとめて更新する。
	const double minX = Min(a.x, b.x);
	const double maxX = Max(a.x, b.x);
	const double minZ = Min(a.z, b.z);
	const double maxZ = Max(a.z, b.z);

	// セル単位に整列させて塗る
	const int startCellGX = static_cast<int>(Math::Floor(minX / kCellSize));
	const int endCellGX   = static_cast<int>(Math::Floor(maxX / kCellSize));
	const int startCellGY = static_cast<int>(Math::Floor(minZ / kCellSize));
	const int endCellGY   = static_cast<int>(Math::Floor(maxZ / kCellSize));

	for (int gx = startCellGX; gx <= endCellGX; ++gx)
	{
		for (int gy = startCellGY; gy <= endCellGY; ++gy)
		{
			const int chunkX = static_cast<int>(Math::Floor(static_cast<double>(gx) / ZONE_CELLS));
			const int chunkY = static_cast<int>(Math::Floor(static_cast<double>(gy) / ZONE_CELLS));
			int cx = gx - chunkX * ZONE_CELLS;
			int cy = gy - chunkY * ZONE_CELLS;

			Chunk* chunk = world.getChunk({ chunkX, chunkY });
			if (!chunk) continue;
			chunk->zoneMap[{ cx, cy }] = zone;
		}
	}
}

ZoneType ZoneManager::getZone(const World& world, Vec3 worldPos) const
{
	const auto [chunkCoord, cell] = worldToCell(worldPos);
	const Chunk* chunk = world.getChunk(chunkCoord);
	if (!chunk) return ZoneType::Unzoned;
	return chunk->zoneMap[{ cell.x, cell.y }];
}

// ===== 発展スコア =====

float ZoneManager::calcDevelopmentScore(Point chunkCoord, int cx, int cy,
                                        const RoadNetwork& network) const
{
	// 発展可否は道路への近さだけを簡潔なスコアへ畳み込み、上位ロジックから使いやすくする。
	const Vec3 center = cellToWorld(chunkCoord, { cx, cy });
	const auto info = nearestRoadInfo(
		static_cast<float>(center.x), static_cast<float>(center.z), network);
	return scoreFromDist(info.dist);
}

// ===== 建物生成 =====

Building ZoneManager::spawnBuilding(ZoneType zone, double gameNow) const
{
	// ゾーン種別ごとの代表建物をここで決め、建築時刻だけ載せた初期 Building を返す。
	Building b;
	b.builtAt = gameNow;

	switch (zone)
	{
	case ZoneType::LowResidential:
		b.type = BuildingType::Detached;
		break;
	case ZoneType::Residential:
		b.type = (Random() < 0.70) ? BuildingType::Detached : BuildingType::LowApartment;
		break;
	case ZoneType::Commercial:
		b.type = (Random() < 0.70) ? BuildingType::Shop : BuildingType::Office;
		break;
	case ZoneType::Industrial:
		b.type = BuildingType::Factory;
		break;
	case ZoneType::Agriculture:
	case ZoneType::UrbanControl:
		b.type = BuildingType::Farmland;
		break;
	default:
		b.type = BuildingType::None;
		break;
	}
	return b;
}

Building ZoneManager::spawnBuildingDeterministic(ZoneType zone, GameTime gameNow, uint32 roll) const
{
	Building building;
	building.builtAt = gameNow;
	switch (zone)
	{
	case ZoneType::LowResidential:
		building.type = BuildingType::Detached;
		break;
	case ZoneType::Residential:
		building.type = ((roll % 100) < 70) ? BuildingType::Detached : BuildingType::LowApartment;
		break;
	case ZoneType::Commercial:
		building.type = ((roll % 100) < 65) ? BuildingType::Shop : BuildingType::Office;
		break;
	case ZoneType::Industrial:
		building.type = BuildingType::Factory;
		break;
	case ZoneType::Agriculture:
	case ZoneType::UrbanControl:
		building.type = BuildingType::Farmland;
		break;
	default:
		building.type = BuildingType::None;
		break;
	}
	return building;
}

// ===== 統計 =====

int ZoneManager::totalHousingCapacity(const World& world) const
{
	int total = 0;
	for (int chunkY = 0; chunkY < WORLD_CHUNKS; ++chunkY)
	{
		for (int chunkX = 0; chunkX < WORLD_CHUNKS; ++chunkX)
		{
			const Chunk* chunk = world.getChunk(Point{ chunkX, chunkY });
			if (!chunk) continue;
			for (int row = 0; row < ZONE_CELLS; ++row)
			{
				for (int col = 0; col < ZONE_CELLS; ++col)
				{
					total += buildingCapacity(chunk->buildingGrid[{ col, row }].type);
				}
			}
		}
	}
	return total;
}

ZoneMonthlyUpdateResult ZoneManager::updateMonthly(World& world, const RoadNetwork& network,
	const ZoneDevelopmentDemand& demand, GameTime gameNow, int64 monthIndex) const
{
	ZoneMonthlyUpdateResult result;
	constexpr int kMaximumSpawnsPerMonth = 64;
	constexpr int kMaximumUpgradesPerMonth = 24;

	for (int chunkY = 0; chunkY < WORLD_CHUNKS; ++chunkY)
	{
		for (int chunkX = 0; chunkX < WORLD_CHUNKS; ++chunkX)
		{
			Chunk* chunk = world.getChunk(Point{ chunkX, chunkY });
			if (!chunk) continue;
			bool changed = false;
			for (int row = 0; row < ZONE_CELLS; ++row)
			{
				for (int col = 0; col < ZONE_CELLS; ++col)
				{
					const ZoneType zone = chunk->zoneMap[{ col, row }];
					if (zone == ZoneType::Unzoned) continue;
					const int globalCol = chunkX * ZONE_CELLS + col;
					const int globalRow = chunkY * ZONE_CELLS + row;
					if ((globalCol & 3) != 0 || (globalRow & 3) != 0) continue;
					const uint32 hash = static_cast<uint32>(globalCol) * 73856093u
						^ static_cast<uint32>(globalRow) * 19349663u
						^ static_cast<uint32>(monthIndex) * 83492791u;

					double zoneDemand = 0.0;
					if (zone == ZoneType::LowResidential || zone == ZoneType::Residential)
						zoneDemand = demand.residential;
					else if (zone == ZoneType::Commercial)
						zoneDemand = demand.commercial;
					else if (zone == ZoneType::Industrial)
						zoneDemand = demand.industrial;
					else if (zone == ZoneType::Agriculture || zone == ZoneType::UrbanControl)
						zoneDemand = 0.35;
					if (zoneDemand <= 0.0) continue;

					Building& building = chunk->buildingGrid[{ col, row }];
					const float developmentScore = calcDevelopmentScore(
						Point{ chunkX, chunkY }, col, row, network);
					const double score = developmentScore * Clamp(zoneDemand, 0.0, 1.0);
					const double roll = static_cast<double>(hash % 10000u) / 10000.0;

					if (building.type == BuildingType::None
						&& result.spawnedBuildings < kMaximumSpawnsPerMonth
						&& score >= kSpawnThreshold && roll < score * 0.10)
					{
						building = spawnBuildingDeterministic(zone, gameNow, hash);
						const Vec3 center = cellToWorld(Point{ chunkX, chunkY }, Point{ col, row });
						const RoadInfo road = nearestRoadInfo(
							static_cast<float>(center.x), static_cast<float>(center.z), network);
						building.angle = road.angle;
						building.edgeId = road.edgeId;
						building.edgeT = road.edgeT;
						++result.spawnedBuildings;
						changed = true;
					}
					else if (result.upgradedBuildings < kMaximumUpgradesPerMonth
						&& score >= 0.85 && roll < score * 0.04)
					{
						BuildingType upgraded = building.type;
						if (zone == ZoneType::Residential && building.type == BuildingType::Detached)
							upgraded = BuildingType::LowApartment;
						else if (zone == ZoneType::Residential && building.type == BuildingType::LowApartment)
							upgraded = BuildingType::MidApartment;
						else if (zone == ZoneType::Commercial && building.type == BuildingType::Detached)
							upgraded = BuildingType::MidApartment;
						else if (zone == ZoneType::Commercial && building.type == BuildingType::MidApartment)
							upgraded = BuildingType::HighApartment;
						if (upgraded != building.type)
						{
							building.type = upgraded;
							building.builtAt = gameNow;
							++result.upgradedBuildings;
							changed = true;
						}
					}
				}
			}
			if (changed) chunk->meshDirty = true;
		}
	}
	return result;
}

// ===== オーバーレイ描画 =====

void ZoneManager::renderOverlay(const World& world) const
{
	if (!showOverlay) return;

	// ゾーン確認用の半透明ボックスを、表示中のアクティブチャンクだけに重ねて描く。
	constexpr float cellSz = kCellSize;
	constexpr float lift   = 0.20f;   // 地面から浮かせる高さ [m]
	constexpr float height = 0.40f;   // Box の高さ [m]

	for (const Chunk* chunk : world.getActiveChunks())
	{
		if (!chunk) continue;
		const Vec3 origin = chunk->worldOrigin();

		for (int cy = 0; cy < ZONE_CELLS; ++cy)
		{
			for (int cx = 0; cx < ZONE_CELLS; ++cx)
			{
				const ZoneType zone = chunk->zoneMap[{ cx, cy }];
				if (zone == ZoneType::Unzoned) continue;

				const float wx = static_cast<float>(origin.x) + (cx + 0.5f) * cellSz;
				const float wz = static_cast<float>(origin.z) + (cy + 0.5f) * cellSz;
				const float wy = world.sampleHeight(wx, wz) + lift + height * 0.5f;

				const ColorF col = zoneColor(zone).withAlpha(0.55);
				Box{ wx, wy, wz, cellSz - 0.5f, height, cellSz - 0.5f }
					.draw(col.removeSRGBCurve());
			}
		}
	}
}
