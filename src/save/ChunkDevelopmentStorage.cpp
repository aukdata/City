#include "ChunkDevelopmentStorage.hpp"
#include <cmath>

namespace
{
	constexpr uint32 kMagic = 0x56454443; // CDEV
	constexpr uint16 kVersion = 2;
	constexpr int kCellCount = ZONE_CELLS * ZONE_CELLS;

	/// @brief Check serialized building values before publishing or restoring a snapshot.
	bool validBuilding(const Building& building)
	{
		return static_cast<uint8>(building.type) < static_cast<uint8>(BuildingType::Count)
			&& std::isfinite(building.builtAt) && std::isfinite(building.angle)
			&& std::isfinite(building.edgeT) && std::isfinite(building.offsetX)
			&& std::isfinite(building.offsetZ) && building.edgeId >= -1;
	}

	/// @brief Retain polygon vertices at their original precision.
	bool validLandPatch(const LandPatch& patch)
	{
		if (static_cast<uint8>(patch.type) > static_cast<uint8>(LandPatchType::IrrigationDitch)
			|| !std::isfinite(patch.elevationOffset) || patch.polygon.size() > 100000) { return false; }
		for (const auto& point : patch.polygon)
		{
			if (!std::isfinite(point.x) || !std::isfinite(point.y)) { return false; }
		}
		return true;
	}
}

bool ChunkDevelopmentStorage::write(const FilePath& path, const Chunk& chunk)
{
	if (chunk.zoneMap.width() != ZONE_CELLS || chunk.zoneMap.height() != ZONE_CELLS
		|| chunk.buildingGrid.width() != ZONE_CELLS || chunk.buildingGrid.height() != ZONE_CELLS) { return false; }
	if (chunk.landPatches.size() > 100000) { return false; }
	for (const auto& patch : chunk.landPatches) { if (!validLandPatch(patch)) { return false; } }
	uint32 count = 0;
	for (int index = 0; index < kCellCount; ++index)
	{
		const Point cell{index % ZONE_CELLS, index / ZONE_CELLS};
		if (static_cast<uint8>(chunk.zoneMap[cell]) > static_cast<uint8>(ZoneType::Agriculture)
			|| !validBuilding(chunk.buildingGrid[cell])) { return false; }
		if (chunk.buildingGrid[cell].type != BuildingType::None) { ++count; }
	}
	{
		BinaryWriter writer{path};
		if (!writer) { return false; }
		writer.write(kMagic); writer.write(kVersion);
		writer.write(static_cast<int32>(chunk.coord.x)); writer.write(static_cast<int32>(chunk.coord.y));
		writer.write(static_cast<uint8>(chunk.isUrbanizationArea));
		static_assert(sizeof(ZoneType) == 1);
		writer.write(chunk.zoneMap.data(), kCellCount);
		writer.write(count);
		for (int index = 0; index < kCellCount; ++index)
		{
			const auto& building = chunk.buildingGrid[{index % ZONE_CELLS, index / ZONE_CELLS}];
			if (building.type == BuildingType::None) { continue; }
			writer.write(static_cast<uint16>(index)); writer.write(static_cast<uint8>(building.type));
			writer.write(building.builtAt); writer.write(building.angle); writer.write(building.edgeId);
			writer.write(building.edgeT); writer.write(building.offsetX); writer.write(building.offsetZ);
		}

		writer.write(static_cast<uint32>(chunk.landPatches.size()));
		for (const auto& patch : chunk.landPatches)
		{
			writer.write(static_cast<int32>(patch.id)); writer.write(static_cast<uint8>(patch.type));
			writer.write(patch.elevationOffset); writer.write(patch.materialVariant); writer.write(patch.sourceParcelKey);
			writer.write(static_cast<uint32>(patch.polygon.size()));
			for (const auto& point : patch.polygon) { writer.write(point.x); writer.write(point.y); }
		}
	}
	Chunk verification{chunk.coord};
	return read(path, verification);
}

bool ChunkDevelopmentStorage::read(const FilePath& path, Chunk& chunk)
{
	BinaryReader reader{path};
	uint32 magic = 0, count = 0; uint16 version = 0; int32 x = 0, y = 0; uint8 urban = 0;
	if (!reader || !reader.read(magic) || magic != kMagic || !reader.read(version) || version != kVersion
		|| !reader.read(x) || !reader.read(y) || Point{x, y} != chunk.coord
		|| !reader.read(urban) || urban > 1) { return false; }
	Grid<ZoneType> zones{ZONE_CELLS, ZONE_CELLS, ZoneType::Unzoned};
	Grid<Building> buildings{ZONE_CELLS, ZONE_CELLS, Building{}};
	if (reader.read(zones.data(), kCellCount) != kCellCount || !reader.read(count) || count > kCellCount) { return false; }
	for (int index = 0; index < kCellCount; ++index)
	{
		if (static_cast<uint8>(zones[{index % ZONE_CELLS, index / ZONE_CELLS}]) > static_cast<uint8>(ZoneType::Agriculture)) { return false; }
	}
	for (uint32 record = 0; record < count; ++record)
	{
		uint16 index = 0; uint8 type = 0; Building building;
		if (!reader.read(index) || index >= kCellCount || !reader.read(type) || type == 0
			|| type >= static_cast<uint8>(BuildingType::Count)
			|| !reader.read(building.builtAt) || !reader.read(building.angle) || !reader.read(building.edgeId)
			|| !reader.read(building.edgeT) || !reader.read(building.offsetX) || !reader.read(building.offsetZ)) { return false; }
		building.type = static_cast<BuildingType>(type);
		const Point cell{index % ZONE_CELLS, index / ZONE_CELLS};
		if (!validBuilding(building) || buildings[cell].type != BuildingType::None) { return false; }
		buildings[cell] = building;
	}

	uint32 patchCount = 0;
	if (!reader.read(patchCount) || patchCount > 100000) { return false; }
	Array<LandPatch> patches;
	for (uint32 record = 0; record < patchCount; ++record)
	{
		LandPatch patch; int32 id = 0; uint8 type = 0; uint32 pointCount = 0;
		if (!reader.read(id) || !reader.read(type) || !reader.read(patch.elevationOffset)
			|| !reader.read(patch.materialVariant) || !reader.read(patch.sourceParcelKey)
			|| !reader.read(pointCount) || pointCount > 100000) { return false; }
		patch.id = id; patch.type = static_cast<LandPatchType>(type);
		for (uint32 point = 0; point < pointCount; ++point)
		{
			double vertexX = 0, vertexY = 0;
			if (!reader.read(vertexX) || !reader.read(vertexY)) { return false; }
			patch.polygon << Vec2{vertexX, vertexY};
		}
		if (!validLandPatch(patch)) { return false; }
		patches << std::move(patch);
	}
	uint8 trailing = 0;
	if (reader.read(trailing)) { return false; }
	chunk.zoneMap = std::move(zones); chunk.buildingGrid = std::move(buildings);
	chunk.landPatches = std::move(patches);
	chunk.isUrbanizationArea = urban != 0; chunk.markMeshDirty();
	return true;
}
