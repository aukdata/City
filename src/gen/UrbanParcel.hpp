#pragma once
#include "../world/World.hpp"

/// @brief Convex frontage plots partition the block between neighbouring buildings.
namespace UrbanParcel
{
	/// @brief Siv3D requires an open clockwise outer ring, independent of the road side.
	inline void normalize(Array<Vec2>& polygon)
	{
		Array<Vec2> unique;
		for (const Vec2 point : polygon)
		{
			if (unique.isEmpty() || unique.back().distanceFromSq(point) > 1e-12) { unique << point; }
		}
		if (unique.size() > 1 && unique.front().distanceFromSq(unique.back()) <= 1e-12) { unique.pop_back(); }
		if (unique.size() >= 3 && !Geometry2D::IsClockwise(unique)) { unique.reverse(); }
		polygon = std::move(unique);
	}

	inline Array<Vec2> clipCloserTo(Array<Vec2> polygon, Vec2 center, Vec2 neighbor)
	{
		Array<Vec2> result;
		const Vec2 normal = neighbor - center;
		if (normal.lengthSq() < 0.001) { return polygon; }
		const Vec2 midpoint = (center + neighbor) * 0.5;
		const double gap = normal.length() * 0.12;
		for (size_t index = 0; index < polygon.size(); ++index)
		{
			const Vec2 a = polygon[index], b = polygon[(index + 1) % polygon.size()];
			const double da = (a - midpoint).dot(normal) + gap;
			const double db = (b - midpoint).dot(normal) + gap;
			if (da <= 0) { result << a; }
			if ((da < 0) != (db < 0)) { result << a.lerp(b, da / (da - db)); }
		}
		return result;
	}

	inline Array<Vec2> partition(const World& world, Point coord, int col, int row,
		Vec2 center, Array<Vec2> polygon)
	{
		constexpr double kCellSize = static_cast<double>(CHUNK_SIZE) / ZONE_CELLS;
		const int globalCol = coord.x * ZONE_CELLS + col;
		const int globalRow = coord.y * ZONE_CELLS + row;
		for (int dz = -3; dz <= 3; ++dz)
		{
			for (int dx = -3; dx <= 3; ++dx)
			{
				if (dx == 0 && dz == 0) { continue; }
				const int x = globalCol + dx, z = globalRow + dz;
				if (x < 0 || z < 0) { continue; }
				const Chunk* other = world.getChunk(Point{ x / ZONE_CELLS, z / ZONE_CELLS });
				if (!other) { continue; }
				const Building& building = other->buildingGrid[{ x % ZONE_CELLS, z % ZONE_CELLS }];
				if (building.type == BuildingType::None || building.type == BuildingType::Farmland) { continue; }
				const Vec2 neighbor{ (x + 0.5) * kCellSize + building.offsetX,
					(z + 0.5) * kCellSize + building.offsetZ };
				polygon = clipCloserTo(std::move(polygon), center, neighbor);
				if (polygon.size() < 3) { return {}; }
			}
		}
		normalize(polygon);
		return polygon;
	}
}
