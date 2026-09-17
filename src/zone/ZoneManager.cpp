
#include "ZoneManager.hpp"
#include <cmath>

// ===== 定数 =====

namespace
{
	constexpr float kCellSize       = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;  // 16.0f [m]

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
			paintCell(*chunk, {cx, cy}, zone);
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
			paintCell(*chunk, {cx, cy}, zone);
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
