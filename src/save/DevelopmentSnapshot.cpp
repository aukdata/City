#include "DevelopmentSnapshot.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <exception>
#include <limits>
#include <type_traits>
#include "../world/World.hpp"

namespace
{
	constexpr uint32 kChunkCount = WORLD_CHUNKS * WORLD_CHUNKS;
	constexpr uint32 kCellCount = ZONE_CELLS * ZONE_CELLS;
	constexpr uint32 kMaxPatchesPerChunk = 65'536;
	constexpr uint32 kMaxPatches = 1'000'000;
	constexpr uint32 kMaxVerticesPerPatch = 4'096;
	constexpr uint32 kMaxVertices = 4'000'000;
	constexpr uint64 kMaxFileBytes = 1ULL << 30;
	constexpr uint64 kZoneRecordBytes = 3;
	constexpr uint64 kBuildingRecordBytes = 31;
	constexpr uint64 kPatchHeaderBytes = 25;
	constexpr uint64 kVertexBytes = 16;
	constexpr uint32 kBitsPerByte = 8;
	constexpr ZoneType kDefaultZone = ZoneType::Unzoned;
	// Version 1 の省略値は将来の Building 初期値に依存させない。
	constexpr Building kDefaultBuilding{ BuildingType::None, 0.0, 0.0f, -1, 0.0f, 0.0f, 0.0f };
	static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
	static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
	static_assert(kCellCount <= std::numeric_limits<uint16>::max());

	struct ZoneRecord
	{
		uint16 cell = 0;
		ZoneType zone = kDefaultZone;
	};

	struct BuildingRecord
	{
		uint16 cell = 0;
		Building building = kDefaultBuilding;
	};

	struct ChunkRecord
	{
		Point coord;
		bool isUrbanizationArea = false;
		Array<ZoneRecord> zones;
		Array<BuildingRecord> buildings;
		Array<LandPatch> landPatches;
	};

	template <class Value>
	using ValueBits = std::conditional_t<sizeof(Value) == 1, uint8,
		std::conditional_t<sizeof(Value) == 2, uint16,
		std::conditional_t<sizeof(Value) == 4, uint32, uint64>>>;

	/// @brief パディング・ホストのエンディアンに依存せず、単一の固定幅値を保存する。
	template <class Value>
	bool writeValue(BinaryWriter& writer, Value value)
	{
		const auto bits = std::bit_cast<ValueBits<Value>>(value);
		std::array<uint8, sizeof(Value)> bytes{};
		for (size_t index = 0; index < bytes.size(); ++index)
		{
			bytes[index] = static_cast<uint8>(bits >> (index * kBitsPerByte));
		}
		return writer.write(bytes.data(), static_cast<int64>(bytes.size())) == static_cast<int64>(bytes.size());
	}

	template <class Value>
	bool readValue(BinaryReader& reader, Value& value)
	{
		std::array<uint8, sizeof(Value)> bytes{};
		if (reader.read(bytes.data(), static_cast<int64>(bytes.size())) != static_cast<int64>(bytes.size()))
		{
			return false;
		}
		ValueBits<Value> bits = 0;
		for (size_t index = 0; index < bytes.size(); ++index)
		{
			bits |= static_cast<ValueBits<Value>>(static_cast<ValueBits<Value>>(bytes[index]) << (index * kBitsPerByte));
		}
		value = std::bit_cast<Value>(bits);
		return true;
	}

	template <class... Values>
	bool writeValues(BinaryWriter& writer, Values... values)
	{
		return (writeValue(writer, values) && ...);
	}

	template <class... Values>
	bool readValues(BinaryReader& reader, Values&... values)
	{
		return (readValue(reader, values) && ...);
	}

	template <class Value>
	bool sameBits(Value first, Value second)
	{
		return std::bit_cast<ValueBits<Value>>(first) == std::bit_cast<ValueBits<Value>>(second);
	}

	bool sameBuilding(const Building& first, const Building& second)
	{
		return first.type == second.type && first.edgeId == second.edgeId
			&& sameBits(first.builtAt, second.builtAt) && sameBits(first.angle, second.angle)
			&& sameBits(first.edgeT, second.edgeT) && sameBits(first.offsetX, second.offsetX)
			&& sameBits(first.offsetZ, second.offsetZ);
	}

	bool validZone(ZoneType zone)
	{
		return static_cast<uint8>(zone) <= static_cast<uint8>(ZoneType::Agriculture);
	}

	bool validBuilding(const Building& building)
	{
		return static_cast<uint8>(building.type) < static_cast<uint8>(BuildingType::Count)
			&& building.edgeId >= -1 && std::isfinite(building.builtAt) && std::isfinite(building.angle)
			&& std::isfinite(building.edgeT) && building.edgeT >= 0.0f && building.edgeT <= 1.0f
			&& std::isfinite(building.offsetX) && std::isfinite(building.offsetZ);
	}

	bool validPatch(const LandPatch& patch)
	{
		if (static_cast<uint8>(patch.type) > static_cast<uint8>(LandPatchType::IrrigationDitch)
			|| !std::isfinite(patch.elevationOffset) || patch.polygon.size() > kMaxVerticesPerPatch)
		{
			return false;
		}
		for (const Vec2& vertex : patch.polygon)
		{
			if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y))
			{
				return false;
			}
		}
		return true;
	}

	bool validChunkShape(const Chunk& chunk, Point coord)
	{
		return chunk.coord == coord && chunk.heightMap.size() == Size{ HEIGHT_CELLS + 1, HEIGHT_CELLS + 1 }
			&& chunk.zoneMap.size() == Size{ ZONE_CELLS, ZONE_CELLS }
			&& chunk.buildingGrid.size() == Size{ ZONE_CELLS, ZONE_CELLS };
	}

	bool collectChunks(const World& world, Array<const Chunk*>& chunks)
	{
		chunks.reserve(kChunkCount);
		for (int cy = 0; cy < WORLD_CHUNKS; ++cy)
		{
			for (int cx = 0; cx < WORLD_CHUNKS; ++cx)
			{
				const Point coord{ cx, cy };
				if (const Chunk* chunk = world.getChunk(coord))
				{
					if (!validChunkShape(*chunk, coord))
					{
						return false;
					}
					chunks << chunk;
				}
			}
		}
		return true;
	}

	bool containsBytes(const BinaryReader& reader, uint64 count)
	{
		return reader.getPos() >= 0 && reader.getPos() <= reader.size()
			&& count <= static_cast<uint64>(reader.size() - reader.getPos());
	}

	bool readSnapshot(const FilePath& path, const World& world, Array<ChunkRecord>& records)
	{
		BinaryReader reader{ path };
		if (!reader || reader.size() < 0 || static_cast<uint64>(reader.size()) > kMaxFileBytes)
		{
			return false;
		}
		uint32 magic = 0, chunkSize = 0, chunkCount = 0;
		uint16 version = 0, worldChunks = 0, heightCells = 0, zoneCells = 0;
		if (!readValues(reader, magic, version, worldChunks, chunkSize, heightCells, zoneCells, chunkCount)
			|| magic != DevelopmentSnapshot::kMagic || version != DevelopmentSnapshot::kVersion
			|| worldChunks != WORLD_CHUNKS || chunkSize != CHUNK_SIZE || heightCells != HEIGHT_CELLS
			|| zoneCells != ZONE_CELLS || chunkCount > kChunkCount)
		{
			return false;
		}
		Array<const Chunk*> chunks;
		if (!collectChunks(world, chunks) || chunkCount != chunks.size())
		{
			return false;
		}
		records.reserve(chunkCount);
		std::array<bool, kChunkCount> seenChunks{};
		uint64 totalPatches = 0, totalVertices = 0;
		for (uint32 chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex)
		{
			uint16 cx = 0, cy = 0;
			uint8 urbanization = 0;
			uint32 zoneCount = 0, buildingCount = 0, patchCount = 0;
			if (!readValues(reader, cx, cy, urbanization, zoneCount, buildingCount, patchCount)
				|| cx >= WORLD_CHUNKS || cy >= WORLD_CHUNKS || urbanization > 1
				|| zoneCount > kCellCount || buildingCount > kCellCount || patchCount > kMaxPatchesPerChunk)
			{
				return false;
			}
			const size_t coordIndex = static_cast<size_t>(cy) * WORLD_CHUNKS + cx;
			const Point coord{ cx, cy };
			totalPatches += patchCount;
			if (seenChunks[coordIndex] || !world.getChunk(coord) || totalPatches > kMaxPatches
				|| !containsBytes(reader, zoneCount * kZoneRecordBytes + buildingCount * kBuildingRecordBytes
					+ patchCount * kPatchHeaderBytes))
			{
				return false;
			}
			seenChunks[coordIndex] = true;
			ChunkRecord record;
			record.coord = coord;
			record.isUrbanizationArea = (urbanization != 0);
			record.zones.reserve(zoneCount);
			record.buildings.reserve(buildingCount);
			record.landPatches.reserve(patchCount);
			std::array<bool, kCellCount> seenZones{}, seenBuildings{};
			for (uint32 index = 0; index < zoneCount; ++index)
			{
				ZoneRecord zone;
				uint8 type = 0;
				if (!readValues(reader, zone.cell, type) || zone.cell >= kCellCount || seenZones[zone.cell])
				{
					return false;
				}
				zone.zone = static_cast<ZoneType>(type);
				if (!validZone(zone.zone))
				{
					return false;
				}
				seenZones[zone.cell] = true;
				record.zones << zone;
			}
			for (uint32 index = 0; index < buildingCount; ++index)
			{
				BuildingRecord entry;
				Building& building = entry.building;
				uint8 type = 0;
				if (!readValues(reader, entry.cell, type, building.builtAt, building.angle, building.edgeId,
					building.edgeT, building.offsetX, building.offsetZ) || entry.cell >= kCellCount
					|| seenBuildings[entry.cell])
				{
					return false;
				}
				building.type = static_cast<BuildingType>(type);
				if (!validBuilding(building))
				{
					return false;
				}
				seenBuildings[entry.cell] = true;
				record.buildings << entry;
			}
			for (uint32 index = 0; index < patchCount; ++index)
			{
				LandPatch patch;
				int32 id = 0;
				uint8 type = 0;
				uint32 vertexCount = 0;
				if (!readValues(reader, id, type, patch.elevationOffset, patch.materialVariant,
					patch.sourceParcelKey, vertexCount) || vertexCount > kMaxVerticesPerPatch)
				{
					return false;
				}
				totalVertices += vertexCount;
				if (totalVertices > kMaxVertices || !containsBytes(reader, vertexCount * kVertexBytes))
				{
					return false;
				}
				patch.id = id;
				patch.type = static_cast<LandPatchType>(type);
				patch.polygon.reserve(vertexCount);
				for (uint32 vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
				{
					Vec2 vertex;
					if (!readValues(reader, vertex.x, vertex.y))
					{
						return false;
					}
					patch.polygon << vertex;
				}
				if (!validPatch(patch))
				{
					return false;
				}
				record.landPatches << std::move(patch);
			}
			record.zones.sort_by([](const ZoneRecord& first, const ZoneRecord& second) { return first.cell < second.cell; });
			record.buildings.sort_by([](const BuildingRecord& first, const BuildingRecord& second) { return first.cell < second.cell; });
			records << std::move(record);
		}
		// Count + 一意な有効座標により、地形の生成済みチャンク集合との完全一致を保証する。
		return reader.getPos() == reader.size();
	}

	bool matchesChunk(const ChunkRecord& record, const Chunk& chunk)
	{
		if (record.isUrbanizationArea != chunk.isUrbanizationArea || record.landPatches.size() != chunk.landPatches.size())
		{
			return false;
		}
		size_t zoneIndex = 0, buildingIndex = 0;
		for (uint32 cell = 0; cell < kCellCount; ++cell)
		{
			const ZoneType zone = zoneIndex < record.zones.size() && record.zones[zoneIndex].cell == cell
				? record.zones[zoneIndex++].zone : kDefaultZone;
			const Building& building = buildingIndex < record.buildings.size() && record.buildings[buildingIndex].cell == cell
				? record.buildings[buildingIndex++].building : kDefaultBuilding;
			if (chunk.zoneMap.data()[cell] != zone || !sameBuilding(chunk.buildingGrid.data()[cell], building))
			{
				return false;
			}
		}
		for (size_t index = 0; index < record.landPatches.size(); ++index)
		{
			const LandPatch& first = record.landPatches[index];
			const LandPatch& second = chunk.landPatches[index];
			if (first.id != second.id || first.type != second.type || first.materialVariant != second.materialVariant
				|| first.sourceParcelKey != second.sourceParcelKey || !sameBits(first.elevationOffset, second.elevationOffset)
				|| first.polygon.size() != second.polygon.size())
			{
				return false;
			}
			for (size_t vertexIndex = 0; vertexIndex < first.polygon.size(); ++vertexIndex)
			{
				if (!sameBits(first.polygon[vertexIndex].x, second.polygon[vertexIndex].x)
					|| !sameBits(first.polygon[vertexIndex].y, second.polygon[vertexIndex].y))
				{
					return false;
				}
			}
		}
		return true;
	}

	bool writeSnapshot(const FilePath& path, const World& world)
	{
		Array<const Chunk*> chunks;
		if (!collectChunks(world, chunks))
		{
			return false;
		}
		uint64 totalPatches = 0, totalVertices = 0;
		for (const Chunk* chunk : chunks)
		{
			totalPatches += chunk->landPatches.size();
			if (chunk->landPatches.size() > kMaxPatchesPerChunk || totalPatches > kMaxPatches)
			{
				return false;
			}
			for (uint32 cell = 0; cell < kCellCount; ++cell)
			{
				if (!validZone(chunk->zoneMap.data()[cell]) || !validBuilding(chunk->buildingGrid.data()[cell]))
				{
					return false;
				}
			}
			for (const LandPatch& patch : chunk->landPatches)
			{
				totalVertices += patch.polygon.size();
				if (totalVertices > kMaxVertices || !validPatch(patch))
				{
					return false;
				}
			}
		}
		BinaryWriter writer{ path };
		if (!writer || !writeValues(writer, DevelopmentSnapshot::kMagic, DevelopmentSnapshot::kVersion,
			static_cast<uint16>(WORLD_CHUNKS), static_cast<uint32>(CHUNK_SIZE), static_cast<uint16>(HEIGHT_CELLS),
			static_cast<uint16>(ZONE_CELLS), static_cast<uint32>(chunks.size())))
		{
			return false;
		}
		for (const Chunk* chunk : chunks)
		{
			uint32 zoneCount = 0, buildingCount = 0;
			for (uint32 cell = 0; cell < kCellCount; ++cell)
			{
				zoneCount += (chunk->zoneMap.data()[cell] != kDefaultZone);
				buildingCount += !sameBuilding(chunk->buildingGrid.data()[cell], kDefaultBuilding);
			}
			if (!writeValues(writer, static_cast<uint16>(chunk->coord.x), static_cast<uint16>(chunk->coord.y),
				static_cast<uint8>(chunk->isUrbanizationArea), zoneCount, buildingCount,
				static_cast<uint32>(chunk->landPatches.size())))
			{
				return false;
			}
			for (uint32 cell = 0; cell < kCellCount; ++cell)
			{
				const ZoneType zone = chunk->zoneMap.data()[cell];
				if (zone != kDefaultZone && !writeValues(writer, static_cast<uint16>(cell), static_cast<uint8>(zone)))
				{
					return false;
				}
			}
			for (uint32 cell = 0; cell < kCellCount; ++cell)
			{
				const Building& building = chunk->buildingGrid.data()[cell];
				if (!sameBuilding(building, kDefaultBuilding)
					&& !writeValues(writer, static_cast<uint16>(cell), static_cast<uint8>(building.type),
						building.builtAt, building.angle, building.edgeId, building.edgeT, building.offsetX, building.offsetZ))
				{
					return false;
				}
			}
			for (const LandPatch& patch : chunk->landPatches)
			{
				if (!writeValues(writer, static_cast<int32>(patch.id), static_cast<uint8>(patch.type), patch.elevationOffset,
					patch.materialVariant, patch.sourceParcelKey, static_cast<uint32>(patch.polygon.size())))
				{
					return false;
				}
				for (const Vec2& vertex : patch.polygon)
				{
					if (!writeValues(writer, vertex.x, vertex.y))
					{
						return false;
					}
				}
			}
		}
		writer.flush();
		writer.close();
		// Siv3D の flush/close は成否を返さないため、閉じたファイルを読み直して保証する。
		return DevelopmentSnapshot::matches(path, world);
	}
}

bool DevelopmentSnapshot::write(const FilePath& path, const World& world)
{
	try
	{
		return writeSnapshot(path, world);
	}
	catch (const std::exception&)
	{
		return false;
	}
}

bool DevelopmentSnapshot::read(const FilePath& path, World& world)
{
	try
	{
		Array<ChunkRecord> records;
		if (!readSnapshot(path, world, records))
		{
			return false;
		}
		// ここから先は固定サイズグリッドの代入と配列 swap のみ。追加確保せず一括反映する。
		for (ChunkRecord& record : records)
		{
			Chunk& chunk = *world.getChunk(record.coord);
			chunk.zoneMap.fill(kDefaultZone);
			chunk.buildingGrid.fill(kDefaultBuilding);
			for (const ZoneRecord& zone : record.zones)
			{
				chunk.zoneMap.data()[zone.cell] = zone.zone;
			}
			for (const BuildingRecord& entry : record.buildings)
			{
				chunk.buildingGrid.data()[entry.cell] = entry.building;
			}
			chunk.landPatches.swap(record.landPatches);
			chunk.isUrbanizationArea = record.isUrbanizationArea;
			chunk.meshDirty = true;
		}
		return true;
	}
	catch (const std::exception&)
	{
		return false;
	}
}

bool DevelopmentSnapshot::matches(const FilePath& path, const World& world)
{
	try
	{
		Array<ChunkRecord> records;
		if (!readSnapshot(path, world, records))
		{
			return false;
		}
		for (const ChunkRecord& record : records)
		{
			if (!matchesChunk(record, *world.getChunk(record.coord)))
			{
				return false;
			}
		}
		return true;
	}
	catch (const std::exception&)
	{
		return false;
	}
}
