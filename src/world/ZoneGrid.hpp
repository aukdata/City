#pragma once
#include "Chunk.hpp"

/// @brief 建物・敷地で共用するチャンク/セル座標の変換。
namespace ZoneGrid
{
	inline int64 zoneCellKey(Point cc, int col, int row)
	{
		return (chunkCoordToKey(cc) << 16) ^ (static_cast<int64>(row) << 8) ^ static_cast<uint32>(col);
	}

	inline void worldToZoneCell(float wx, float wz, Point& outChunk, int& outCol, int& outRow)
	{
		const int chunkX = static_cast<int>(Math::Floor(wx / CHUNK_SIZE));
		const int chunkZ = static_cast<int>(Math::Floor(wz / CHUNK_SIZE));
		const float lx = wx - static_cast<float>(chunkX * CHUNK_SIZE);
		const float lz = wz - static_cast<float>(chunkZ * CHUNK_SIZE);
		outChunk = Point{ chunkX, chunkZ };
		outCol = Clamp(static_cast<int>(lx / (static_cast<float>(CHUNK_SIZE) / ZONE_CELLS)), 0, ZONE_CELLS - 1);
		outRow = Clamp(static_cast<int>(lz / (static_cast<float>(CHUNK_SIZE) / ZONE_CELLS)), 0, ZONE_CELLS - 1);
	}

	inline Vec2 cellCenterXZ(Point cc, int col, int row)
	{
		constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
		return Vec2{
			static_cast<float>(cc.x * CHUNK_SIZE) + (col + 0.5f) * cellSize,
			static_cast<float>(cc.y * CHUNK_SIZE) + (row + 0.5f) * cellSize
		};
	}

}
