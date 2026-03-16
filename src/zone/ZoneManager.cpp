
#include "ZoneManager.hpp"

// ===== 定数 =====

namespace
{
	constexpr float kCellSize       = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;  // 16.0f [m]
	constexpr float kSpawnThreshold = 0.30f;  ///< 建物生成に必要な最低スコア
	constexpr float kGrowThreshold  = 0.60f;  ///< 成長に必要なスコア
	constexpr float kDecayThreshold = 0.10f;  ///< 衰退・撤去が起きるスコア上限
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

			Chunk& chunk = world.getOrCreateChunk(cc);
			chunk.zoneMap[{ cx, cy }] = zone;
		}
	}
}

void ZoneManager::paintZoneRect(World& world, Vec3 a, Vec3 b, ZoneType zone)
{
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

			Chunk& chunk = world.getOrCreateChunk({ chunkX, chunkY });
			chunk.zoneMap[{ cx, cy }] = zone;
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
	const Vec3 center = cellToWorld(chunkCoord, { cx, cy });

	// 最寄り道路ノードまでの距離を計算する
	float minDistSq = 1e12f;
	for (const auto& node : network.nodes())
	{
		if (node.id == -1) continue;
		const float dx = static_cast<float>(node.position.x - center.x);
		const float dz = static_cast<float>(node.position.z - center.z);
		const float dSq = dx * dx + dz * dz;
		if (dSq < minDistSq) minDistSq = dSq;
	}

	// 道路アクセス係数（最寄り道路ノードまでの距離）
	constexpr float kNearDist  = 100.0f;   // この距離以内 → アクセス 1.0
	constexpr float kFarDist   = 200.0f;   // この距離以内 → アクセス 0.3
	const float dist = Math::Sqrt(minDistSq);
	float access;
	if      (dist < kNearDist) access = 1.0f;
	else if (dist < kFarDist)  access = 0.3f + (kFarDist - dist) / (kFarDist - kNearDist) * 0.7f;
	else                       access = 0.0f;

	return access;
}

// ===== 建物生成 =====

Building ZoneManager::spawnBuilding(ZoneType zone, double gameNow) const
{
	Building b;
	b.stage   = 0;
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
		b.type = BuildingType::Shop;
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

int ZoneManager::maxStage(BuildingType t, [[maybe_unused]] ZoneType zone)
{
	switch (t)
	{
	case BuildingType::Detached:      return 2;
	case BuildingType::LowApartment:  return 1;
	case BuildingType::MidApartment:  return 1;
	case BuildingType::Shop:          return 1;
	case BuildingType::Office:        return 1;
	case BuildingType::Factory:       return 1;
	default:                          return 0;
	}
}

bool ZoneManager::tryGrowBuilding(Building& b, ZoneType zone, float score) const
{
	if (score < kGrowThreshold) return false;
	const int ms = maxStage(b.type, zone);
	if (b.stage >= ms) return false;
	b.stage++;
	return true;
}

// ===== 月次更新 =====

void ZoneManager::monthlyUpdate(World& world, const RoadNetwork& network,
                                double gameNow, Economy& economy)
{
	for (Chunk* chunk : world.getActiveChunks())
	{
		if (!chunk) continue;
		const Point cc = chunk->coord;

		for (int cy = 0; cy < ZONE_CELLS; ++cy)
		{
			for (int cx = 0; cx < ZONE_CELLS; ++cx)
			{
				const ZoneType zone = chunk->zoneMap[{ cx, cy }];
				if (zone == ZoneType::Unzoned) continue;

				const float score = calcDevelopmentScore(cc, cx, cy, network);
				Building& b = chunk->buildingGrid[{ cx, cy }];

				if (b.type == BuildingType::None)
				{
					// 空地 → 建物生成
					if (score >= kSpawnThreshold)
					{
						b = spawnBuilding(zone, gameNow);
					}
				}
				else
				{
					if (score < kDecayThreshold)
					{
						// 衰退 → 撤去
						b = Building{};
					}
					else
					{
						// 成長試行
						tryGrowBuilding(b, zone, score);
					}
				}
			}
		}
	}

	// 住宅収容人口から人口を更新する
	const int cap = totalHousingCapacity(world);
	if (cap > 0)
	{
		// 収容人口の 80〜100% に緩やかに近づける（急変を防ぐ）
		const int target = static_cast<int>(cap * 0.90);
		economy.population += (target - economy.population) / 12;  // 1年で収束
		if (economy.population < 100) economy.population = 100;
	}

	// 月次収支を適用する
	economy.applyMonthly(network);
}

// ===== 統計 =====

int ZoneManager::totalHousingCapacity(const World& world) const
{
	int total = 0;
	for (const Chunk* chunk : world.getActiveChunks())
	{
		if (!chunk) continue;
		for (int cy = 0; cy < ZONE_CELLS; ++cy)
		{
			for (int cx = 0; cx < ZONE_CELLS; ++cx)
			{
				const Building& b = chunk->buildingGrid[{ cx, cy }];
				total += buildingCapacity(b.type);
			}
		}
	}
	return total;
}

// ===== オーバーレイ描画 =====

void ZoneManager::renderOverlay(const World& world) const
{
	if (!showOverlay) return;

	constexpr double cellSz = kCellSize;
	constexpr double h      = 0.15;   // わずかに浮かせる高さ [m]
	constexpr double height = 0.30;   // Box の高さ [m]

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

				const ColorF col = zoneColor(zone);
				const double wx = origin.x + (cx + 0.5) * cellSz;
				const double wz = origin.z + (cy + 0.5) * cellSz;

				Box{ wx, h, wz, cellSz - 0.5, height, cellSz - 0.5 }.draw(col);
			}
		}
	}
}
