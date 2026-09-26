#pragma once
#include "GenerationSettings.hpp"
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

	/// @brief Clip to the near side of a boundary between two site centers.
	/// @param gapRatio Negative uses the configured gap; zero retains the full half-plane.
	inline Array<Vec2> clipCloserTo(Array<Vec2> polygon, Vec2 center, Vec2 neighbor, double gapRatio = -1.0)
	{
		Array<Vec2> result;
		const Vec2 normal = neighbor - center;
		if (normal.lengthSq() < 0.001) { return polygon; }
		const Vec2 midpoint = (center + neighbor) * 0.5;
		const double gap = normal.length() * (gapRatio >= 0.0 ? gapRatio : GenerationSettings::get().parcels_neighborGapRatio);
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

	/// @brief 大きな駐車場と隣家の間は建物中心の中点でなく、両敷地の間の空地で分ける。
	/// @param preserveFootprint 建物の占有範囲の外側で境界を分ける。
	inline Array<Vec2> clipBetweenSites(Array<Vec2> polygon, Vec2 center, const Building& owner,
		Vec2 neighbor, const Building& adjacent, bool preserveFootprint = false)
	{
		if (!preserveFootprint && !isCompleteSiteBuilding(owner.type) && !isCompleteSiteBuilding(adjacent.type))
		{
			return clipCloserTo(std::move(polygon), center, neighbor);
		}
		const Vec2 ownerAxis{Cos(owner.angle), Sin(owner.angle)};
		const Vec2 neighborAxis{Cos(adjacent.angle), Sin(adjacent.angle)};
		const Vec2 ownerSide{-ownerAxis.y, ownerAxis.x}, neighborSide{-neighborAxis.y, neighborAxis.x};
		double bestGap = -Math::Inf;
		Vec2 bestNormal{0, 0}, boundary{0, 0};
		for (Vec2 normal : {ownerAxis, ownerSide, neighborAxis, neighborSide})
		{
			if (normal.dot(neighbor-center) < 0) { normal = -normal; }
			const double ownerReach = buildingFootprintXZ(owner.type)*.5*(Abs(normal.dot(ownerAxis))+Abs(normal.dot(ownerSide)));
			const double neighborReach = buildingFootprintXZ(adjacent.type)*.5*(Abs(normal.dot(neighborAxis))+Abs(normal.dot(neighborSide)));
			const double gap = normal.dot(neighbor-center)-ownerReach-neighborReach;
			if (gap > bestGap)
			{
				bestGap = gap;
				bestNormal = normal;
				boundary = center+normal*(ownerReach+gap*.5);
			}
		}
		if (preserveFootprint && bestGap >= 0.0)
		{
			// Split the free space between footprints without trimming either building's lot.
			return clipCloserTo(std::move(polygon), boundary-bestNormal, boundary+bestNormal, 0.0);
		}
		if (bestGap < GenerationSettings::get().parcels_minimumBuildingGap) { return clipCloserTo(std::move(polygon), center, neighbor); }
		return clipCloserTo(std::move(polygon), boundary-bestNormal, boundary+bestNormal);
	}


	/// @brief 隣接する建物との境界で敷地を分割する。
	/// @param preserveFootprint 建物の四隅が敷地から削られないようにする。
	inline Array<Vec2> partition(const World& world, Point coord, int col, int row,
		Vec2 center, Array<Vec2> polygon, bool preserveFootprint = false)
	{
		constexpr double kCellSize = static_cast<double>(CHUNK_SIZE) / ZONE_CELLS;
		const int globalCol = coord.x * ZONE_CELLS + col;
		const int globalRow = coord.y * ZONE_CELLS + row;
		const Building& owner = world.getChunk(coord)->buildingGrid[{col, row}];
		const int range=Max(5,static_cast<int>(Ceil((buildingFootprintXZ(owner.type)+maximumBuildingFootprint())/kCellSize))+2);
		for (int dz = -range; dz <= range; ++dz)
		{
			for (int dx = -range; dx <= range; ++dx)
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
				polygon = clipBetweenSites(std::move(polygon), center, owner, neighbor, building, preserveFootprint);
				if (polygon.size() < 3) { return {}; }
			}
		}
		normalize(polygon);
		return polygon;
	}
}
